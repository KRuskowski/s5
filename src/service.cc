/// @file service.cc
/// @brief Daemon-side execution of s5 product wire commands.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/service.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "einheit/cli/confd/boot_report.h"
#include "einheit/s5/dnsmasq.h"
#include "einheit/s5/dsa.h"
#include "einheit/s5/fabric.h"
#include "einheit/s5/l3.h"
#include "einheit/s5/lldp.h"
#include "einheit/s5/poe.h"
#include "einheit/s5/stp.h"
#include "einheit/s5/svc.h"
#include "einheit/s5/sys.h"
#include "einheit/s5/util.h"

namespace einheit::s5::service {
namespace {

using cli::protocol::Request;
using cli::protocol::Response;
using cli::protocol::ResponseError;
using cli::protocol::ResponseStatus;

/// Response data is line-oriented: one row per line, fields
/// separated by tabs. The adapter's renderer splits it back into
/// semantic table cells. Free-form command output (log, ping) is
/// passed through as raw text.
constexpr char kFieldSep = '\t';

auto Ok(const Request &req, const std::string &body) -> Response {
  Response r;
  r.id = req.id;
  r.status = ResponseStatus::Ok;
  r.data.assign(body.begin(), body.end());
  return r;
}

auto Err(const Request &req, const std::string &code,
         const std::string &message, const std::string &hint = "")
    -> Response {
  Response r;
  r.id = req.id;
  r.status = ResponseStatus::Error;
  r.error = ResponseError{code, message, hint};
  return r;
}

auto Arg(const Request &req, std::size_t idx) -> std::string {
  return idx < req.args.size() ? req.args[idx] : "";
}

auto ParseInt(const std::string &s) -> std::optional<int> {
  if (s.empty()) return std::nullopt;
  errno = 0;
  char *end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0' || errno != 0 ||
      v < INT_MIN || v > INT_MAX) {
    return std::nullopt;
  }
  return static_cast<int>(v);
}

auto ParsePort(const std::string &s) -> std::optional<int> {
  auto v = ParseInt(s);
  if (!v || *v < 1 || *v > 5) return std::nullopt;
  return v;
}

/// Arguments interpolated into RunCmd shell lines (ping hosts,
/// usernames) must never carry shell metacharacters.
auto SafeToken(const std::string &v) -> bool {
  if (v.empty()) return false;
  for (char c : v) {
    const bool ok = std::isalnum(static_cast<unsigned char>(c)) ||
                    c == '.' || c == ':' || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

auto PortsCache() -> std::vector<std::string> & {
  static std::vector<std::string> ports;
  return ports;
}

auto Ports() -> const std::vector<std::string> & {
  // DSA port set is fixed by the device tree, so discover once. Empty
  // is not cached: a box whose ports have not appeared yet must be
  // allowed to find them on the next command rather than insisting
  // forever that it has none.
  auto &ports = PortsCache();
  if (ports.empty()) ports = dsa::DiscoverPorts();
  return ports;
}

/// Ports to serve for a command with an optional port-name filter.
auto FilteredPorts(const Request &req)
    -> std::vector<std::string> {
  const auto filter = Arg(req, 0);
  if (filter.empty()) return Ports();
  std::vector<std::string> out;
  for (const auto &p : Ports()) {
    if (p == filter) out.push_back(p);
  }
  return out;
}

auto Row(std::vector<std::string> fields) -> std::string {
  std::string line;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) line += kFieldSep;
    line += fields[i];
  }
  line += '\n';
  return line;
}

auto ShowInterfaces(const Request &req) -> Response {
  std::string body;
  for (const auto &name : FilteredPorts(req)) {
    const auto st = dsa::GetPortStatus(name);
    body += Row({st.name, st.link ? "up" : "down", st.speed,
                 st.duplex});
  }
  return Ok(req, body);
}

/// Counter baselines for `clear counters`.
///
/// The kernel does not let anything zero a netdev's statistics, so
/// "clear" is a snapshot the reads subtract from. It lives in the
/// service rather than on disk on purpose: counters are operational
/// state, and a reboot genuinely does reset them, so a baseline that
/// outlived the process would under-report after one.
std::map<std::string, dsa::PortCounters> g_counter_base;  // NOLINT

auto SubtractBase(const dsa::PortCounters &c) -> dsa::PortCounters {
  const auto it = g_counter_base.find(c.name);
  if (it == g_counter_base.end()) return c;
  const auto &b = it->second;
  dsa::PortCounters out = c;
  // Saturating: an interface that reset under us (module reload, or a
  // counter wrap) must read as 0, never as an enormous number.
  const auto sub = [](std::uint64_t now, std::uint64_t base) {
    return now >= base ? now - base : 0;
  };
  out.rx_bytes = sub(c.rx_bytes, b.rx_bytes);
  out.tx_bytes = sub(c.tx_bytes, b.tx_bytes);
  out.rx_packets = sub(c.rx_packets, b.rx_packets);
  out.tx_packets = sub(c.tx_packets, b.tx_packets);
  out.rx_errors = sub(c.rx_errors, b.rx_errors);
  out.tx_errors = sub(c.tx_errors, b.tx_errors);
  return out;
}

auto ShowCounters(const Request &req) -> Response {
  std::string body;
  for (const auto &name : FilteredPorts(req)) {
    const auto c = SubtractBase(dsa::GetPortCounters(name));
    body += Row({c.name, std::to_string(c.rx_bytes),
                 std::to_string(c.tx_bytes),
                 std::to_string(c.rx_packets),
                 std::to_string(c.tx_packets),
                 std::to_string(c.rx_errors)});
  }
  return Ok(req, body);
}

auto ClearCounters(const Request &req) -> Response {
  const auto filter = Arg(req, 0);
  if (!filter.empty() && !SafeToken(filter)) {
    return Err(req, "bad-arg", std::format("invalid port '{}'", filter));
  }
  std::size_t cleared = 0;
  for (const auto &name : Ports()) {
    if (!filter.empty() && name != filter) continue;
    g_counter_base[name] = dsa::GetPortCounters(name);
    ++cleared;
  }
  if (cleared == 0) {
    return Err(req, "bad-arg", std::format("no such port '{}'", filter));
  }
  return Ok(req, std::format("counters cleared on {} port(s)\n", cleared));
}

auto ShowMacTable(const Request &req) -> Response {
  std::string body;
  const auto filter = Arg(req, 0);
  for (const auto &e : dsa::GetMacTable()) {
    if (!filter.empty() && e.port != filter) continue;
    // `local` is the port's own address and `multicast` a group the
    // kernel joined: both are permanent but neither is configuration,
    // and calling them "static" would invite an operator to try to
    // delete them.
    const char *type = e.is_local          ? "local"
                       : e.is_multicast    ? "multicast"
                       : e.is_static       ? "static"
                                           : "dynamic";
    body += Row({e.mac, e.port, std::to_string(e.vid), type});
  }
  return Ok(req, body);
}

/// `clear mac-table [port]` — flush LEARNED entries only. A static
/// entry is configuration; an operational verb must not delete config.
auto ClearMacTable(const Request &req) -> Response {
  const auto port = Arg(req, 0);
  if (!port.empty() && !SafeToken(port)) {
    return Err(req, "bad-arg", std::format("invalid port '{}'", port));
  }
  if (!dsa::FlushMacTable(port)) {
    return Err(req, "hw-error", "flushing the MAC table failed");
  }
  return Ok(req, port.empty()
                     ? "learned MAC entries flushed\n"
                     : std::format("learned MAC entries on {} flushed\n",
                                   port));
}

auto ShowIgmpSnooping(const Request &req) -> Response {
  const auto topo = fabric::S5Topology();
  const auto st = dsa::GetSnooping(topo.bridge);
  std::string body;
  body += Row({"snooping", st.enabled ? "enabled" : "disabled"});
  body += Row({"querier", st.querier ? "enabled" : "disabled"});
  for (const auto &e : dsa::GetMdb()) {
    body += Row({std::format("group {}", e.group),
                 std::format("{} vlan {}", e.port, e.vid)});
  }
  return Ok(req, body);
}

/// BPDU-counter baselines for `clear spanning-tree statistics`. mstpd
/// has no counter-reset in its control protocol, so "clear" is the same
/// snapshot-subtract `clear counters` uses, and for the same reason: a
/// restart of mstpd genuinely does zero them, so a baseline that
/// outlived the process would under-report afterwards.
std::map<std::string, stp::PortState> g_stp_base;  // NOLINT

auto SubtractStpBase(const stp::PortState &p) -> stp::PortState {
  const auto it = g_stp_base.find(p.port);
  if (it == g_stp_base.end()) return p;
  const auto &b = it->second;
  const auto sub = [](std::uint64_t now, std::uint64_t base) {
    return now >= base ? now - base : 0;
  };
  stp::PortState out = p;
  out.tx_bpdu = sub(p.tx_bpdu, b.tx_bpdu);
  out.rx_bpdu = sub(p.rx_bpdu, b.rx_bpdu);
  out.tx_tcn = sub(p.tx_tcn, b.tx_tcn);
  out.rx_tcn = sub(p.rx_tcn, b.rx_tcn);
  out.transitions_fwd = sub(p.transitions_fwd, b.transitions_fwd);
  out.transitions_blk = sub(p.transitions_blk, b.transitions_blk);
  return out;
}

/// `show spanning-tree` — the bridge's place in the tree, then every
/// port's role and state. Two row shapes in one blob, tagged in the
/// first field, because "who is root" and "what is each port doing" are
/// one question an operator asks after patching a cable.
auto ShowSpanningTree(const Request &req) -> Response {
  const auto bridge = fabric::S5Topology().bridge;
  const auto st = stp::GetBridgeState(bridge);
  std::string body;
  if (!st.enabled) {
    body += Row({"bridge", "state", "disabled"});
    body += Row({"bridge", "note",
                 stp::Available()
                     ? "set stp.mode rstp to protect against loops"
                     : "mstpd is not installed on this box"});
    return Ok(req, body);
  }
  const bool we_are_root = st.bridge_id == st.root_id;
  body += Row({"bridge", "protocol", st.mode});
  body += Row({"bridge", "bridge id", st.bridge_id});
  body += Row({"bridge", "root id",
               we_are_root ? std::format("{} (this bridge)", st.root_id)
                           : st.root_id});
  body += Row({"bridge", "root port",
               st.root_port.empty() ? "-" : st.root_port});
  body += Row({"bridge", "hello / max age / fwd delay",
               std::format("{} / {} / {}", st.hello, st.max_age,
                           st.forward_delay)});
  body += Row({"bridge", "topology changes",
               std::format("{} (last {}s ago)", st.topology_changes,
                           st.time_since_change)});
  for (const auto &p : stp::GetPortStates(bridge)) {
    body += Row({"port", p.port, p.role, p.state, p.cost,
                 p.oper_edge ? "yes" : "-",
                 p.bpdu_guard_error ? "BLOCKED"
                                    : (p.bpdu_guard ? "on" : "-")});
  }
  return Ok(req, body);
}

/// `show spanning-tree statistics [port]` — BPDU counters per port,
/// baselined by `clear spanning-tree statistics`.
auto ShowSpanningTreeStatistics(const Request &req) -> Response {
  const auto filter = Arg(req, 0);
  std::string body;
  for (const auto &raw : stp::GetPortStates(fabric::S5Topology().bridge)) {
    if (!filter.empty() && raw.port != filter) continue;
    const auto p = SubtractStpBase(raw);
    body += Row({p.port, std::to_string(p.tx_bpdu),
                 std::to_string(p.rx_bpdu), std::to_string(p.tx_tcn),
                 std::to_string(p.rx_tcn),
                 std::to_string(p.transitions_fwd),
                 std::to_string(p.transitions_blk)});
  }
  return Ok(req, body);
}

auto ClearSpanningTreeStatistics(const Request &req) -> Response {
  const auto filter = Arg(req, 0);
  if (!filter.empty() && !SafeToken(filter)) {
    return Err(req, "bad-arg", std::format("invalid port '{}'", filter));
  }
  std::size_t cleared = 0;
  for (const auto &p : stp::GetPortStates(fabric::S5Topology().bridge)) {
    if (!filter.empty() && p.port != filter) continue;
    g_stp_base[p.port] = p;
    ++cleared;
  }
  if (cleared == 0) {
    return Err(req, "bad-arg",
               filter.empty()
                   ? "spanning tree is not running"
                   : std::format("no spanning-tree port '{}'", filter),
               "check `show spanning-tree`");
  }
  return Ok(req, std::format(
                     "spanning-tree statistics cleared on {} port(s)\n",
                     cleared));
}

/// `clear spanning-tree bpdu-guard <port>` — the recovery verb for a
/// port the guard is holding down. Without it a BPDU-guard event needs
/// a reboot or a shell, and the whole point of the guard is that the
/// operator stays in control from the CLI.
auto ClearBpduGuard(const Request &req) -> Response {
  const auto port = Arg(req, 0);
  if (!SafeToken(port)) {
    return Err(req, "bad-arg", "invalid port", "usage: clear "
                                               "spanning-tree bpdu-guard "
                                               "<port>");
  }
  const auto bridge = fabric::S5Topology().bridge;
  const auto ports = stp::GetPortStates(bridge);
  const auto it = std::find_if(ports.begin(), ports.end(),
                               [&port](const auto &p) {
                                 return p.port == port;
                               });
  if (it == ports.end()) {
    return Err(req, "bad-arg",
               std::format("no spanning-tree port '{}'", port),
               "check `show spanning-tree`");
  }
  if (!it->bpdu_guard_error) {
    return Ok(req, std::format("{} is not blocked by bpdu-guard\n", port));
  }
  if (!stp::ClearBpduGuard(bridge, port)) {
    return Err(req, "hw-error",
               std::format("bouncing {} failed", port));
  }
  return Ok(req, std::format("{} bounced; bpdu-guard cleared\n", port));
}

/// `show neighbors [port]` — who is on the other end of each cable,
/// as LLDP heard it. Every string here came off the network and was
/// sanitised on the way in (see lldp::SanitizeWireString).
auto ShowNeighbors(const Request &req) -> Response {
  const auto filter = Arg(req, 0);
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::string body;
  for (const auto &n : lldp::ReadNeighbors(now)) {
    if (!filter.empty() && n.local_port != filter) continue;
    body += Row({n.local_port, n.chassis_id,
                 n.system_name.empty() ? "-" : n.system_name, n.port_id,
                 n.management_address.empty() ? "-" : n.management_address,
                 n.capabilities.empty() ? "-" : n.capabilities,
                 std::format("{}s", now - n.last_seen)});
  }
  return Ok(req, body);
}

/// `show system services` — what the switch runs behind the schema,
/// and whether it is actually up. This is where "DHCP is configured
/// but dnsmasq died" becomes visible instead of becoming a support
/// call about clients not getting addresses.
auto ShowSystemServices(const Request &req) -> Response {
  std::string body;
  for (const auto &s : svc::GetAll()) {
    const char *state = !s.wanted ? (s.running ? "running (not configured)"
                                               : "not configured")
                        : s.running ? "running"
                                    : "DOWN";
    body += Row({s.name, state, s.detail});
  }
  return Ok(req, body);
}

/// `show interfaces detail [port]` — what a port was CONFIGURED to do
/// alongside what it actually negotiated. Both, because "I set 1000
/// and the link came up at 100" is the question this answers.
auto ShowInterfacesDetail(const Request &req) -> Response {
  std::string body;
  for (const auto &name : FilteredPorts(req)) {
    const auto st = dsa::GetPortStatus(name);
    const auto link = dsa::GetPortParams(name);
    const auto c = dsa::GetPortCounters(name);
    body += Row({name, "link", st.link ? "up" : "down"});
    body += Row({name, "admin", st.enabled ? "up" : "down"});
    body += Row({name, "speed", std::format("{} (negotiated {})",
                                            link.speed,
                                            link.negotiated_speed)});
    body += Row({name, "duplex", std::format("{} (negotiated {})",
                                             link.duplex,
                                             link.negotiated_duplex)});
    body += Row({name, "mtu", std::to_string(link.mtu)});
    body += Row({name, "flow-control", link.flow_control ? "on" : "off"});
    body += Row({name, "rx", std::format("{} bytes / {} pkts / {} err",
                                         c.rx_bytes, c.rx_packets,
                                         c.rx_errors)});
    body += Row({name, "tx", std::format("{} bytes / {} pkts / {} err",
                                         c.tx_bytes, c.tx_packets,
                                         c.tx_errors)});
    if (const auto n = ParseInt(name.substr(3)); n && poe::Available()) {
      const auto p = poe::GetPortStatus(*n);
      body += Row({name, "poe", std::format("{:.1f} W ({})", p.power_w,
                                            p.status)});
    }
  }
  return Ok(req, body);
}

/// The running configuration, for the handful of things that have no
/// counterpart on the box. Empty when nothing supplied a reader.
std::function<cli::confd::Config()> g_config_reader;  // NOLINT

auto RunningConfig() -> cli::confd::Config {
  return g_config_reader ? g_config_reader() : cli::confd::Config{};
}

/// `show vlans` — one row per VLAN rather than one per (VLAN, port).
/// A VLAN is the thing an operator reasons about: what it is called,
/// whether the switch routes for it, and which ports are in it. The
/// per-port breakdown lives in the members column, where it reads as
/// the answer to "who is in VLAN 20" instead of as four rows that have
/// to be assembled by eye.
auto ShowVlans(const Request &req) -> Response {
  const auto config = RunningConfig();
  const auto topo = fabric::S5Topology();
  const auto ports = Ports();
  std::map<int, std::vector<std::string>> members;
  for (const auto &v : dsa::GetVlans()) {
    // `bridge vlan show` also lists the bridge device itself; the
    // bridge's own membership is the SVI, reported in the L3 column.
    if (std::find(ports.begin(), ports.end(), v.port) == ports.end()) {
      continue;
    }
    const char *mode = (v.untagged && v.pvid) ? "u,pvid"
                       : v.untagged           ? "u"
                       : v.pvid               ? "pvid"
                                              : "t";
    members[v.vid].push_back(std::format("{}({})", v.port, mode));
  }
  std::map<int, std::string> addresses;
  for (const auto &svi : l3::GetSvis(topo.bridge)) {
    if (!svi.address.empty()) addresses[svi.vid] = svi.address;
  }
  std::map<int, std::string> names;
  for (const auto &[path, value] : config) {
    if (!path.starts_with("vlans.") || !path.ends_with(".name")) continue;
    const auto vid = std::atoi(path.substr(6).c_str());
    if (vid > 0) names[vid] = value;
  }
  // Union of everything: a VLAN can exist as ports alone, as a name
  // alone, or as an address alone, and all three are worth showing.
  std::set<int> vids;
  for (const auto &[vid, unused] : members) vids.insert(vid);
  for (const auto &[vid, unused] : addresses) vids.insert(vid);
  for (const auto &[vid, unused] : names) vids.insert(vid);
  std::string body;
  for (int vid : vids) {
    std::string member_list;
    const auto it = members.find(vid);
    if (it != members.end()) {
      for (std::size_t i = 0; i < it->second.size(); ++i) {
        if (i > 0) member_list += ' ';
        member_list += it->second[i];
      }
    }
    const auto name = names.contains(vid) ? names.at(vid) : std::string("-");
    const auto addr =
        addresses.contains(vid) ? addresses.at(vid) : std::string("-");
    body += Row({std::to_string(vid), name, addr,
                 member_list.empty() ? "-" : member_list});
  }
  return Ok(req, body);
}

/// `show dhcp leases` — who currently holds an address. Read from
/// dnsmasq's lease database, so it reflects the server rather than the
/// configuration.
auto ShowDhcpLeases(const Request &req) -> Response {
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::string body;
  for (const auto &l : dnsmasq::ReadLeases()) {
    const auto remaining = l.expires - now;
    body += Row({l.ip, l.mac,
                 l.hostname.empty() || l.hostname == "*" ? "-" : l.hostname,
                 remaining > 0 ? std::format("{}m", remaining / 60)
                               : "expired"});
  }
  return Ok(req, body);
}

/// `show dhcp server` — the pools as the server actually holds them,
/// read back out of the generated configuration rather than out of
/// the candidate. A pool that failed to reach the file is exactly the
/// thing an operator is looking for here.
auto ShowDhcpServer(const Request &req) -> Response {
  std::string body;
  const bool running = svc::Running("dnsmasq");
  body += Row({"server", running ? "running" : "not running", "", ""});
  std::ifstream f(util::FsPath(dnsmasq::ConfigPath()));
  if (!f) {
    body += Row({"pools", "none configured", "", ""});
    return Ok(req, body);
  }
  const auto leases = dnsmasq::ReadLeases();
  std::string line;
  while (std::getline(f, line)) {
    if (!line.starts_with("dhcp-range=set:")) continue;
    // dhcp-range=set:vlan10,10.10.0.100,10.10.0.200,255.255.255.0,720m
    const auto body_text = line.substr(std::strlen("dhcp-range=set:"));
    std::vector<std::string> f2;
    std::string field;
    std::istringstream ls(body_text);
    while (std::getline(ls, field, ',')) f2.push_back(field);
    if (f2.size() < 5) continue;
    // Count leases inside this pool's range, which is the number that
    // answers "am I about to run out of addresses".
    std::size_t used = 0;
    for (const auto &l : leases) {
      if (l.ip >= f2[1] && l.ip <= f2[2]) ++used;
    }
    body += Row({f2[0], std::format("{} - {}", f2[1], f2[2]), f2[4],
                 std::format("{} lease(s)", used)});
  }
  return Ok(req, body);
}

/// `clear dhcp lease <ip|mac>` — hand an address back to the pool.
auto ClearDhcpLease(const Request &req) -> Response {
  const auto who = Arg(req, 0);
  if (!SafeToken(who)) {
    return Err(req, "bad-arg", "invalid address or MAC",
               "usage: clear dhcp lease <ip|mac>");
  }
  bool removed = false;
  if (!dnsmasq::RemoveLease(who, &removed)) {
    return Err(req, "sys-error", "rewriting the lease database failed");
  }
  if (!removed) {
    return Ok(req, std::format("no lease held by {}\n", who));
  }
  // dnsmasq reads its lease database only at startup, so the file edit
  // alone would leave the daemon still holding the lease in memory.
  if (svc::Running("dnsmasq") &&
      !svc::Restart({.name = "dnsmasq", .command = dnsmasq::Command()})) {
    return Err(req, "sys-error",
               "the lease was released but dnsmasq did not come back",
               "check `show system services`");
  }
  return Ok(req, std::format("lease held by {} released\n", who));
}

/// `show route` — the forwarding table with an origin column, so a
/// route the operator configured is distinguishable from one the
/// kernel or a DHCP lease put there.
auto ShowRoute(const Request &req) -> Response {
  std::string body;
  body += Row({"forwarding",
               l3::GetForwarding() ? "enabled" : "disabled", "", ""});
  for (const auto &r : l3::GetRoutes()) {
    body += Row({r.prefix, r.via.empty() ? "-" : r.via,
                 r.device.empty() ? "-" : r.device, r.origin});
  }
  return Ok(req, body);
}

/// State directory, for the boot report `show system` reads.
std::string g_state_dir;  // NOLINT: process-wide config seam

/// The `config-divergence` row (WP0.6). Three distinguishable answers,
/// because they mean different things to an operator:
///   "none"                  — the box came back holding what was
///                             committed.
///   "N path(s) at boot ..." — reality disagreed with committed intent:
///                             something changed the box outside the
///                             management plane.
///   "unknown (...)"         — we cannot say, and saying "none" would
///                             be a lie. Notably when boot-restore did
///                             not run on this boot at all.
auto DivergenceSummary() -> std::string {
  if (g_state_dir.empty()) return "unknown (no state directory)";
  auto rep = cli::confd::LoadBootReport(g_state_dir);
  if (!rep) return "unknown (boot report unreadable)";
  if (!rep->has_value()) return "unknown (no boot report)";
  const auto &r = **rep;
  if (!cli::confd::IsFromCurrentBoot(r, cli::confd::CurrentBootId())) {
    return "unknown (boot-restore did not run this boot)";
  }
  if (r.reconcile_conflicts == 0) return "none";
  return std::format("{} path(s) diverged at boot from commit {}",
                     r.reconcile_conflicts, r.applied_revision);
}

/// The `alarms` row of `show system`. A BPDU-guard violation takes a
/// port out of service, which is exactly the class of event an operator
/// finds by looking at the box's overall health rather than by
/// happening to run `show spanning-tree`.
auto AlarmSummary() -> std::string {
  std::vector<std::string> blocked;
  for (const auto &p : stp::GetPortStates(fabric::S5Topology().bridge)) {
    if (p.bpdu_guard_error) blocked.push_back(p.port);
  }
  if (blocked.empty()) return "none";
  std::string ports;
  for (std::size_t i = 0; i < blocked.size(); ++i) {
    if (i > 0) ports += ' ';
    ports += blocked[i];
  }
  return std::format("bpdu-guard blocked {} — `clear spanning-tree "
                     "bpdu-guard <port>` after removing the loop",
                     ports);
}

auto Join(const std::vector<std::string> &items) -> std::string {
  if (items.empty()) return "-";
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += ' ';
    out += items[i];
  }
  return out;
}

