/// @file lldp.cc
/// @brief LLDP TLV codec, neighbour table, and the daemon loop.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/lldp.h"

#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_packet.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "einheit/s5/svc.h"
#include "einheit/s5/sys.h"
#include "einheit/s5/util.h"

namespace einheit::s5::lldp {
namespace {

/// 802.1AB TLV type codes.
constexpr std::uint8_t kTlvEnd = 0;
constexpr std::uint8_t kTlvChassisId = 1;
constexpr std::uint8_t kTlvPortId = 2;
constexpr std::uint8_t kTlvTtl = 3;
constexpr std::uint8_t kTlvPortDescription = 4;
constexpr std::uint8_t kTlvSystemName = 5;
constexpr std::uint8_t kTlvSystemDescription = 6;
constexpr std::uint8_t kTlvSystemCapabilities = 7;
constexpr std::uint8_t kTlvManagementAddress = 8;

/// Chassis id subtype 4 = MAC address; port id subtype 5 = interface
/// name. A switch's ports have names an operator recognises, which is
/// the whole reason `show neighbors` is readable.
constexpr std::uint8_t kChassisSubtypeMac = 4;
constexpr std::uint8_t kPortSubtypeIfName = 5;

/// System capability bits (802.1AB Table 8-4).
constexpr std::uint16_t kCapBridge = 0x0004;
constexpr std::uint16_t kCapRouter = 0x0010;

/// The nearest-bridge LLDP multicast group and ethertype.
constexpr std::uint8_t kLldpMulticast[6] = {0x01, 0x80, 0xc2,
                                            0x00, 0x00, 0x0e};
constexpr std::uint16_t kEthertypeLldp = 0x88cc;

/// A string TLV longer than this is truncated rather than trusted.
/// 802.1AB caps these at 255 anyway; the tighter bound is what keeps a
/// neighbour from filling a terminal with one field.
constexpr std::size_t kMaxWireString = 64;

/// Append a TLV header (7-bit type, 9-bit length) plus body.
auto PushTlv(std::vector<std::uint8_t> &out, std::uint8_t type,
             const std::uint8_t *body, std::size_t len) -> void {
  const std::uint16_t header =
      static_cast<std::uint16_t>((type << 9) | (len & 0x1ff));
  out.push_back(static_cast<std::uint8_t>(header >> 8));
  out.push_back(static_cast<std::uint8_t>(header & 0xff));
  out.insert(out.end(), body, body + len);
}

auto PushTlvString(std::vector<std::uint8_t> &out, std::uint8_t type,
                   const std::string &s) -> void {
  const auto len = std::min(s.size(), kMaxWireString);
  PushTlv(out, type, reinterpret_cast<const std::uint8_t *>(s.data()), len);
}

/// aa:bb:cc:dd:ee:ff → six bytes. Anything else yields all-zero, which
/// is a visible-but-harmless chassis id rather than a parse failure
/// halfway through building a frame.
auto MacBytes(const std::string &mac) -> std::array<std::uint8_t, 6> {
  std::array<std::uint8_t, 6> out{};
  unsigned v[6] = {0, 0, 0, 0, 0, 0};
  if (std::sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2],
                  &v[3], &v[4], &v[5]) == 6) {
    for (int i = 0; i < 6; ++i) {
      out[i] = static_cast<std::uint8_t>(v[i] & 0xff);
    }
  }
  return out;
}

auto FormatMac(const std::uint8_t *b, std::size_t len) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < len; ++i) {
    if (i > 0) out += ':';
    out += std::format("{:02x}", b[i]);
  }
  return out;
}

auto Now() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/// Set when a signal asks the daemon to reload or quit. Only
/// sig_atomic_t may be touched from a handler.
volatile sig_atomic_t g_reload = 0;  // NOLINT
volatile sig_atomic_t g_quit = 0;    // NOLINT

extern "C" void OnHup(int) { g_reload = 1; }
extern "C" void OnTerm(int) { g_quit = 1; }

}  // namespace

