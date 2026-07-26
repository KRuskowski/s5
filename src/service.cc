/// @file service.cc
/// @brief Daemon-side execution of s5 product wire commands.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/service.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "einheit/cli/confd/boot_report.h"
#include "einheit/s5/dsa.h"
#include "einheit/s5/fabric.h"
#include "einheit/s5/poe.h"
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

auto ShowVlans(const Request &req) -> Response {
  std::string body;
  for (const auto &v : dsa::GetVlans()) {
    body += Row({std::to_string(v.vid), v.port,
                 v.untagged ? "yes" : "-", v.pvid ? "yes" : "-"});
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

auto ResetCachesForTesting() -> void {
  PortsCache().clear();
  g_counter_base.clear();
}

auto HandleProduct(const Request &req) -> std::optional<Response> {
  const auto &c = req.command;
  if (c == "show_interfaces") return ShowInterfaces(req);
  if (c == "show_counters") return ShowCounters(req);
  if (c == "show_mac_table") return ShowMacTable(req);
  if (c == "clear_mac_table") return ClearMacTable(req);
  if (c == "show_igmp_snooping") return ShowIgmpSnooping(req);
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
