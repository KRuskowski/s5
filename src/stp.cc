/// @file stp.cc
/// @brief RSTP control plane — mstpctl for writes, its JSON for reads.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/stp.h"

#include <cctype>
#include <cstdlib>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "einheit/s5/dsa.h"
#include "einheit/s5/util.h"

namespace einheit::s5::stp {
namespace {

using util::ReadSysfs;
using util::RunCmd;

/// mstpctl is only ever on the system path; the full path keeps the
/// generated command lines stable for the unit tests' fake box and
/// stops a PATH surprise from silently disabling loop protection.
constexpr const char *kMstpctl = "mstpctl";

/// Kernel bridge stp_state values. 2 (BR_USER_STP) is the only one that
/// means "a userspace daemon owns this bridge's spanning tree"; 1 is the
/// kernel's own 802.1D, which is precisely what we are replacing.
constexpr const char *kUserStp = "2";

/// Flat-JSON reader for mstpctl's `-f json` output.
///
/// mstpctl emits an array of flat objects whose values are ALL strings —
/// no nesting, no numbers, no escapes beyond the quoting. Parsing that
/// shape is thirty lines; the alternative is the two-column plain-text
/// layout, whose field boundaries are column positions that shift with
/// the longest value on the line. A malformed document yields fewer
/// objects, never a crash.
/// @param text One mstpctl JSON document.
/// @returns One key→value map per object, in document order.
auto ParseFlatJsonArray(const std::string &text)
    -> std::vector<std::map<std::string, std::string>> {
  std::vector<std::map<std::string, std::string>> out;
  std::map<std::string, std::string> cur;
  std::string key;
  std::string token;
  bool in_string = false;
  bool have_key = false;
  bool in_object = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (in_string) {
      if (c == '\\' && i + 1 < text.size()) {
        token += text[++i];
      } else if (c == '"') {
        in_string = false;
      } else {
        token += c;
      }
      continue;
    }
    switch (c) {
      case '"':
        in_string = true;
        token.clear();
        break;
      case ':':
        key = token;
        have_key = true;
        break;
      case ',':
        if (have_key) cur[key] = token;
        have_key = false;
        break;
      case '{':
        in_object = true;
        cur.clear();
        have_key = false;
        break;
      case '}':
        if (have_key) cur[key] = token;
        have_key = false;
        if (in_object) out.push_back(cur);
        in_object = false;
        break;
      default:
        break;
    }
  }
  return out;
}

auto Get(const std::map<std::string, std::string> &m,
         const std::string &key) -> std::string {
  const auto it = m.find(key);
  return it == m.end() ? std::string() : it->second;
}

auto GetBool(const std::map<std::string, std::string> &m,
             const std::string &key) -> bool {
  return Get(m, key) == "yes";
}

auto GetUint(const std::map<std::string, std::string> &m,
             const std::string &key) -> std::uint64_t {
  const auto v = Get(m, key);
  if (v.empty()) return 0;
  return std::strtoull(v.c_str(), nullptr, 10);
}

/// Decode the priority field out of an mstpd identifier. Bridge ids
/// read `8.000.26:C6:…` and port ids `8.001`; in both the leading hex
/// digit is the priority field, scaled by the standard's step.
/// @param id Bridge id or port id.
/// @param step 4096 for a bridge, 16 for a port.
/// @returns Priority in operator units, or the default when unparsable.
auto PriorityFromId(const std::string &id, int step, int fallback)
    -> int {
  const auto dot = id.find('.');
  if (dot == std::string::npos || dot == 0) return fallback;
  const auto field = id.substr(0, dot);
  for (char c : field) {
    if (std::isxdigit(static_cast<unsigned char>(c)) == 0) return fallback;
  }
  return static_cast<int>(std::strtol(field.c_str(), nullptr, 16)) * step;
}

/// Run an mstpctl subcommand. Silence on stdout+stderr is mstpctl's
/// success signal; anything else (including "Not a valid interface")
/// is a failed write.
auto Ctl(const std::string &args) -> bool {
  return RunCmd(std::format("{} {} 2>&1", kMstpctl, args)).empty();
}

auto Query(const std::string &args) -> std::string {
  return RunCmd(std::format("{} -f json {} 2>/dev/null", kMstpctl, args));
}

auto StpStatePath(const std::string &bridge) -> std::string {
  return std::format("/sys/class/net/{}/bridge/stp_state", bridge);
}

}  // namespace