auto SanitizeWireString(const std::string &raw) -> std::string {
  std::string out;
  out.reserve(std::min(raw.size(), kMaxWireString));
  for (char c : raw) {
    if (out.size() >= kMaxWireString) break;
    const auto u = static_cast<unsigned char>(c);
    // Printable ASCII only. A tab would forge a column in the
    // neighbour table, a newline a whole row, and an ESC would let a
    // neighbour drive the operator's terminal.
    out += (u >= 0x20 && u < 0x7f) ? c : '.';
  }
  return out;
}

auto ConfigPath() -> std::string {
  return svc::RunDir() + "/lldp.conf";
}

auto NeighborsPath() -> std::string {
  return svc::RunDir() + "/lldp-neighbors";
}

auto BuildLldpdu(const Config &cfg, const std::string &port,
                 const std::string &chassis_mac, int ttl)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;

  const auto mac = MacBytes(chassis_mac);
  std::uint8_t chassis[7];
  chassis[0] = kChassisSubtypeMac;
  std::memcpy(chassis + 1, mac.data(), 6);
  PushTlv(out, kTlvChassisId, chassis, sizeof(chassis));

  std::vector<std::uint8_t> port_id;
  port_id.push_back(kPortSubtypeIfName);
  const auto port_len = std::min(port.size(), kMaxWireString);
  port_id.insert(port_id.end(), port.begin(), port.begin() + port_len);
  PushTlv(out, kTlvPortId, port_id.data(), port_id.size());

  const std::uint8_t ttl_body[2] = {
      static_cast<std::uint8_t>((ttl >> 8) & 0xff),
      static_cast<std::uint8_t>(ttl & 0xff)};
  PushTlv(out, kTlvTtl, ttl_body, sizeof(ttl_body));

  PushTlvString(out, kTlvPortDescription, port);
  PushTlvString(out, kTlvSystemName, cfg.system_name);
  PushTlvString(out, kTlvSystemDescription, cfg.system_description);

  // Capabilities: this box is a bridge, and it is a router whenever
  // forwarding is on. Advertising "router" unconditionally would be a
  // lie a neighbour's topology map would act on.
  const std::uint16_t caps = kCapBridge | kCapRouter;
  std::uint16_t enabled = kCapBridge;
  if (util::ReadSysfs("/proc/sys/net/ipv4/ip_forward") == "1") {
    enabled = static_cast<std::uint16_t>(enabled | kCapRouter);
  }
  const std::uint8_t cap_body[4] = {
      static_cast<std::uint8_t>(caps >> 8),
      static_cast<std::uint8_t>(caps & 0xff),
      static_cast<std::uint8_t>(enabled >> 8),
      static_cast<std::uint8_t>(enabled & 0xff)};
  PushTlv(out, kTlvSystemCapabilities, cap_body, sizeof(cap_body));

  if (!cfg.management_address.empty()) {
    in_addr addr{};
    if (::inet_pton(AF_INET, cfg.management_address.c_str(), &addr) == 1) {
      std::vector<std::uint8_t> body;
      // addr string length = 1 subtype byte + 4 address bytes
      body.push_back(5);
      body.push_back(1);  // IPv4
      const auto *raw = reinterpret_cast<const std::uint8_t *>(&addr);
      body.insert(body.end(), raw, raw + 4);
      body.push_back(2);  // interface numbering subtype: ifIndex
      body.insert(body.end(), {0, 0, 0, 0});
      body.push_back(0);  // OID length
      PushTlv(out, kTlvManagementAddress, body.data(), body.size());
    }
  }

  PushTlv(out, kTlvEnd, nullptr, 0);
  return out;
}

