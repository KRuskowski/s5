/// @file dsa.cc
/// @brief Linux DSA interface — sysfs + bridge commands.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/dsa.h"
#include "einheit/s5/util.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

namespace einheit::s5::dsa {
namespace {

namespace fs = std::filesystem;
using util::ReadSysfs;
using util::ReadUint;
using util::RunCmd;

auto Lower(std::string s) -> std::string {
  for (char &c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

auto IsDsaPort(const std::string &name) -> bool {
  auto path = util::FsPath("/sys/class/net/" + name + "/dsa");
  return fs::exists(path) || fs::is_symlink(path);
}

}  // namespace

auto DiscoverPorts() -> std::vector<std::string> {
  std::vector<std::string> ports;
  if (!fs::exists(util::FsPath("/sys/class/net"))) return ports;
  for (const auto &entry :
       fs::directory_iterator(util::FsPath("/sys/class/net"))) {
    auto name = entry.path().filename().string();
    // DSA ports have a "dsa" symlink or start with "lan"/"wan".
    if (IsDsaPort(name) ||
        name.rfind("lan", 0) == 0 ||
        name.rfind("wan", 0) == 0) {
      ports.push_back(name);
    }
  }
  std::sort(ports.begin(), ports.end());
  return ports;
}

auto IsUp(const std::string &iface) -> bool {
  const auto flags = ReadSysfs("/sys/class/net/" + iface + "/flags");
  if (flags.size() < 3 || flags[0] != '0' ||
      (flags[1] != 'x' && flags[1] != 'X')) {
    return false;
  }
  unsigned long value = 0;
  for (std::size_t i = 2; i < flags.size(); ++i) {
    const char c = flags[i];
    unsigned digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<unsigned>(c - 'a') + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<unsigned>(c - 'A') + 10;
    } else {
      return false;
    }
    value = value * 16 + digit;
  }
  // IFF_UP
  return (value & 0x1U) != 0;
}

auto GetPortStatus(const std::string &port) -> PortStatus {
  PortStatus st;
  st.name = port;
  auto base = "/sys/class/net/" + port + "/";
  auto operstate = ReadSysfs(base + "operstate");
  st.link = (operstate == "up");
  st.enabled = IsUp(port);
  st.speed = ReadSysfs(base + "speed");
  if (st.speed == "-1" || st.speed.empty())
    st.speed = "-";
  st.duplex = ReadSysfs(base + "duplex");
  if (st.duplex == "unknown" || st.duplex.empty())
    st.duplex = "-";
  return st;
}

auto GetPortCounters(const std::string &port) -> PortCounters {
  PortCounters c;
  c.name = port;
  auto base = "/sys/class/net/" + port + "/statistics/";
  c.rx_bytes = ReadUint(base + "rx_bytes");
  c.tx_bytes = ReadUint(base + "tx_bytes");
  c.rx_packets = ReadUint(base + "rx_packets");
  c.tx_packets = ReadUint(base + "tx_packets");
  c.rx_errors = ReadUint(base + "rx_errors");
  c.tx_errors = ReadUint(base + "tx_errors");
  return c;
}

auto SetPortEnabled(const std::string &port, bool up) -> bool {
  auto cmd = "ip link set " + port +
             (up ? " up" : " down") + " 2>&1";
  auto out = RunCmd(cmd);
  return out.empty();
}

auto GetVlans() -> std::vector<VlanEntry> {
  std::vector<VlanEntry> vlans;
  auto out = RunCmd("bridge vlan show 2>/dev/null");
  std::istringstream iss(out);
  std::string line;
  std::string current_port;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    // Lines look like:
    //   lan1   100 PVID Egress Untagged
    //          200
    // or:
    //   lan1    100 PVID Egress Untagged
    std::istringstream ls(line);
    std::string first;
    ls >> first;
    std::uint16_t vid = 0;
    if (std::isdigit(first[0])) {
      vid = static_cast<std::uint16_t>(std::stoi(first));
    } else {
      current_port = first;
      std::string vid_str;
      ls >> vid_str;
      if (vid_str.empty() || !std::isdigit(vid_str[0]))
        continue;
      vid = static_cast<std::uint16_t>(std::stoi(vid_str));
    }
    VlanEntry v;
    v.vid = vid;
    v.port = current_port;
    std::string flag;
    while (ls >> flag) {
      if (flag == "PVID") v.pvid = true;
      if (flag == "Untagged") v.untagged = true;
    }
    vlans.push_back(v);
  }
  return vlans;
}

auto AddVlan(const std::string &port, std::uint16_t vid,
             bool untagged, bool pvid) -> bool {
  auto cmd = "bridge vlan add dev " + port +
             " vid " + std::to_string(vid);
  if (untagged) cmd += " untagged";
  if (pvid) cmd += " pvid";
  cmd += " 2>&1";
  return RunCmd(cmd).empty();
}

auto DelVlan(const std::string &port, std::uint16_t vid)
    -> bool {
  auto cmd = "bridge vlan del dev " + port +
             " vid " + std::to_string(vid) + " 2>&1";
  return RunCmd(cmd).empty();
}

auto GetMacTable() -> std::vector<MacEntry> {
  std::vector<MacEntry> macs;
  // Not `fdb show dynamic`: static entries are configuration, and an
  // operator who set one needs to see it in the table they are looking
  // at. The type column tells them apart.
  auto out = RunCmd("bridge fdb show 2>/dev/null");
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    // aa:bb:cc:dd:ee:ff dev lan1 vlan 100 master br0 permanent
    std::istringstream ls(line);
    MacEntry e;
    std::string token;
    bool on_master = false;
    ls >> e.mac;
    while (ls >> token) {
      if (token == "dev") ls >> e.port;
      if (token == "vlan") {
        std::string vid_str;
        ls >> vid_str;
        if (!vid_str.empty())
          e.vid = static_cast<std::uint16_t>(
              std::stoi(vid_str));
      }
      if (token == "master") on_master = true;
      if (token == "permanent" || token == "static") e.is_static = true;
    }
    if (e.mac.empty() || e.port.empty()) continue;
    // `self` entries belong to the netdev's own RX filter, not to the
    // bridge's forwarding database. They are not switch state and are
    // certainly not configuration.
    if (!on_master) continue;
    // The I/G bit of the first octet: multicast/group addresses are
    // kernel-managed (IPv6 all-nodes, IGMP, STP), never operator
    // configuration.
    if (e.mac.size() >= 2) {
      const auto hi = std::stoi(e.mac.substr(0, 2), nullptr, 16);
      e.is_multicast = (hi & 0x01) != 0;
    }
    // The bridge installs a permanent entry for each port's own
    // address. It looks exactly like a configured static entry and is
    // not one — reconciling against it tries to delete kernel state and
    // fails the whole apply.
    e.is_local = !e.port.empty() &&
                 Lower(ReadSysfs("/sys/class/net/" + e.port +
                                 "/address")) == Lower(e.mac);
    macs.push_back(e);
  }
  return macs;
}

