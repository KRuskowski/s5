/// @file fabric.cc
/// @brief Switch-fabric bootstrap over iproute2 + sysfs.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/fabric.h"

#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "einheit/s5/dsa.h"
#include "einheit/s5/util.h"

namespace einheit::s5::fabric {
namespace {

namespace fs = std::filesystem;
using cli::Error;
using dsa::IsUp;
using util::ReadSysfs;
using util::RunCmd;

auto Fail(std::string msg) -> std::unexpected<Error<FabricError>> {
  return std::unexpected(
      Error<FabricError>{FabricError::CommandFailed, std::move(msg)});
}

auto NetdevPath(const std::string &name) -> fs::path {
  return util::FsPath("/sys/class/net/" + name);
}

auto Exists(const std::string &name) -> bool {
  std::error_code ec;
  const auto path = NetdevPath(name);
  return fs::exists(path, ec) || fs::is_symlink(path, ec);
}

/// Bridge a port is enslaved to, or empty when it is routed. The kernel
/// exposes this as a symlink to the master netdev.
auto MasterOf(const std::string &name) -> std::string {
  const auto path = NetdevPath(name) / "master";
  std::error_code ec;
  if (!fs::is_symlink(path, ec)) return {};
  const auto target = fs::read_symlink(path, ec);
  if (ec) return {};
  return target.filename().string();
}

auto VlanFilteringOn(const std::string &bridge) -> bool {
  return ReadSysfs("/sys/class/net/" + bridge +
                   "/bridge/vlan_filtering") == "1";
}

/// Find the DSA conduit. The kernel links a user port to its conduit as
/// `lower_<conduit>`; when that is not present (older kernels, or a
/// non-DSA dev box) fall back to the topology's candidate names.
auto FindConduit(const Topology &topo) -> std::string {
  std::error_code ec;
  for (const auto &member : topo.members) {
    const auto dir = NetdevPath(member);
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
      const auto file = entry.path().filename().string();
      if (file.rfind("lower_", 0) != 0) continue;
      const auto name = file.substr(6);
      if (!name.empty() && name != topo.bridge) return name;
    }
  }
  for (const auto &candidate : topo.conduit_candidates) {
    if (Exists(candidate)) return candidate;
  }
  return {};
}

/// Run one `ip` command; empty output means success (iproute2 is quiet
/// on success and prints to stderr, which the caller redirects in).
auto Ip(const std::string &args) -> bool {
  return RunCmd("ip " + args + " 2>&1").empty();
}

}  // namespace

auto S5Topology() -> const Topology & {
  static const Topology topo;
  return topo;
}

auto Ensure(const Topology &topo)
    -> std::expected<void, Error<FabricError>> {
  // 1. The bridge itself. Without vlan_filtering every `bridge vlan`
  //    entry is accepted and then ignored, which is the worst failure
  //    mode available: a VLAN configuration that commits and does
  //    nothing.
  if (!Exists(topo.bridge)) {
    if (!Ip(std::format("link add name {} type bridge", topo.bridge))) {
      return Fail(std::format("creating bridge {} failed", topo.bridge));
    }
  }
  if (!VlanFilteringOn(topo.bridge)) {
    if (!Ip(std::format("link set {} type bridge vlan_filtering 1",
                        topo.bridge))) {
      return Fail(std::format("enabling vlan_filtering on {} failed",
                              topo.bridge));
    }
  }

  // 2. The CPU-port conduit. DSA brings it up on its own when a user
  //    port opens, so a missing conduit is not fatal — but doing it
  //    explicitly means the fabric is up even if no port ever opens.
  if (const auto conduit = FindConduit(topo);
      !conduit.empty() && !IsUp(conduit)) {
    if (!Ip(std::format("link set {} up", conduit))) {
      return Fail(std::format("bringing conduit {} up failed", conduit));
    }
  }

  // 3. Switch ports into the bridge.
  //
  //    Enslavement only — NEVER port admin state. `ports.<p>.enabled`
  //    is configuration, and a fabric that brought every member up
  //    would re-enable, on every single CLI invocation, a port the
  //    operator deliberately shut. An unconfigured box gets its ports
  //    turned on by the factory configuration the boot apply seeds,
  //    which is config too, so exactly one thing owns admin state.
  for (const auto &member : topo.members) {
    if (!Exists(member)) continue;  // reported by GetStatus, not fatal
    if (MasterOf(member) != topo.bridge) {
      if (!Ip(std::format("link set {} master {}", member, topo.bridge))) {
        return Fail(std::format("enslaving {} to {} failed", member,
                                topo.bridge));
      }
    }
  }

  // 4. Routed ports stay out of the bridge. Converging this too means
  //    a box someone bridged by hand comes back to the shipped
  //    topology instead of quietly keeping a bridged uplink.
  for (const auto &port : topo.routed) {
    if (!Exists(port)) continue;
    if (!MasterOf(port).empty()) {
      if (!Ip(std::format("link set {} nomaster", port))) {
        return Fail(std::format("detaching {} from its bridge failed",
                                port));
      }
    }
  }

  // 5. The bridge last: it has members by now, so it comes up carrying
  //    traffic rather than as an empty device.
  if (!IsUp(topo.bridge)) {
    if (!Ip(std::format("link set {} up", topo.bridge))) {
      return Fail(std::format("bringing {} up failed", topo.bridge));
    }
  }
  return {};
}

auto GetStatus(const Topology &topo) -> Status {
  Status st;
  st.bridge = topo.bridge;
  st.exists = Exists(topo.bridge);
  st.vlan_filtering = VlanFilteringOn(topo.bridge);
  st.up = IsUp(topo.bridge);
  st.conduit = FindConduit(topo);
  for (const auto &member : topo.members) {
    if (!Exists(member)) {
      st.absent.push_back(member);
    } else if (MasterOf(member) == topo.bridge) {
      st.enslaved.push_back(member);
    } else {
      st.detached.push_back(member);
    }
  }
  for (const auto &port : topo.routed) {
    if (Exists(port)) st.routed.push_back(port);
  }
  return st;
}

}  // namespace einheit::s5::fabric
