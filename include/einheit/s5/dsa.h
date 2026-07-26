/// @file dsa.h
/// @brief Linux DSA (Distributed Switch Architecture) interface.
///
/// Reads switch port state from /sys/class/net/ and the kernel's
/// bridge/VLAN subsystem. This is how the KSZ9477 appears on
/// Linux when the ksz9477 DSA driver is loaded — each switch
/// port is a separate netdev (lan1..lan5, wan).
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_DSA_H_
#define EINHEIT_S5_DSA_H_

#include <cstdint>
#include <string>
#include <vector>

namespace einheit::s5::dsa {

struct PortStatus {
  std::string name;
  bool link = false;
  bool enabled = false;
  std::string speed;
  std::string duplex;
};

struct PortCounters {
  std::string name;
  std::uint64_t rx_bytes = 0;
  std::uint64_t tx_bytes = 0;
  std::uint64_t rx_packets = 0;
  std::uint64_t tx_packets = 0;
  std::uint64_t rx_errors = 0;
  std::uint64_t tx_errors = 0;
};

struct VlanEntry {
  std::uint16_t vid = 0;
  std::string port;
  bool untagged = false;
  bool pvid = false;
};

struct MacEntry {
  std::string mac;
  std::string port;
  std::uint16_t vid = 0;
  /// Permanent (configured) rather than learned.
  bool is_static = false;
  /// The port's OWN address, installed by the bridge rather than by
  /// anyone's configuration. Reconciliation must never touch these.
  bool is_local = false;
  /// Group/multicast address (the I/G bit in the first octet). The
  /// kernel manages these; they are not operator configuration.
  bool is_multicast = false;
};

/// Discover DSA switch ports. Returns netdev names like
/// "lan1", "lan2", ..., "wan".
auto DiscoverPorts() -> std::vector<std::string>;

/// True when `iface` is administratively up — IFF_UP in the sysfs
/// `flags` attribute. Must be a bit test, not a comparison against a
/// known flags word: the other bits move with the interface's role
/// (an enslaved switch port reads 0x1303/0x1302, a routed one
/// 0x1003/0x1002), so anything that matches whole values reports a
/// bridged-and-down port as up.
/// @param iface Netdev name.
/// @returns Whether IFF_UP is set.
auto IsUp(const std::string &iface) -> bool;

/// Read port status (link, speed, duplex) from sysfs.
auto GetPortStatus(const std::string &port) -> PortStatus;

/// Read port counters from sysfs.
auto GetPortCounters(const std::string &port) -> PortCounters;

/// Enable or disable a port (ip link set up/down).
auto SetPortEnabled(const std::string &port, bool up) -> bool;

/// Read VLAN entries via bridge vlan show.
auto GetVlans() -> std::vector<VlanEntry>;

/// Add a VLAN to a port (bridge vlan add).
auto AddVlan(const std::string &port, std::uint16_t vid,
             bool untagged, bool pvid) -> bool;

/// Delete a VLAN from a port (bridge vlan del).
auto DelVlan(const std::string &port, std::uint16_t vid)
    -> bool;

/// Read the MAC table via bridge fdb show. Includes static entries,
/// distinguished by MacEntry::is_static.
auto GetMacTable() -> std::vector<MacEntry>;

// ── Port parameters (WP1.1) ─────────────────────────────────────

/// Link settings as configured and as negotiated.
struct PortParams {
  /// "auto", or a speed in Mbit/s.
  std::string speed = "auto";
  /// "auto", "half" or "full".
  std::string duplex = "auto";
  /// Current MTU.
  int mtu = 0;
  /// Whether 802.3x pause is on.
  bool flow_control = false;
  /// What autoneg actually settled on, for `show interfaces detail`.
  std::string negotiated_speed;
  std::string negotiated_duplex;
};

/// Read a port's link parameters.
auto GetPortParams(const std::string &port) -> PortParams;

/// Set speed/duplex via ethtool. "auto" for either turns autoneg on;
/// a forced value turns it off, and BOTH must then be concrete —
/// ethtool rejects a half-configured forced link.
auto SetPortSpeedDuplex(const std::string &port,
                        const std::string &speed,
                        const std::string &duplex) -> bool;

/// Set the MTU of a netdev.
auto SetPortMtu(const std::string &port, int mtu) -> bool;

/// Enable or disable 802.3x pause frames.
auto SetPortFlowControl(const std::string &port, bool on) -> bool;

// ── MAC table (WP1.4) ───────────────────────────────────────────

/// Add a permanent fdb entry.
auto AddStaticMac(const std::string &mac, const std::string &port,
                  std::uint16_t vid) -> bool;

/// Remove a permanent fdb entry.
auto DelStaticMac(const std::string &mac, const std::string &port,
                  std::uint16_t vid) -> bool;

/// Bridge MAC ageing time, in seconds.
auto GetMacAging(const std::string &bridge) -> int;
auto SetMacAging(const std::string &bridge, int seconds) -> bool;

/// Flush learned (dynamic) entries, optionally on one port only.
/// Static entries are configuration and are never flushed.
auto FlushMacTable(const std::string &port) -> bool;

// ── IGMP snooping (WP1.7) ───────────────────────────────────────

/// Bridge multicast-snooping state.
struct SnoopState {
  bool enabled = false;
  bool querier = false;
};
auto GetSnooping(const std::string &bridge) -> SnoopState;
auto SetSnooping(const std::string &bridge, bool enabled) -> bool;
auto SetQuerier(const std::string &bridge, bool enabled) -> bool;

/// One multicast group the bridge is forwarding.
struct MdbEntry {
  std::string port;
  std::string group;
  std::uint16_t vid = 0;
};
auto GetMdb() -> std::vector<MdbEntry>;

}  // namespace einheit::s5::dsa

#endif  // EINHEIT_S5_DSA_H_
