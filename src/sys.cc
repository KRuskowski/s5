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
  std::ifstream f(util::FsPath("/proc/meminfo"));
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

auto GetManagementPath() -> MgmtPath {
  MgmtPath path;
  // SSH_CONNECTION is "<client ip> <client port> <server ip> <server
  // port>". A console session has none, and a console operator cannot
  // lock themselves out of the console.
  const char *conn = std::getenv("SSH_CONNECTION");
  if (conn == nullptr) return path;
  std::istringstream iss(conn);
  std::string peer;
  iss >> peer;
  if (peer.empty()) return path;
  // Only ever fed to `ip route get`, but that is a shell line.
  for (char c : peer) {
    const bool ok = (std::isalnum(static_cast<unsigned char>(c)) != 0) ||
                    c == '.' || c == ':';
    if (!ok) return path;
  }
  path.peer = peer;
  // `ip route get 10.0.0.9` answers with the route the kernel would
  // actually use: "10.0.0.9 dev br0.10 src 10.10.0.1 uid 0", or with a
  // `via` when the answer has to be routed.
  const auto route = RunCmd("ip route get " + peer + " 2>/dev/null");
  std::istringstream ls(route);
  std::string token;
  while (ls >> token) {
    if (token == "dev") ls >> path.device;
    if (token == "src") ls >> path.address;
    if (token == "via") path.routed = true;
  }
  return path;
}

// ── DNS ─────────────────────────────────────────────────────

auto GetDnsServers() -> std::vector<std::string> {
  std::vector<std::string> servers;
  std::ifstream f(util::FsPath("/etc/resolv.conf"));
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
  // Rough check — if ntpd is running, consider synced.
  st.synced = !RunCmd("pidof ntpd 2>/dev/null").empty();
  // Recover the configured server from the running daemon's
  // command line (`ntpd -p <server>`): busybox ntpd has no query
  // interface, and probing optional tools here used to leak their
  // shell error text into the displayed value.
  auto cmdline = RunCmd(
      "p=$(pidof ntpd 2>/dev/null | cut -d' ' -f1); "
      "[ -n \"$p\" ] && tr '\\0' ' ' < \"/proc/$p/cmdline\" "
      "2>/dev/null");
  if (const auto pos = cmdline.find("-p ");
      pos != std::string::npos) {
    std::istringstream iss(cmdline.substr(pos + 3));
    iss >> st.server;
  }
  if (st.server.empty()) st.server = "not configured";
  return st;
}

auto SetNtpServer(const std::string &server, bool serve) -> bool {
  RunCmd("pkill -x ntpd 2>/dev/null; killall ntpd 2>/dev/null");
  // busybox ntpd's `-l` turns the same daemon into a server as well as
  // a client, which is why serving is not a separate process: a box
  // handing out time it never synchronised would be worse than one
  // that hands out none.
  const std::string listen = serve ? "-l " : "";
  // ntpd daemonizes on launch; the apply only counts if the daemon
  // is actually up afterwards — a box without ntpd must fail the
  // commit, not silently pretend.
  const auto out = RunCmd("ntpd " + listen + "-p " + server +
                          " 2>/dev/null; sleep 1; "
                          "pidof ntpd 2>/dev/null");
  return !out.empty();
}

auto GetNtpServing() -> bool {
  const auto cmdline = RunCmd(
      "p=$(pidof ntpd 2>/dev/null | cut -d' ' -f1); "
      "[ -n \"$p\" ] && tr '\\0' ' ' < \"/proc/$p/cmdline\" 2>/dev/null");
  return cmdline.find(" -l ") != std::string::npos;
}

// ── Users ───────────────────────────────────────────────────

auto GetUsers() -> std::vector<UserInfo> {
  std::vector<UserInfo> users;
  std::ifstream f(util::FsPath("/etc/passwd"));
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
