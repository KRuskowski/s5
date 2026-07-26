/// @file l3.cc
/// @brief SVIs and routes over iproute2 and sysctl.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/l3.h"

#include <cctype>
#include <filesystem>
#include <format>
#include <sstream>
#include <string>
#include <vector>

#include "einheit/s5/dsa.h"
#include "einheit/s5/util.h"

namespace einheit::s5::l3 {
namespace {

namespace fs = std::filesystem;
using util::ReadSysfs;
using util::RunCmd;

/// Every route we install carries this protocol tag, and nothing
/// without it is ever removed.
///
/// A private number rather than `proto static`, and that distinction
/// cost a test VM its default route: `static` means "an administrator
/// put this here" and is what ifupdown, systemd-networkd and a plain
/// `ip route add` all use. Reconciling against it made the first
/// commit that touched routing delete the box's own uplink route —
/// the switch cutting itself off, which is precisely the failure the
/// anti-lockout work exists to prevent. 201 is ours alone, so
/// "the configuration owns the static set" means the set the
/// configuration actually created.
constexpr const char *kProto = "201";

}  // namespace

auto SviName(const std::string &bridge, int vid) -> std::string {
  return std::format("{}.{}", bridge, vid);
}

auto GetSvis(const std::string &bridge) -> std::vector<Svi> {
  std::vector<Svi> out;
  const auto net = util::FsPath("/sys/class/net");
  if (!fs::exists(net)) return out;
  const std::string prefix = bridge + ".";
  std::vector<std::string> names;
  for (const auto &entry : fs::directory_iterator(net)) {
    const auto name = entry.path().filename().string();
    if (!name.starts_with(prefix)) continue;
    const auto tail = name.substr(prefix.size());
    if (tail.empty()) continue;
    bool numeric = true;
    for (char c : tail) {
      if (std::isdigit(static_cast<unsigned char>(c)) == 0) numeric = false;
    }
    if (!numeric) continue;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  for (const auto &name : names) {
    Svi svi;
    svi.device = name;
    svi.vid = static_cast<std::uint16_t>(
        std::atoi(name.substr(prefix.size()).c_str()));
    svi.up = dsa::IsUp(name);
    const auto raw =
        RunCmd(std::format("ip -o -4 addr show dev {} 2>/dev/null", name));
    std::istringstream iss(raw);
    std::string line;
    if (std::getline(iss, line)) {
      std::istringstream ls(line);
      std::string idx, dev, family;
      ls >> idx >> dev >> family >> svi.address;
      if (family != "inet") svi.address.clear();
    }
    out.push_back(svi);
  }
  return out;
}

auto AddSvi(const std::string &bridge, int vid) -> bool {
  const auto dev = SviName(bridge, vid);
  if (!fs::exists(util::FsPath("/sys/class/net/" + dev))) {
    if (!RunCmd(std::format(
                    "ip link add link {} name {} type vlan id {} 2>&1",
                    bridge, dev, vid))
             .empty()) {
      return false;
    }
  }
  // The half everyone forgets. Without `self` the bridge is not a
  // member of the VLAN it just grew an interface for: the SVI exists,
  // holds its address, answers nothing, and every diagnostic looks
  // fine.
  if (!RunCmd(std::format("bridge vlan add dev {} vid {} self 2>&1", bridge,
                          vid))
           .empty()) {
    return false;
  }
  return RunCmd(std::format("ip link set {} up 2>&1", dev)).empty();
}

auto DelSvi(const std::string &bridge, int vid) -> bool {
  const auto dev = SviName(bridge, vid);
  if (fs::exists(util::FsPath("/sys/class/net/" + dev))) {
    if (!RunCmd(std::format("ip link del {} 2>&1", dev)).empty()) {
      return false;
    }
  }
  // Best-effort: a VLAN the bridge is a member of for other reasons
  // must not make removing the SVI fail.
  RunCmd(std::format("bridge vlan del dev {} vid {} self 2>&1", bridge, vid));
  return true;
}

auto SetSviAddress(const std::string &device, const std::string &address)
    -> bool {
  RunCmd(std::format("ip -4 addr flush dev {} 2>&1", device));
  if (address.empty()) return true;
  if (!RunCmd(std::format("ip addr add {} dev {} 2>&1", address, device))
           .empty()) {
    return false;
  }
  return RunCmd(std::format("ip link set {} up 2>&1", device)).empty();
}

auto GetForwarding() -> bool {
  return ReadSysfs("/proc/sys/net/ipv4/ip_forward") == "1";
}

auto SetForwarding(bool on) -> bool {
  // sysctl is not guaranteed present on a busybox image; the procfs
  // write is, and it is the same knob.
  util::WriteFile("/proc/sys/net/ipv4/ip_forward", on ? "1\n" : "0\n");
  return GetForwarding() == on;
}

auto GetRoutes() -> std::vector<Route> {
  std::vector<Route> out;
  const auto raw = RunCmd("ip -4 route show 2>/dev/null");
  std::istringstream iss(raw);
  std::string line;
  while (std::getline(iss, line)) {
    std::istringstream ls(line);
    Route r;
    if (!(ls >> r.prefix)) continue;
    std::string token;
    while (ls >> token) {
      if (token == "via") ls >> r.via;
      if (token == "dev") ls >> r.device;
      if (token == "proto") ls >> r.origin;
    }
    // `ip route` omits `proto` for routes the kernel added implicitly
    // on address assignment; naming them is more useful to an operator
    // than a blank column. Our own private protocol number reads back
    // as what the operator called it.
    if (r.origin.empty()) r.origin = "kernel";
    // Ours reads as "config", not "static": the box may well also
    // carry a `proto static` route somebody put there with `ip route`
    // or a network unit, and calling both of them the same thing is
    // how the reconcile came to delete one of them.
    if (r.origin == kProto) {
      r.origin = "config";
      r.owned = true;
    }
    out.push_back(r);
  }
  return out;
}

auto AddRoute(const std::string &prefix, const std::string &via) -> bool {
  // `replace` rather than `add`: re-applying the committed
  // configuration must not fail because the route is already right.
  return RunCmd(std::format("ip route replace {} via {} proto {} 2>&1",
                            prefix, via, kProto))
      .empty();
}

auto DelRoute(const std::string &prefix) -> bool {
  return RunCmd(std::format("ip route del {} proto {} 2>&1", prefix, kProto))
      .empty();
}

}  // namespace einheit::s5::l3