/// The fabric the backend builds, as the box actually holds it. This is
/// how "my VLANs do nothing" gets diagnosed: vlan_filtering off, or a
/// port that never made it into the bridge.
auto ShowFabric(const Request &req) -> Response {
  const auto st = fabric::GetStatus(fabric::S5Topology());
  std::string body;
  body += Row({"bridge", st.bridge});
  body += Row({"exists", st.exists ? "yes" : "no"});
  body += Row({"vlan filtering", st.vlan_filtering ? "yes" : "no"});
  body += Row({"state", st.up ? "up" : "down"});
  body += Row({"conduit", st.conduit.empty() ? "-" : st.conduit});
  body += Row({"enslaved", Join(st.enslaved)});
  body += Row({"detached", Join(st.detached)});
  body += Row({"absent", Join(st.absent)});
  body += Row({"routed", Join(st.routed)});
  return Ok(req, body);
}

auto ShowVersion(const Request &req) -> Response {
  std::string body = std::format("  product: einheit s5\n  ports:   {}\n",
                                 Ports().size());
  for (const auto &p : Ports()) {
    body += std::format("    {}\n", p);
  }
  return Ok(req, body);
}

auto ShowSystem(const Request &req) -> Response {
  const auto mem = sys::GetMemInfo();
  const auto disk = sys::GetDiskInfo();
  std::string body;
  body += Row({"hostname", sys::GetHostname()});
  body += Row({"uptime", sys::GetUptime()});
  body += Row({"cpu temp", sys::GetCpuTemp()});
  body += Row({"memory", std::format("{} MB / {} MB",
                                     mem.avail_kb / 1024,
                                     mem.total_kb / 1024)});
  body += Row({"disk", std::format("{} / {} ({})", disk.used,
                                   disk.size, disk.use_pct)});
  body += Row({"ports", std::to_string(Ports().size())});
  body += Row({"config-divergence", DivergenceSummary()});
  body += Row({"alarms", AlarmSummary()});
  return Ok(req, body);
}

