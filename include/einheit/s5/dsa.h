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
};

/// Discover DSA switch ports. Returns netdev names like
/// "lan1", "lan2", ..., "wan".
auto DiscoverPorts() -> std::vector<std::string>;

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

/// Read dynamic MAC table via bridge fdb show.
auto GetMacTable() -> std::vector<MacEntry>;

}  // namespace einheit::s5::dsa

#endif  // EINHEIT_S5_DSA_H_
