/// @file l3.h
/// @brief Layer 3 on the switch — SVIs and the routing table.
///
/// A VLAN with an address is an SVI: an 802.1Q upper on the bridge
/// (`br0.<vid>`) plus a `bridge vlan add ... self` entry so the bridge
/// itself is a tagged member of that VLAN. Miss the `self` entry and
/// the interface exists, carries an address, and silently receives
/// nothing — the single most confusing failure in this area, which is
/// why AddSvi does both halves or neither.
///
/// Routing is the same shape as VLAN membership: the configuration owns
/// the full static route set, so a route the operator deleted is
/// removed from the box rather than surviving until the next reboot.
/// Only routes we installed are eligible — `proto static` marks ours,
/// and kernel/DHCP routes are somebody else's to manage.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_L3_H_
#define EINHEIT_S5_L3_H_

#include <cstdint>
#include <string>
#include <vector>

namespace einheit::s5::l3 {

/// A switched virtual interface as the box holds it.
struct Svi {
  std::uint16_t vid = 0;
  /// Netdev name, `<bridge>.<vid>`.
  std::string device;
  /// Address with prefix, empty when the interface carries none.
  std::string address;
  /// Whether the netdev is administratively up.
  bool up = false;
};

/// The SVI netdev name for a VID on a bridge.
auto SviName(const std::string &bridge, int vid) -> std::string;

/// Every SVI currently on the box for `bridge`.
auto GetSvis(const std::string &bridge) -> std::vector<Svi>;

/// Create the SVI for `vid` if it is missing, and make the bridge a
/// member of that VLAN. Idempotent.
/// @param bridge Bridge netdev.
/// @param vid VLAN id.
/// @returns Whether the box now has a usable SVI.
auto AddSvi(const std::string &bridge, int vid) -> bool;

/// Remove an SVI and the bridge's own membership of that VLAN.
auto DelSvi(const std::string &bridge, int vid) -> bool;

/// Set (or clear, with an empty address) an SVI's address and bring it
/// up.
auto SetSviAddress(const std::string &device, const std::string &address)
    -> bool;

// ── Routing ─────────────────────────────────────────────────────

/// Whether IPv4 forwarding is on.
auto GetForwarding() -> bool;
auto SetForwarding(bool on) -> bool;

/// One route, as `show route` prints it.
struct Route {
  /// "default" or a CIDR prefix.
  std::string prefix;
  /// Next hop, empty for a link route.
  std::string via;
  /// Egress interface.
  std::string device;
  /// Where the route came from, for the operator: "config" when the
  /// switch configuration installed it, otherwise whatever the kernel
  /// calls it (kernel, dhcp, static, ...).
  std::string origin;
  /// Whether THIS switch installed it. Only owned routes may be
  /// reconciled away — a `proto static` route the box's own network
  /// configuration put there is not ours to delete, and deleting one
  /// is how a switch cuts off its own uplink.
  bool owned = false;
};

/// The whole IPv4 routing table.
auto GetRoutes() -> std::vector<Route>;

/// Install or replace one static route.
/// @param prefix "default" or a CIDR prefix.
/// @param via Next-hop address.
/// @returns Whether the route is installed.
auto AddRoute(const std::string &prefix, const std::string &via) -> bool;

/// Remove a static route by prefix.
auto DelRoute(const std::string &prefix) -> bool;

}  // namespace einheit::s5::l3

#endif  // EINHEIT_S5_L3_H_