auto ShowIp(const Request &req) -> Response {
  std::string body;
  for (const auto &i : sys::GetInterfaces()) {
    body += Row({i.name, i.address, i.state, i.mac});
  }
  return Ok(req, body);
}

auto ShowDns(const Request &req) -> Response {
  std::string body;
  for (const auto &s : sys::GetDnsServers()) {
    body += Row({s});
  }
  return Ok(req, body);
}

auto ShowNtp(const Request &req) -> Response {
  const auto ntp = sys::GetNtpStatus();
  std::string body;
  body += Row({"synced", ntp.synced ? "yes" : "no"});
  body += Row({"server", ntp.server});
  body += Row({"serving clients",
               sys::GetNtpServing() ? "yes" : "no"});
  return Ok(req, body);
}

auto ShowLog(const Request &req) -> Response {
  int lines = 20;
  if (const auto arg = Arg(req, 0); !arg.empty()) {
    const auto n = ParseInt(arg);
    if (!n || *n < 1) {
      return Err(req, "bad-arg",
                 std::format("invalid line count '{}'", arg),
                 "expected a positive integer");
    }
    lines = *n;
  }
  return Ok(req, sys::GetSyslog(lines));
}

auto ShowUsers(const Request &req) -> Response {
  std::string body;
  for (const auto &u : sys::GetUsers()) {
    body += Row({u.name, u.role, std::to_string(u.uid)});
  }
  return Ok(req, body);
}

