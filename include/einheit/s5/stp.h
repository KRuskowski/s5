/// @file stp.h
/// @brief Spanning tree (RSTP) over mstpd.
///
/// The kernel bridge implements classic 802.1D STP only, whose 30-second
/// convergence is not something a shipped switch should be melting a
/// network with. RSTP therefore lives in userspace: mstpd owns the state
/// machines, the kernel bridge is put into BR_USER_STP mode, and this
/// module is the control surface — mstpctl for writes, mstpctl's JSON
/// output for reads.
///
/// Spanning tree is ON in the factory configuration on purpose. A
/// five-port switch that ships with a loop-protection feature switched
/// off is a switch that melts the first time somebody patches two wall
/// ports together, and the operator who could have turned it on is the
/// one who cannot reach it any more.
///
/// TODO(bench): the ksz9477 DSA driver may be able to offload port
/// states to the ASIC (`br_set_state` → `ksz_port_stp_state_set`), which
/// would make blocking happen in silicon rather than only on the CPU
/// path. Whether the states actually reach the switch registers on the
/// T113 kernel is a bench question (WP0.7's driver feature inventory);
/// nothing here depends on the answer, and the VM tier cannot answer it.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_STP_H_
#define EINHEIT_S5_STP_H_

#include <cstdint>
#include <string>
#include <vector>

namespace einheit::s5::stp {

/// Protocol mode as the operator configures it.
enum class Mode {
  /// No spanning tree at all — a loop is then the operator's problem.
  Off,
  /// Classic 802.1D, for peers that cannot speak RSTP.
  Stp,
  /// 802.1w rapid spanning tree. The default.
  Rstp,
};

/// Parse a schema enum token into a Mode.
/// @param s One of "off", "stp", "rstp".
/// @returns The mode, or Mode::Off for anything unrecognised.
auto ParseMode(const std::string &s) -> Mode;

/// The mstpctl token for a mode ("stp" / "rstp"); empty for Off.
auto ModeToken(Mode m) -> std::string;

/// Bridge-wide spanning-tree state, as `show spanning-tree` prints it.
struct BridgeState {
  /// Whether the kernel bridge is in user-space STP mode AND mstpd is
  /// tracking it. Everything else is meaningless when this is false.
  bool enabled = false;
  /// Protocol actually in force ("rstp", "stp"), from mstpd.
  std::string mode;
  /// This bridge's own id, `prio.sysid.mac`.
  std::string bridge_id;
  /// The elected root's id; equal to bridge_id when we are the root.
  std::string root_id;
  /// Port toward the root, empty when this bridge IS the root.
  std::string root_port;
  /// Bridge priority in operator units (0..61440), decoded from the
  /// bridge id's priority nibble.
  int priority = 0;
  std::string hello;
  /// Timers in force, which on a non-root bridge come from the ROOT's
  /// BPDUs rather than from this box's configuration.
  std::string max_age;
  std::string forward_delay;
  /// The timers this bridge is configured with — what read-back must
  /// use, since the operational values above are somebody else's.
  std::string admin_max_age;
  std::string admin_forward_delay;
  /// Topology changes counted since mstpd started.
  std::string topology_changes;
  /// Seconds since the last topology change.
  std::string time_since_change;
};

/// Per-port spanning-tree state.
struct PortState {
  std::string port;
  /// Designated / Root / Alternate / Backup / Disabled.
  std::string role;
  /// forwarding / learning / discarding.
  std::string state;
  /// Effective path cost (mstpd's computed value when admin cost is 0).
  std::string cost;
  /// Configured path cost, 0 meaning "derive from link speed". This is
  /// the value read-back must use: reporting the derived cost as
  /// configuration would turn an operator's `auto` into a hard-coded
  /// number on the next commit.
  std::string admin_cost;
  std::string port_id;
  /// Port priority in operator units (0..240), from the port id.
  int priority = 0;
  /// Administratively configured edge port.
  bool edge = false;
  /// Edge as the state machine currently sees it.
  bool oper_edge = false;
  bool bpdu_guard = false;
  /// A BPDU arrived on a guarded port; mstpd is holding it down.
  bool bpdu_guard_error = false;
  /// BPDU counters, for `show spanning-tree statistics`.
  std::uint64_t tx_bpdu = 0;
  std::uint64_t rx_bpdu = 0;
  std::uint64_t tx_tcn = 0;
  std::uint64_t rx_tcn = 0;
  std::uint64_t transitions_fwd = 0;
  std::uint64_t transitions_blk = 0;
};

/// Whether this box can run RSTP at all — both mstpd and mstpctl
/// present and executable. A commit that configures spanning tree on a
/// box without them must FAIL, not quietly leave the network unguarded.
auto Available() -> bool;

/// Whether the mstpd daemon is currently running.
auto Running() -> bool;

/// Put `bridge` into `mode`, starting or detaching mstpd as needed.
/// Idempotent: a bridge already in the requested mode is left alone, so
/// an unrelated commit does not bounce spanning tree (and with it every
/// port's forwarding state) on a live network.
/// @param bridge Bridge netdev name.
/// @param mode Desired protocol mode.
/// @returns Whether the box ended up in the requested mode.
auto SetMode(const std::string &bridge, Mode mode) -> bool;

/// The mode the box currently holds, read back off the kernel bridge
/// and mstpd.
auto GetMode(const std::string &bridge) -> Mode;

/// Bridge priority, in the operator's units (0..61440, steps of 4096).
auto SetBridgePriority(const std::string &bridge, int priority) -> bool;

/// Bridge timers, in seconds.
auto SetHello(const std::string &bridge, int seconds) -> bool;
auto SetMaxAge(const std::string &bridge, int seconds) -> bool;
auto SetForwardDelay(const std::string &bridge, int seconds) -> bool;

/// Port path cost; 0 means "derive from link speed".
auto SetPortCost(const std::string &bridge, const std::string &port,
                 int cost) -> bool;

/// Port priority in operator units (0..240, steps of 16).
auto SetPortPriority(const std::string &bridge, const std::string &port,
                     int priority) -> bool;

/// Admin edge: an edge port skips listening/learning and goes straight
/// to forwarding, which is what you want on an access port and exactly
/// what you do not want on a port that might see another switch.
auto SetPortEdge(const std::string &bridge, const std::string &port,
                 bool edge) -> bool;

/// BPDU guard: a BPDU arriving on a guarded port is treated as a
/// misconfiguration and mstpd holds the port down.
auto SetPortBpduGuard(const std::string &bridge,
                      const std::string &port, bool on) -> bool;

/// Read the bridge's spanning-tree state. `enabled` is false (and the
/// rest empty) when spanning tree is off or mstpd is not running.
auto GetBridgeState(const std::string &bridge) -> BridgeState;

/// Read every tracked port's state. Empty when spanning tree is off.
auto GetPortStates(const std::string &bridge) -> std::vector<PortState>;

/// Recover a port mstpd is holding down for a BPDU-guard violation.
/// mstpd latches the error until the port bounces, so this bounces it:
/// the operator has to be able to clear the condition from the CLI,
/// having presumably unplugged the switch somebody patched in.
/// @param bridge Bridge netdev name.
/// @param port Port to recover.
/// @returns Whether the bounce succeeded.
auto ClearBpduGuard(const std::string &bridge, const std::string &port)
    -> bool;

}  // namespace einheit::s5::stp

#endif  // EINHEIT_S5_STP_H_
