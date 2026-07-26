/// @file dsa.cc
/// @brief Linux DSA interface — sysfs + bridge commands.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/dsa.h"
#include "einheit/s5/util.h"

#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

namespace einheit::s5::dsa {
namespace {

namespace fs = std::filesystem;
using util::ReadSysfs;
using util::ReadUint;
using util::RunCmd;

auto IsDsaPort(const std::string &name) -> bool {
  auto path = util::FsPath("/sys/class/net/" + name + "/dsa");
  return fs::exists(path) || fs::is_symlink(path);
}

}  // namespace

auto DiscoverPorts() -> std::vector<std::string> {
  std::vector<std::string> ports;
  if (!fs::exists(util::FsPath("/sys/class/net"))) return ports;
  for (const auto &entry :
       fs::directory_iterator(util::FsPath("/sys/class/net"))) {
    auto name = entry.path().filename().string();
    // DSA ports have a "dsa" symlink or start with "lan"/"wan".
    if (IsDsaPort(name) ||
        name.rfind("lan", 0) == 0 ||
        name.rfind("wan", 0) == 0) {
      ports.push_back(name);
    }
  }
  std::sort(ports.begin(), ports.end());
  return ports;
}

auto IsUp(const std::string &iface) -> bool {
  const auto flags = ReadSysfs("/sys/class/net/" + iface + "/flags");
  if (flags.size() < 3 || flags[0] != '0' ||
      (flags[1] != 'x' && flags[1] != 'X')) {
    return false;
  }
  unsigned long value = 0;
  for (std::size_t i = 2; i < flags.size(); ++i) {
    const char c = flags[i];
    unsigned digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<unsigned>(c - 'a') + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<unsigned>(c - 'A') + 10;
    } else {
      return false;
    }
    value = value * 16 + digit;
  }
  // IFF_UP
  return (value & 0x1U) != 0;
}

auto GetPortStatus(const std::string &port) -> PortStatus {
  PortStatus st;
  st.name = port;
  auto base = "/sys/class/net/" + port + "/";
  auto operstate = ReadSysfs(base + "operstate");
  st.link = (operstate == "up");
  st.enabled = IsUp(port);
  st.speed = ReadSysfs(base + "speed");
  if (st.speed == "-1" || st.speed.empty())
    st.speed = "-";
  st.duplex = ReadSysfs(base + "duplex");
  if (st.duplex == "unknown" || st.duplex.empty())
    st.duplex = "-";
  return st;
}

auto GetPortCounters(const std::string &port) -> PortCounters {
  PortCounters c;
  c.name = port;
  auto base = "/sys/class/net/" + port + "/statistics/";
  c.rx_bytes = ReadUint(base + "rx_bytes");
  c.tx_bytes = ReadUint(base + "tx_bytes");
  c.rx_packets = ReadUint(base + "rx_packets");
  c.tx_packets = ReadUint(base + "tx_packets");
  c.rx_errors = ReadUint(base + "rx_errors");
  c.tx_errors = ReadUint(base + "tx_errors");
  return c;
}

auto SetPortEnabled(const std::string &port, bool up) -> bool {
  auto cmd = "ip link set " + port +
             (up ? " up" : " down") + " 2>&1";
  auto out = RunCmd(cmd);
  return out.empty();
}

auto GetVlans() -> std::vector<VlanEntry> {
  std::vector<VlanEntry> vlans;
  auto out = RunCmd("bridge vlan show 2>/dev/null");
  std::istringstream iss(out);
  std::string line;
  std::string current_port;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    // Lines look like:
    //   lan1   100 PVID Egress Untagged
    //          200
    // or:
    //   lan1    100 PVID Egress Untagged
    std::istringstream ls(line);
    std::string first;
    ls >> first;
    std::uint16_t vid = 0;
    if (std::isdigit(first[0])) {
      vid = static_cast<std::uint16_t>(std::stoi(first));
    } else {
      current_port = first;
      std::string vid_str;
      ls >> vid_str;
      if (vid_str.empty() || !std::isdigit(vid_str[0]))
        continue;
      vid = static_cast<std::uint16_t>(std::stoi(vid_str));
    }
    VlanEntry v;
    v.vid = vid;
    v.port = current_port;
    std::string flag;
    while (ls >> flag) {
      if (flag == "PVID") v.pvid = true;
      if (flag == "Untagged") v.untagged = true;
    }
    vlans.push_back(v);
  }
  return vlans;
}

auto AddVlan(const std::string &port, std::uint16_t vid,
             bool untagged, bool pvid) -> bool {
  auto cmd = "bridge vlan add dev " + port +
             " vid " + std::to_string(vid);
  if (untagged) cmd += " untagged";
  if (pvid) cmd += " pvid";
  cmd += " 2>&1";
  return RunCmd(cmd).empty();
}

auto DelVlan(const std::string &port, std::uint16_t vid)
    -> bool {
  auto cmd = "bridge vlan del dev " + port +
             " vid " + std::to_string(vid) + " 2>&1";
  return RunCmd(cmd).empty();
}

auto GetMacTable() -> std::vector<MacEntry> {
  std::vector<MacEntry> macs;
  auto out = RunCmd("bridge fdb show dynamic 2>/dev/null");
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    // Format: aa:bb:cc:dd:ee:ff dev lan1 vlan 100 ...
    std::istringstream ls(line);
    MacEntry e;
    std::string token;
    ls >> e.mac;
    while (ls >> token) {
      if (token == "dev") ls >> e.port;
      if (token == "vlan") {
        std::string vid_str;
        ls >> vid_str;
        if (!vid_str.empty())
          e.vid = static_cast<std::uint16_t>(
              std::stoi(vid_str));
      }
    }
    if (!e.mac.empty() && !e.port.empty())
      macs.push_back(e);
  }
  return macs;
}

}  // namespace einheit::s5::dsa