auto ParseLldpdu(const std::uint8_t *data, std::size_t len,
                 const std::string &local_port) -> std::optional<Neighbor> {
  Neighbor n;
  n.local_port = local_port;
  bool saw_chassis = false;
  bool saw_port = false;
  std::size_t i = 0;
  while (i + 2 <= len) {
    const std::uint16_t header =
        static_cast<std::uint16_t>((data[i] << 8) | data[i + 1]);
    const std::uint8_t type = static_cast<std::uint8_t>(header >> 9);
    const std::size_t tlv_len = header & 0x1ff;
    i += 2;
    // The length field is the neighbour's claim, not a fact. Anything
    // that would read past the frame ends the parse — what we already
    // decoded stays usable.
    if (i + tlv_len > len) break;
    const std::uint8_t *body = data + i;
    switch (type) {
      case kTlvEnd:
        i = len;
        continue;
      case kTlvChassisId:
        if (tlv_len >= 2) {
          n.chassis_id = body[0] == kChassisSubtypeMac
                             ? FormatMac(body + 1, tlv_len - 1)
                             : SanitizeWireString(std::string(
                                   reinterpret_cast<const char *>(body + 1),
                                   tlv_len - 1));
          saw_chassis = true;
        }
        break;
      case kTlvPortId:
        if (tlv_len >= 2) {
          n.port_id = body[0] == kChassisSubtypeMac
                          ? FormatMac(body + 1, tlv_len - 1)
                          : SanitizeWireString(std::string(
                                reinterpret_cast<const char *>(body + 1),
                                tlv_len - 1));
          saw_port = true;
        }
        break;
      case kTlvTtl:
        if (tlv_len >= 2) {
          n.ttl = (body[0] << 8) | body[1];
        }
        break;
      case kTlvPortDescription:
        n.port_description = SanitizeWireString(
            std::string(reinterpret_cast<const char *>(body), tlv_len));
        break;
      case kTlvSystemName:
        n.system_name = SanitizeWireString(
            std::string(reinterpret_cast<const char *>(body), tlv_len));
        break;
      case kTlvSystemDescription:
        n.system_description = SanitizeWireString(
            std::string(reinterpret_cast<const char *>(body), tlv_len));
        break;
      case kTlvSystemCapabilities:
        if (tlv_len >= 4) {
          const std::uint16_t enabled =
              static_cast<std::uint16_t>((body[2] << 8) | body[3]);
          std::string caps;
          if ((enabled & kCapBridge) != 0) caps += "Bridge ";
          if ((enabled & kCapRouter) != 0) caps += "Router ";
          if (!caps.empty()) caps.pop_back();
          n.capabilities = caps.empty() ? "-" : caps;
        }
        break;
      case kTlvManagementAddress:
        // addr-len subtype addr... — the first byte counts the subtype
        // byte too, so an IPv4 address reads 5.
        if (tlv_len >= 6 && body[0] == 5 && body[1] == 1) {
          char buf[INET_ADDRSTRLEN] = {};
          in_addr addr{};
          std::memcpy(&addr, body + 2, 4);
          if (::inet_ntop(AF_INET, &addr, buf, sizeof(buf)) != nullptr) {
            n.management_address = buf;
          }
        }
        break;
      default:
        break;
    }
    i += tlv_len;
  }
  // A frame with no identity is not a neighbour; recording it would
  // put a blank row in `show neighbors` that nobody can act on.
  if (!saw_chassis || !saw_port) return std::nullopt;
  n.last_seen = Now();
  return n;
}

auto RenderConfig(const Config &cfg) -> std::string {
  std::string out =
      "# einheit s5 LLDP — GENERATED on commit, do not edit.\n";
  out += std::format("enabled {}\n", cfg.enabled ? "true" : "false");
  out += std::format("tx_interval {}\n", cfg.tx_interval);
  out += std::format("system_name {}\n", cfg.system_name);
  out += std::format("system_description {}\n", cfg.system_description);
  // No management address here on purpose: it is whatever the box is
  // reachable on right now, which a DHCP lease can change without any
  // commit. The daemon resolves it at each transmission instead, so a
  // neighbour's topology map never points at an address we gave up.
  for (const auto &p : cfg.ports) {
    out += std::format("port {}\n", p);
  }
  return out;
}

auto ParseConfig(const std::string &text) -> Config {
  Config cfg;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto sp = line.find(' ');
    if (sp == std::string::npos) continue;
    const auto key = line.substr(0, sp);
    const auto value = line.substr(sp + 1);
    if (key == "enabled") {
      cfg.enabled = value == "true";
    } else if (key == "tx_interval") {
      cfg.tx_interval = std::atoi(value.c_str());
    } else if (key == "system_name") {
      cfg.system_name = value;
    } else if (key == "system_description") {
      cfg.system_description = value;
    } else if (key == "port") {
      cfg.ports.push_back(value);
    }
  }
  if (cfg.tx_interval < 1) cfg.tx_interval = 30;
  return cfg;
}