auto ParseMode(const std::string &s) -> Mode {
  if (s == "rstp") return Mode::Rstp;
  if (s == "stp") return Mode::Stp;
  return Mode::Off;
}

auto ModeToken(Mode m) -> std::string {
  switch (m) {
    case Mode::Rstp: return "rstp";
    case Mode::Stp: return "stp";
    case Mode::Off: return "";
  }
  return "";
}

auto Available() -> bool {
  return !RunCmd("command -v mstpd 2>/dev/null").empty() &&
         !RunCmd("command -v mstpctl 2>/dev/null").empty();
}

auto Running() -> bool {
  return !RunCmd("pidof mstpd 2>/dev/null").empty();
}

auto GetMode(const std::string &bridge) -> Mode {
  if (ReadSysfs(StpStatePath(bridge)) != kUserStp) return Mode::Off;
  const auto docs = ParseFlatJsonArray(Query("showbridge " + bridge));
  if (docs.empty()) return Mode::Off;
  if (Get(docs[0], "stp-enabled") != "yes") return Mode::Off;
  return ParseMode(Get(docs[0], "force-protocol-version"));
}

auto SetMode(const std::string &bridge, Mode mode) -> bool {
  const bool user_stp = ReadSysfs(StpStatePath(bridge)) == kUserStp;
  if (mode == Mode::Off) {
    if (!user_stp) return true;
    // delbridge first: dropping the kernel out of user-STP mode while
    // mstpd still tracks the bridge leaves the daemon driving port
    // states nobody asked it to own.
    Ctl(std::format("delbridge {}", bridge));
    return RunCmd(std::format(
                      "ip link set {} type bridge stp_state 0 2>&1", bridge))
        .empty();
  }
  if (!user_stp) {
    // Writing 1 makes the kernel run /sbin/bridge-stp, which starts
    // mstpd and registers the bridge; the kernel only settles on
    // BR_USER_STP (2) if that helper succeeds. Going through 0 first
    // matters when the bridge is stuck in KERNEL STP (1) — the kernel
    // ignores a redundant enable and would leave 802.1D running while
    // the CLI reported RSTP.
    RunCmd(std::format("ip link set {} type bridge stp_state 0 2>&1",
                       bridge));
    RunCmd(std::format("ip link set {} type bridge stp_state 1 2>&1",
                       bridge));
  }
  // The helper adds the bridge asynchronously when it had to start the
  // daemon (a kernel lock forces it to background the call), so do it
  // again here rather than racing it. addbridge is idempotent.
  Ctl(std::format("addbridge {}", bridge));
  if (ReadSysfs(StpStatePath(bridge)) != kUserStp) return false;
  return Ctl(std::format("setforcevers {} {}", bridge, ModeToken(mode)));
}

auto SetBridgePriority(const std::string &bridge, int priority) -> bool {
  // mstpctl takes the 4-bit priority field; the operator configures the
  // 16-bit value the standard prints.
  return Ctl(std::format("settreeprio {} 0 {}", bridge, priority / 4096));
}

auto SetHello(const std::string &bridge, int seconds) -> bool {
  return Ctl(std::format("sethello {} {}", bridge, seconds));
}

auto SetMaxAge(const std::string &bridge, int seconds) -> bool {
  return Ctl(std::format("setmaxage {} {}", bridge, seconds));
}

auto SetForwardDelay(const std::string &bridge, int seconds) -> bool {
  return Ctl(std::format("setfdelay {} {}", bridge, seconds));
}

auto SetPortCost(const std::string &bridge, const std::string &port,
                 int cost) -> bool {
  return Ctl(std::format("setportpathcost {} {} {}", bridge, port, cost));
}

auto SetPortPriority(const std::string &bridge, const std::string &port,
                     int priority) -> bool {
  return Ctl(
      std::format("settreeportprio {} {} 0 {}", bridge, port, priority / 16));
}

