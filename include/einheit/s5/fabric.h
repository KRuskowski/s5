/// @file fabric.h
/// @brief Switch-fabric bootstrap — the bridge is config, not
/// preexisting environment.
///
/// The VLAN-aware bridge, the enslaved switch ports and the DSA
/// conduit are what make an s5 a switch. Until this existed they were
/// hand-built on the test VM by a prep script, which meant the product
/// could not construct its own fabric: every port and VLAN apply
/// depended on setup nobody on the box owned, and a reboot took the
/// whole thing with it.
///
/// The topology is hardcoded rather than schema-driven. On a five-port
/// box with one ASIC there is nothing here an operator can usefully
/// change, and a `fabric.*` schema root would only invite
/// configurations the hardware cannot honour.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_FABRIC_H_
#define EINHEIT_S5_FABRIC_H_

#include <expected>
#include <string>
#include <vector>

#include "einheit/cli/error.h"

namespace einheit::s5::fabric {

/// The s5 switch fabric.
struct Topology {
  /// VLAN-aware bridge the switch ports join.
  std::string bridge = "br0";
  /// Ports enslaved to the bridge. A port named here that does not
  /// exist on this box is skipped and reported, not treated as a
  /// failure — a dev board with fewer ports still has to work.
  std::vector<std::string> members = {"lan1", "lan2", "lan3", "lan4"};
  /// Ports deliberately kept out of the bridge and brought up routed.
  /// lan5 is the box's uplink: roadmap Phase 2 puts
  /// `interfaces.wan.gateway` on it and Phase 3 masquerades to it, so
  /// bridging it would take the WAN port away.
  std::vector<std::string> routed = {"lan5"};
  /// Fallback names for the DSA conduit (the CPU-port netdev) when it
  /// cannot be discovered from a member port.
  std::vector<std::string> conduit_candidates = {"eth0", "end0"};
};

/// Why the fabric could not be constructed.
enum class FabricError {
  /// A bridge / enslave / link command was rejected.
  CommandFailed,
};

/// The topology this build of s5 owns.
/// @returns The hardcoded s5 fabric.
auto S5Topology() -> const Topology &;

/// Converge the box onto `topo`, idempotently: create the bridge with
/// vlan_filtering enabled, enslave the member ports, keep the routed
/// ports out of the bridge, and bring the bridge and the DSA conduit
/// up. Only issues the commands whose state is actually wrong, so
/// calling it before every apply costs a handful of sysfs reads once
/// the fabric is already right.
///
/// It does NOT touch per-port admin state. `ports.<p>.enabled` is
/// configuration; a fabric that brought its members up would silently
/// re-enable a shut port on every CLI invocation. An unconfigured box
/// gets its ports enabled by the factory configuration the boot apply
/// seeds — also config, so admin state has exactly one owner.
/// @param topo Desired fabric.
/// @returns void on success, or the first command that failed.
auto Ensure(const Topology &topo)
    -> std::expected<void, cli::Error<FabricError>>;

/// Observed fabric state.
struct Status {
  /// Bridge name from the topology.
  std::string bridge;
  /// Whether the bridge netdev exists.
  bool exists = false;
  /// Whether 802.1Q filtering is on (without it VLAN config is inert).
  bool vlan_filtering = false;
  /// Whether the bridge is administratively up.
  bool up = false;
  /// DSA conduit netdev, empty when none was found.
  std::string conduit;
  /// Member ports actually enslaved to the bridge.
  std::vector<std::string> enslaved;
  /// Member ports that exist but are not enslaved.
  std::vector<std::string> detached;
  /// Member ports the topology names but the box does not have.
  std::vector<std::string> absent;
  /// Routed ports that exist, in topology order.
  std::vector<std::string> routed;
};

/// Read the fabric back off the box. Drives `show fabric`, so an
/// operator can tell "VLANs are not taking effect" from "the bridge
/// never came up".
/// @param topo Topology to compare against.
/// @returns Observed state.
auto GetStatus(const Topology &topo) -> Status;

}  // namespace einheit::s5::fabric

#endif  // EINHEIT_S5_FABRIC_H_