auto ReadConfig() -> std::optional<Config> {
  std::ifstream f(util::FsPath(ConfigPath()));
  if (!f) return std::nullopt;
  std::stringstream ss;
  ss << f.rdbuf();
  return ParseConfig(ss.str());
}

auto RenderNeighbors(const std::vector<Neighbor> &neighbors) -> std::string {
  std::string out;
  for (const auto &n : neighbors) {
    // Every field here has been through SanitizeWireString, so none of
    // them can carry the tab that separates them.
    out += std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", n.local_port,
                       n.chassis_id, n.port_id, n.port_description,
                       n.system_name, n.system_description,
                       n.management_address.empty() ? "-"
                                                    : n.management_address,
                       n.capabilities.empty() ? "-" : n.capabilities,
                       std::format("{} {}", n.ttl, n.last_seen));
  }
  return out;
}

auto ParseNeighbors(const std::string &text) -> std::vector<Neighbor> {
  std::vector<Neighbor> out;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    std::vector<std::string> f;
    std::istringstream ls(line);
    std::string field;
    while (std::getline(ls, field, '\t')) f.push_back(field);
    if (f.size() < 9) continue;
    Neighbor n;
    n.local_port = f[0];
    n.chassis_id = f[1];
    n.port_id = f[2];
    n.port_description = f[3];
    n.system_name = f[4];
    n.system_description = f[5];
    n.management_address = f[6];
    n.capabilities = f[7];
    std::istringstream ts(f[8]);
    ts >> n.ttl >> n.last_seen;
    out.push_back(n);
  }
  return out;
}

auto ReadNeighbors(std::int64_t now) -> std::vector<Neighbor> {
  std::ifstream f(util::FsPath(NeighborsPath()));
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  std::vector<Neighbor> out;
  for (auto &n : ParseNeighbors(ss.str())) {
    // A neighbour that stopped advertising is gone. Listing it because
    // the daemon has not rewritten the file yet would be worse than an
    // empty table: it is a topology map that is quietly wrong.
    if (n.ttl > 0 && now - n.last_seen > n.ttl) continue;
    out.push_back(std::move(n));
  }
  return out;
}