auto ShowPoe(const Request &req) -> Response {
  std::vector<poe::PortPoeStatus> statuses;
  if (const auto arg = Arg(req, 0); !arg.empty()) {
    const auto port = ParsePort(arg);
    if (!port) {
      return Err(req, "bad-arg",
                 std::format("invalid port '{}'", arg),
                 "expected 1-5");
    }
    statuses.push_back(poe::GetPortStatus(*port));
  } else {
    statuses = poe::GetAllStatus();
  }
  std::string body;
  for (const auto &s : statuses) {
    body += Row({std::to_string(s.port), s.status,
                 s.delivering ? "delivering"
                              : (s.enabled ? "enabled" : "disabled"),
                 std::format("{:.1f}", s.voltage_v),
                 std::format("{:.0f}", s.current_ma),
                 std::format("{:.1f}", s.power_w),
                 s.classification});
  }
  body += Row({"total", std::format("{:.1f}", poe::GetTotalPower())});
  return Ok(req, body);
}

auto PoeReset(const Request &req) -> Response {
  const auto port = ParsePort(Arg(req, 0));
  if (!port) {
    return Err(req, "bad-arg", "invalid port", "expected 1-5");
  }
  if (!poe::ResetPort(*port)) {
    return Err(req, "hw-error",
               std::format("power-cycle of port {} failed", *port));
  }
  return Ok(req, std::format("port {} power-cycled\n", *port));
}

