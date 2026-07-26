/// @file sys.h
/// @brief System management — hostname, network, DNS, NTP, users.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_SYS_H_
#define EINHEIT_S5_SYS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace einheit::s5::sys {

// ── System info ─────────────────────────────────────────────

auto GetHostname() -> std::string;
auto SetHostname(const std::string &name) -> bool;
auto GetUptime() -> std::string;

struct MemInfo {
  std::uint64_t total_kb = 0;
  std::uint64_t free_kb = 0;
  std::uint64_t avail_kb = 0;
};
auto GetMemInfo() -> MemInfo;

struct DiskInfo {
  std::string mount;
  std::string size;
  std::string used;
  std::string avail;
  std::string use_pct;
};
auto GetDiskInfo() -> DiskInfo;

auto GetCpuTemp() -> std::string;

// ── Network ─────────────────────────────────────────────────

struct InterfaceInfo {
  std::string name;
  std::string address;
  std::string state;
  std::string mac;
};
auto GetInterfaces() -> std::vector<InterfaceInfo>;
auto SetInterfaceAddr(const std::string &iface,
                      const std::string &addr) -> bool;
auto SetInterfaceDhcp(const std::string &iface) -> bool;

/// How the session issuing a command reaches this box.
///
/// A remote operator's commands arrive over a path made of exactly the
/// things they are about to reconfigure: an address, an interface, a
/// VLAN, a route. Knowing which ones is what lets the switch warn
/// before it cuts the branch it is sitting on.
struct MgmtPath {
  /// The far end of this session; empty on a local console, where
  /// there is nothing to lose.
  std::string peer;
  /// Netdev the box would answer the peer through.
  std::string device;
  /// Our own address on that path.
  std::string address;
  /// Whether the peer is off-subnet, so the answer needs a route.
  bool routed = false;
};

/// Work out the current session's management path from SSH_CONNECTION
/// and the routing table.
/// @returns The path, with an empty peer when there is none to find.
auto GetManagementPath() -> MgmtPath;

// ── DNS ─────────────────────────────────────────────────────

auto GetDnsServers() -> std::vector<std::string>;
auto SetDnsServers(const std::vector<std::string> &servers)
    -> bool;

// ── NTP ─────────────────────────────────────────────────────

struct NtpStatus {
  std::string server;
  bool synced = false;
  std::string offset;
};
auto GetNtpStatus() -> NtpStatus;

/// Point the box's time daemon at `server`, optionally also answering
/// queries from clients. One daemon does both (busybox ntpd's `-l`),
/// so they are set together: a box that served time it had never
/// synchronised would be worse than one that served none.
/// @param server Upstream NTP server.
/// @param serve Whether to also answer client queries.
/// @returns Whether the daemon is running afterwards.
auto SetNtpServer(const std::string &server, bool serve) -> bool;

/// Whether the running time daemon is also answering clients.
auto GetNtpServing() -> bool;

// ── Users ───────────────────────────────────────────────────

struct UserInfo {
  std::string name;
  std::string role;
  std::uint32_t uid = 0;
};
auto GetUsers() -> std::vector<UserInfo>;
auto AddUser(const std::string &name,
             const std::string &role) -> bool;
auto DelUser(const std::string &name) -> bool;

// ── Logging ─────────────────────────────────────────────────

auto GetSyslog(int lines) -> std::string;

// ── System control ──────────────────────────────────────────

auto Reboot() -> void;

}  // namespace einheit::s5::sys

#endif  // EINHEIT_S5_SYS_H_