// ── Port parameters ─────────────────────────────────────────────

auto GetPortParams(const std::string &port) -> PortParams {
  PortParams p;
  const auto base = "/sys/class/net/" + port + "/";
  const auto mtu = ReadSysfs(base + "mtu");
  p.mtu = mtu.empty() ? 0 : std::atoi(mtu.c_str());
  // Negotiated values come from sysfs; what was *asked for* lives in
  // the config, which is why `show interfaces detail` prints both.
  p.negotiated_speed = ReadSysfs(base + "speed");
  if (p.negotiated_speed == "-1" || p.negotiated_speed.empty()) {
    p.negotiated_speed = "-";
  }
  p.negotiated_duplex = ReadSysfs(base + "duplex");
  if (p.negotiated_duplex == "unknown" || p.negotiated_duplex.empty()) {
    p.negotiated_duplex = "-";
  }
  // Read-back has to produce values the SCHEMA can express, or the
  // reconcile overlay poisons running config with something the next
  // commit then rejects — every subsequent commit fails with a value
  // the operator never typed. So: "auto" unless ethtool positively
  // says autoneg is off AND the negotiated value is one of the
  // configurable ones. A link at 2.5G, or a netdev with no ethtool
  // support at all, reads as "auto" with the truth in the negotiated
  // fields, which is what `show interfaces detail` prints anyway.
  const auto out = RunCmd("ethtool " + port + " 2>/dev/null");
  const bool autoneg_off =
      out.find("Auto-negotiation: off") != std::string::npos;
  p.speed = "auto";
  p.duplex = "auto";
  if (autoneg_off) {
    if (p.negotiated_speed == "10" || p.negotiated_speed == "100" ||
        p.negotiated_speed == "1000") {
      p.speed = p.negotiated_speed;
    }
    if (p.negotiated_duplex == "half" || p.negotiated_duplex == "full") {
      p.duplex = p.negotiated_duplex;
    }
  }
  const auto pause = RunCmd("ethtool -a " + port + " 2>/dev/null");
  p.flow_control = pause.find("RX: on") != std::string::npos ||
                   pause.find("TX: on") != std::string::npos;
  return p;
}