auto UserAdd(const Request &req) -> Response {
  const auto name = Arg(req, 0);
  const auto role = Arg(req, 1);
  if (!SafeToken(name)) {
    return Err(req, "bad-arg", "invalid username");
  }
  if (role != "admin" && role != "operator") {
    return Err(req, "bad-arg", "invalid role",
               "expected admin|operator");
  }
  if (!sys::AddUser(name, role)) {
    return Err(req, "sys-error",
               std::format("adding user '{}' failed", name));
  }
  return Ok(req, std::format("user '{}' added as {}\n", name, role));
}

auto UserRemove(const Request &req) -> Response {
  const auto name = Arg(req, 0);
  if (!SafeToken(name)) {
    return Err(req, "bad-arg", "invalid username");
  }
  if (!sys::DelUser(name)) {
    return Err(req, "sys-error",
               std::format("removing user '{}' failed", name));
  }
  return Ok(req, std::format("user '{}' removed\n", name));
}

auto Ping(const Request &req) -> Response {
  const auto host = Arg(req, 0);
  if (!SafeToken(host)) {
    return Err(req, "bad-arg", "invalid host");
  }
  return Ok(req, util::RunCmd(
      std::format("ping -c 4 -W 2 {} 2>&1", host)));
}

auto Traceroute(const Request &req) -> Response {
  const auto host = Arg(req, 0);
  if (!SafeToken(host)) {
    return Err(req, "bad-arg", "invalid host");
  }
  return Ok(req, util::RunCmd(
      std::format("traceroute {} 2>&1", host)));
}

