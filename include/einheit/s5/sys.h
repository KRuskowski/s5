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
auto SetNtpServer(const std::string &server) -> bool;

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