auto SetPortSpeedDuplex(const std::string &port,
                        const std::string &speed,
                        const std::string &duplex) -> bool {
  // Forcing one of the two and auto-negotiating the other is not a
  // thing ethtool accepts, so the backend resolves the pair before it
  // gets here; "auto" in either position means autoneg for both.
  if (speed == "auto" || duplex == "auto") {
    return RunCmd("ethtool -s " + port + " autoneg on 2>&1").empty();
  }
  return RunCmd("ethtool -s " + port + " autoneg off speed " + speed +
                " duplex " + duplex + " 2>&1")
      .empty();
}

auto SetPortMtu(const std::string &port, int mtu) -> bool {
  return RunCmd("ip link set " + port + " mtu " +
                std::to_string(mtu) + " 2>&1")
      .empty();
}

auto SetPortFlowControl(const std::string &port, bool on) -> bool {
  const std::string state = on ? "on" : "off";
  return RunCmd("ethtool -A " + port + " rx " + state + " tx " + state +
                " 2>&1")
      .empty();
}

// ── MAC table ───────────────────────────────────────────────────

auto AddStaticMac(const std::string &mac, const std::string &port,
                  std::uint16_t vid) -> bool {
  // `replace` rather than `add`: re-applying the same configuration
  // must not fail because the entry is already there.
  return RunCmd("bridge fdb replace " + mac + " dev " + port +
                " master static vlan " + std::to_string(vid) + " 2>&1")
      .empty();
}

auto DelStaticMac(const std::string &mac, const std::string &port,
                  std::uint16_t vid) -> bool {
  return RunCmd("bridge fdb del " + mac + " dev " + port +
                " master vlan " + std::to_string(vid) + " 2>&1")
      .empty();
}

auto GetMacAging(const std::string &bridge) -> int {
  // sysfs reports centiseconds; the schema and the CLI talk seconds.
  const auto raw =
      ReadSysfs("/sys/class/net/" + bridge + "/bridge/ageing_time");
  if (raw.empty()) return 0;
  return std::atoi(raw.c_str()) / 100;
}

auto SetMacAging(const std::string &bridge, int seconds) -> bool {
  return RunCmd("ip link set " + bridge + " type bridge ageing_time " +
                std::to_string(seconds * 100) + " 2>&1")
      .empty();
}

auto FlushMacTable(const std::string &port) -> bool {
  // Only dynamic entries: a static entry is configuration, and
  // `clear mac-table` must not quietly delete config.
  const auto cmd = port.empty()
                       ? std::string("bridge fdb flush dev br0 master "
                                     "dynamic 2>&1")
                       : "bridge fdb flush dev " + port +
                             " master dynamic 2>&1";
  return RunCmd(cmd).empty();
}

// ── IGMP snooping ───────────────────────────────────────────────

auto GetSnooping(const std::string &bridge) -> SnoopState {
  SnoopState s;
  const auto base = "/sys/class/net/" + bridge + "/bridge/";
  s.enabled = ReadSysfs(base + "multicast_snooping") == "1";
  s.querier = ReadSysfs(base + "multicast_querier") == "1";
  return s;
}

auto SetSnooping(const std::string &bridge, bool enabled) -> bool {
  return RunCmd("ip link set " + bridge + " type bridge mcast_snooping " +
                (enabled ? "1" : "0") + " 2>&1")
      .empty();
}

auto SetQuerier(const std::string &bridge, bool enabled) -> bool {
  return RunCmd("ip link set " + bridge + " type bridge mcast_querier " +
                (enabled ? "1" : "0") + " 2>&1")
      .empty();
}

auto GetMdb() -> std::vector<MdbEntry> {
  std::vector<MdbEntry> out;
  auto raw = RunCmd("bridge mdb show 2>/dev/null");
  std::istringstream iss(raw);
  std::string line;
  while (std::getline(iss, line)) {
    // dev br0 port lan1 grp 239.1.1.1 permanent vid 10
    std::istringstream ls(line);
    std::string token;
    MdbEntry e;
    while (ls >> token) {
      if (token == "port") ls >> e.port;
      if (token == "grp") ls >> e.group;
      if (token == "vid") {
        std::string vid;
        ls >> vid;
        if (!vid.empty()) {
          e.vid = static_cast<std::uint16_t>(std::stoi(vid));
        }
      }
    }
    if (!e.port.empty() && !e.group.empty()) out.push_back(e);
  }
  return out;
}

}  // namespace einheit::s5::dsa