auto RunDaemon() -> int {
  ::signal(SIGHUP, OnHup);
  ::signal(SIGTERM, OnTerm);
  ::signal(SIGINT, OnTerm);
  ::signal(SIGPIPE, SIG_IGN);

  std::map<std::string, Neighbor> neighbors;
  std::vector<int> socks;
  std::vector<std::string> sock_ports;
  Config cfg;
  std::int64_t next_tx = 0;

  const auto close_all = [&socks, &sock_ports]() {
    for (int fd : socks) ::close(fd);
    socks.clear();
    sock_ports.clear();
  };

  const auto reload = [&]() {
    close_all();
    std::ifstream f(ConfigPath());
    std::stringstream ss;
    if (f) ss << f.rdbuf();
    cfg = ParseConfig(ss.str());
    if (!cfg.enabled) return;
    for (const auto &port : cfg.ports) {
      const unsigned index = ::if_nametoindex(port.c_str());
      if (index == 0) continue;
      const int fd = ::socket(AF_PACKET, SOCK_RAW,
                              htons(kEthertypeLldp));
      if (fd < 0) continue;
      sockaddr_ll addr{};
      addr.sll_family = AF_PACKET;
      addr.sll_protocol = htons(kEthertypeLldp);
      addr.sll_ifindex = static_cast<int>(index);
      if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
          0) {
        ::close(fd);
        continue;
      }
      socks.push_back(fd);
      sock_ports.push_back(port);
    }
    // A reload must advertise immediately: the whole point of a commit
    // that changes the system name is that neighbours learn the new
    // one, not that they learn it up to tx_interval seconds later.
    next_tx = 0;
  };

  const auto transmit = [&]() {
    const int ttl = std::min(65535, cfg.tx_interval * 4);
    // Resolved live rather than baked into the generated config: the
    // box's management address can change under a DHCP lease with no
    // commit involved, and an LLDP entry pointing at an address we
    // gave up is worse than none.
    cfg.management_address.clear();
    for (const auto &iface : sys::GetInterfaces()) {
      if (iface.name == "lo" || iface.address.empty()) continue;
      if (iface.address.find(':') != std::string::npos) continue;
      cfg.management_address =
          iface.address.substr(0, iface.address.find('/'));
      if (iface.name == "br0") break;
    }
    for (std::size_t i = 0; i < socks.size(); ++i) {
      const auto &port = sock_ports[i];
      const auto chassis =
          util::ReadSysfs("/sys/class/net/br0/address");
      const auto payload = BuildLldpdu(cfg, port, chassis, ttl);
      std::vector<std::uint8_t> frame;
      frame.insert(frame.end(), kLldpMulticast,
                   kLldpMulticast + sizeof(kLldpMulticast));
      const auto src = MacBytes(
          util::ReadSysfs("/sys/class/net/" + port + "/address"));
      frame.insert(frame.end(), src.begin(), src.end());
      frame.push_back(kEthertypeLldp >> 8);
      frame.push_back(kEthertypeLldp & 0xff);
      frame.insert(frame.end(), payload.begin(), payload.end());
      // Ethernet's 60-byte minimum, without which the NIC pads with
      // whatever was in the buffer.
      while (frame.size() < 60) frame.push_back(0);
      sockaddr_ll dst{};
      dst.sll_family = AF_PACKET;
      dst.sll_protocol = htons(kEthertypeLldp);
      dst.sll_ifindex =
          static_cast<int>(::if_nametoindex(port.c_str()));
      dst.sll_halen = 6;
      std::memcpy(dst.sll_addr, kLldpMulticast, 6);
      (void)::sendto(socks[i], frame.data(), frame.size(), 0,
                     reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    }
  };

  const auto persist = [&]() {
    std::vector<Neighbor> list;
    const auto now = Now();
    for (auto it = neighbors.begin(); it != neighbors.end();) {
      if (it->second.ttl > 0 && now - it->second.last_seen > it->second.ttl) {
        it = neighbors.erase(it);
        continue;
      }
      list.push_back(it->second);
      ++it;
    }
    bool changed = false;
    (void)svc::WriteGenerated(NeighborsPath(), RenderNeighbors(list),
                              &changed);
  };

  svc::EnsureRunDir();
  reload();
  while (g_quit == 0) {
    if (g_reload != 0) {
      g_reload = 0;
      reload();
    }
    const auto now = Now();
    if (now >= next_tx) {
      transmit();
      persist();
      next_tx = now + cfg.tx_interval;
    }
    std::vector<pollfd> fds;
    fds.reserve(socks.size());
    for (int fd : socks) {
      fds.push_back(pollfd{.fd = fd, .events = POLLIN, .revents = 0});
    }
    // Wake at least once a second even with no sockets, so a SIGHUP
    // that turns LLDP back on is noticed promptly.
    const int timeout_ms = 1000;
    const int ready =
        fds.empty() ? (::poll(nullptr, 0, timeout_ms), 0)
                    : ::poll(fds.data(), fds.size(), timeout_ms);
    if (ready <= 0) continue;
    for (std::size_t i = 0; i < fds.size(); ++i) {
      if ((fds[i].revents & POLLIN) == 0) continue;
      std::uint8_t buf[1600];
      const auto got = ::recv(fds[i].fd, buf, sizeof(buf), 0);
      if (got < static_cast<ssize_t>(sizeof(ether_header))) continue;
      const auto payload_len =
          static_cast<std::size_t>(got) - sizeof(ether_header);
      auto parsed = ParseLldpdu(buf + sizeof(ether_header), payload_len,
                                sock_ports[i]);
      if (!parsed) continue;
      // Keyed on (our port, their chassis, their port): one physical
      // neighbour per key, so a re-advertisement refreshes rather than
      // accumulates.
      neighbors[std::format("{}|{}|{}", parsed->local_port,
                            parsed->chassis_id, parsed->port_id)] = *parsed;
      persist();
    }
  }
  close_all();
  return 0;
}

}  // namespace einheit::s5::lldp