auto RebootBox(const Request &req) -> Response {
  // Deferred so this response still reaches the renderer before
  // the box goes down.
  util::RunCmd("(sleep 1; reboot) >/dev/null 2>&1 &");
  return Ok(req, "rebooting...\n");
}

}  // namespace

auto SetStateDir(std::string dir) -> void {
  g_state_dir = std::move(dir);
}

auto SetRunningConfigReader(std::function<cli::confd::Config()> reader)
    -> void {
  g_config_reader = std::move(reader);
}

auto ResetCachesForTesting() -> void {
  PortsCache().clear();
  g_counter_base.clear();
  g_stp_base.clear();
  g_config_reader = {};
}

auto HandleProduct(const Request &req) -> std::optional<Response> {
  const auto &c = req.command;
  if (c == "show_interfaces") return ShowInterfaces(req);
  if (c == "show_counters") return ShowCounters(req);
  if (c == "show_mac_table") return ShowMacTable(req);
  if (c == "clear_mac_table") return ClearMacTable(req);
  if (c == "show_igmp_snooping") return ShowIgmpSnooping(req);
  if (c == "show_spanning_tree") return ShowSpanningTree(req);
  if (c == "show_spanning_tree_statistics") {
    return ShowSpanningTreeStatistics(req);
  }
  if (c == "clear_spanning_tree_statistics") {
    return ClearSpanningTreeStatistics(req);
  }
  if (c == "clear_bpdu_guard") return ClearBpduGuard(req);
  if (c == "show_neighbors") return ShowNeighbors(req);
  if (c == "show_route") return ShowRoute(req);
  if (c == "show_dhcp_leases") return ShowDhcpLeases(req);
  if (c == "show_dhcp_server") return ShowDhcpServer(req);
  if (c == "clear_dhcp_lease") return ClearDhcpLease(req);
  if (c == "show_system_services") return ShowSystemServices(req);
  if (c == "show_interfaces_detail") return ShowInterfacesDetail(req);
  if (c == "clear_counters") return ClearCounters(req);
  if (c == "show_vlans") return ShowVlans(req);
  if (c == "show_fabric") return ShowFabric(req);
  if (c == "show_version") return ShowVersion(req);
  if (c == "show_system") return ShowSystem(req);
  if (c == "show_ip") return ShowIp(req);
  if (c == "show_dns") return ShowDns(req);
  if (c == "show_ntp") return ShowNtp(req);
  if (c == "show_log") return ShowLog(req);
  if (c == "show_users") return ShowUsers(req);
  if (c == "show_poe") return ShowPoe(req);
  if (c == "poe_reset") return PoeReset(req);
  if (c == "user_add") return UserAdd(req);
  if (c == "user_remove") return UserRemove(req);
  if (c == "ping") return Ping(req);
  if (c == "traceroute") return Traceroute(req);
  if (c == "reboot") return RebootBox(req);
  return std::nullopt;
}

}  // namespace einheit::s5::service
