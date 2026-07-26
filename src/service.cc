/// @file service.cc
/// @brief Daemon-side execution of s5 product wire commands.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/service.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

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

auto Ports() -> const std::vector<std::string> & {
  // DSA port set is fixed by the device tree; discover once.
  static const std::vector<std::string> ports = dsa::DiscoverPorts();
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

auto ShowCounters(const Request &req) -> Response {
  std::string body;
  for (const auto &name : FilteredPorts(req)) {
    const auto c = dsa::GetPortCounters(name);
    body += Row({c.name, std::to_string(c.rx_bytes),
                 std::to_string(c.tx_bytes),
                 std::to_string(c.rx_packets),
                 std::to_string(c.tx_packets),
                 std::to_string(c.rx_errors)});
  }
  return Ok(req, body);
}

auto ShowMacTable(const Request &req) -> Response {
  std::string body;
  for (const auto &e : dsa::GetMacTable()) {
    body += Row({e.mac, e.port, std::to_string(e.vid)});
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

auto HandleProduct(const Request &req) -> std::optional<Response> {
  const auto &c = req.command;
  if (c == "show_interfaces") return ShowInterfaces(req);
  if (c == "show_counters") return ShowCounters(req);
  if (c == "show_mac_table") return ShowMacTable(req);
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
