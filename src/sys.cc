/// @file sys.cc
/// @brief System management — wraps Linux commands.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/sys.h"
#include "einheit/s5/util.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

namespace einheit::s5::sys {

using util::ReadSysfs;
using util::ReadUint;
using util::RunCmd;
using util::WriteFile;

// ── System info ─────────────────────────────────────────────

auto GetHostname() -> std::string {
  return RunCmd("hostname 2>/dev/null");
}

auto SetHostname(const std::string &name) -> bool {
  WriteFile("/etc/hostname", name + "\n");
  return RunCmd("hostname " + name + " 2>&1").empty();
}

auto GetUptime() -> std::string {
  auto raw = ReadSysfs("/proc/uptime");
  if (raw.empty()) return "unknown";
  double secs = std::stod(raw);
  int d = static_cast<int>(secs) / 86400;
  int h = (static_cast<int>(secs) % 86400) / 3600;
  int m = (static_cast<int>(secs) % 3600) / 60;
  if (d > 0) return std::format("{}d {}h {}m", d, h, m);
  if (h > 0) return std::format("{}h {}m", h, m);
  return std::format("{}m", m);
}

auto GetMemInfo() -> MemInfo {
  MemInfo info;
  std::ifstream f("/proc/meminfo");
  std::string line;
  while (std::getline(f, line)) {
    std::uint64_t val = 0;
    if (line.starts_with("MemTotal:")) {
      std::sscanf(line.c_str(), "MemTotal: %lu", &val);
      info.total_kb = val;
    } else if (line.starts_with("MemFree:")) {
      std::sscanf(line.c_str(), "MemFree: %lu", &val);
      info.free_kb = val;
    } else if (line.starts_with("MemAvailable:")) {
      std::sscanf(line.c_str(), "MemAvailable: %lu", &val);
      info.avail_kb = val;
    }
  }
  return info;
}

auto GetDiskInfo() -> DiskInfo {
  DiskInfo info;
  auto out = RunCmd("df -h / 2>/dev/null | tail -1");
  std::istringstream iss(out);
  std::string fs;
  iss >> fs >> info.size >> info.used >> info.avail
      >> info.use_pct >> info.mount;
  return info;
}

auto GetCpuTemp() -> std::string {
  auto raw = ReadSysfs(
      "/sys/class/thermal/thermal_zone0/temp");
  if (raw.empty()) return "n/a";
  int millideg = std::stoi(raw);
  return std::format("{}.{}°C", millideg / 1000,
                     (millideg % 1000) / 100);
}

// ── Network ─────────────────────────────────────────────────

auto GetInterfaces() -> std::vector<InterfaceInfo> {
  std::vector<InterfaceInfo> result;
  auto out = RunCmd("ip -o addr show 2>/dev/null");
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    // Format: 2: eth0  inet 192.168.0.138/24 ...
    std::istringstream ls(line);
    std::string idx, name, family, addr;
    ls >> idx >> name >> family >> addr;
    if (family != "inet" && family != "inet6") continue;
    if (!name.empty() && name.back() == ':')
      name.pop_back();
    // Skip link-local IPv6.
    if (addr.starts_with("fe80:")) continue;
    InterfaceInfo info;
    info.name = name;
    info.address = addr;
    auto base = "/sys/class/net/" + name + "/";
    info.state = ReadSysfs(base + "operstate");
    info.mac = ReadSysfs(base + "address");
    result.push_back(info);
  }
  return result;
}

auto SetInterfaceAddr(const std::string &iface,
                      const std::string &addr) -> bool {
  RunCmd("ip addr flush dev " + iface + " 2>&1");
  auto out = RunCmd("ip addr add " + addr + " dev " +
                    iface + " 2>&1");
  RunCmd("ip link set " + iface + " up 2>&1");
  return out.empty();
}

auto SetInterfaceDhcp(const std::string &iface) -> bool {
  RunCmd("ip addr flush dev " + iface + " 2>&1");
  auto out = RunCmd("udhcpc -i " + iface +
                    " -b -q 2>&1");
  return true;
}

// ── DNS ─────────────────────────────────────────────────────

auto GetDnsServers() -> std::vector<std::string> {
  std::vector<std::string> servers;
  std::ifstream f("/etc/resolv.conf");
  std::string line;
  while (std::getline(f, line)) {
    if (line.starts_with("nameserver ")) {
      servers.push_back(line.substr(11));
    }
  }
  return servers;
}

auto SetDnsServers(const std::vector<std::string> &servers)
    -> bool {
  std::string content;
  for (const auto &s : servers) {
    content += "nameserver " + s + "\n";
  }
  return WriteFile("/etc/resolv.conf", content);
}

// ── NTP ─────────────────────────────────────────────────────

auto GetNtpStatus() -> NtpStatus {
  NtpStatus st;
  auto out = RunCmd("ntpq -p 2>/dev/null || "
                    "busybox ntpd -q 2>&1 | head -1");
  st.server = out.empty() ? "not configured" : out;
  // Rough check — if ntpd is running, consider synced.
  auto ps = RunCmd("pidof ntpd 2>/dev/null");
  st.synced = !ps.empty();
  return st;
}

auto SetNtpServer(const std::string &server) -> bool {
  RunCmd("killall ntpd 2>/dev/null");
  auto out = RunCmd("ntpd -p " + server + " 2>&1");
  return true;
}

// ── Users ───────────────────────────────────────────────────

auto GetUsers() -> std::vector<UserInfo> {
  std::vector<UserInfo> users;
  std::ifstream f("/etc/passwd");
  std::string line;
  while (std::getline(f, line)) {
    // root:x:0:0:root:/root:/usr/bin/einheit_s5
    std::istringstream ls(line);
    std::string name, x, uid_str;
    std::getline(ls, name, ':');
    std::getline(ls, x, ':');
    std::getline(ls, uid_str, ':');
    if (name.empty()) continue;
    std::uint32_t uid = 0;
    try { uid = std::stoul(uid_str); } catch (...) {}
    // Skip system users (uid >= 1000 or root).
    if (uid != 0 && uid < 1000) continue;
    UserInfo u;
    u.name = name;
    u.uid = uid;
    u.role = (uid == 0) ? "admin" : "operator";
    users.push_back(u);
  }
  return users;
}

auto AddUser(const std::string &name,
             const std::string &role) -> bool {
  std::string group = (role == "admin") ? "root" : "users";
  auto out = RunCmd("adduser -D -G " + group + " " +
                    name + " 2>&1");
  return out.empty() || out.find("already exists") == std::string::npos;
}

auto DelUser(const std::string &name) -> bool {
  if (name == "root") return false;
  auto out = RunCmd("deluser " + name + " 2>&1");
  return out.empty();
}

// ── Logging ─────────────────────────────────────────────────

auto GetSyslog(int lines) -> std::string {
  return RunCmd(std::format(
      "tail -n {} /var/log/messages 2>/dev/null", lines));
}

// ── System control ──────────────────────────────────────────

auto Reboot() -> void {
  RunCmd("reboot");
}

}  // namespace einheit::s5::sys