auto SetPortEdge(const std::string &bridge, const std::string &port,
                 bool edge) -> bool {
  if (!Ctl(std::format("setportadminedge {} {} {}", bridge, port,
                       edge ? "yes" : "no"))) {
    return false;
  }
  // Auto-edge would drag an admin-edge port back out of edge state the
  // moment a BPDU arrives, and drag a deliberately non-edge port INTO
  // it after a quiet interval. Either way the configured value stops
  // describing the box, so admin edge owns the port outright.
  return Ctl(std::format("setportautoedge {} {} {}", bridge, port,
                         edge ? "no" : "yes"));
}

auto SetPortBpduGuard(const std::string &bridge, const std::string &port,
                      bool on) -> bool {
  return Ctl(std::format("setbpduguard {} {} {}", bridge, port,
                         on ? "yes" : "no"));
}

auto GetBridgeState(const std::string &bridge) -> BridgeState {
  BridgeState st;
  if (ReadSysfs(StpStatePath(bridge)) != kUserStp) return st;
  const auto docs = ParseFlatJsonArray(Query("showbridge " + bridge));
  if (docs.empty()) return st;
  const auto &b = docs[0];
  if (Get(b, "stp-enabled") != "yes") return st;
  st.enabled = true;
  st.mode = Get(b, "force-protocol-version");
  st.bridge_id = Get(b, "bridge-id");
  st.root_id = Get(b, "designated-root");
  st.root_port = Get(b, "root-port");
  st.priority = PriorityFromId(st.bridge_id, 4096, 32768);
  st.hello = Get(b, "hello-time");
  st.max_age = Get(b, "max-age");
  st.forward_delay = Get(b, "forward-delay");
  st.admin_max_age = Get(b, "bridge-max-age");
  st.admin_forward_delay = Get(b, "bridge-forward-delay");
  st.topology_changes = Get(b, "topology-change-count");
  st.time_since_change = Get(b, "time-since-topology-change");
  return st;
}

auto GetPortStates(const std::string &bridge) -> std::vector<PortState> {
  std::vector<PortState> out;
  if (ReadSysfs(StpStatePath(bridge)) != kUserStp) return out;
  for (const auto &p :
       ParseFlatJsonArray(Query("showportdetail " + bridge))) {
    PortState st;
    st.port = Get(p, "port");
    if (st.port.empty()) continue;
    st.role = Get(p, "role");
    st.state = Get(p, "state");
    st.cost = Get(p, "external-port-cost");
    st.admin_cost = Get(p, "admin-external-cost");
    st.port_id = Get(p, "port-id");
    st.priority = PriorityFromId(st.port_id, 16, 128);
    st.edge = GetBool(p, "admin-edge-port");
    st.oper_edge = GetBool(p, "oper-edge-port");
    st.bpdu_guard = GetBool(p, "bpdu-guard-port");
    st.bpdu_guard_error = GetBool(p, "bpdu-guard-error");
    st.tx_bpdu = GetUint(p, "num-tx-bpdu");
    st.rx_bpdu = GetUint(p, "num-rx-bpdu");
    st.tx_tcn = GetUint(p, "num-tx-tcn");
    st.rx_tcn = GetUint(p, "num-rx-tcn");
    st.transitions_fwd = GetUint(p, "num-transition-fwd");
    st.transitions_blk = GetUint(p, "num-transition-blk");
    out.push_back(st);
  }
  return out;
}

auto ClearBpduGuard(const std::string &bridge, const std::string &port)
    -> bool {
  (void)bridge;
  // mstpd latches bpdu-guard-error until the port's link goes away and
  // comes back — there is no "unlatch" in the control protocol. So the
  // recovery verb bounces the port, which is also the honest semantic:
  // the operator is asserting the loop is gone.
  //
  // Only a port the configuration says is UP gets bounced. Bringing a
  // deliberately shut port back up would be an operational verb
  // overwriting configuration, which is the one thing they must never
  // do — and a shut port cannot be latched by the guard anyway.
  if (!dsa::IsUp(port)) return true;
  if (!RunCmd(std::format("ip link set {} down 2>&1", port)).empty()) {
    return false;
  }
  return RunCmd(std::format("ip link set {} up 2>&1", port)).empty();
}

}  // namespace einheit::s5::stp
