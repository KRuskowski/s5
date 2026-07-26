/// @file test_backend.cc
/// @brief S5Backend unit tests against a fake box, plus the
/// schema-driven exerciser over the real backend.
// Copyright (c) 2026 Einheit Networks

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/confd/boot_report.h"
#include "einheit/cli/confd/exerciser.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/schema.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/s5/backend.h"
#include "einheit/s5/dnsmasq.h"
#include "einheit/s5/dsa.h"
#include "einheit/s5/fabric.h"
#include "einheit/s5/lldp.h"
#include "einheit/s5/svc.h"
#include "einheit/s5/service.h"
#include "einheit/s5/util.h"

namespace einheit::s5 {
namespace {

namespace fs = std::filesystem;
using cli::confd::ApplyError;
using cli::confd::Candidate;

/// Fake box: canned command output by substring match (first rule
/// wins, default success/""), every command recorded; a scratch
/// tree stands in for /sys, /proc and /etc.
class FakeBox {
 public:
  FakeBox() {
    service::ResetCachesForTesting();
    root_ = fs::temp_directory_path() /
            std::format("s5-fakebox-{}", ::getpid());
    fs::create_directories(root_ / "etc");
    fs::create_directories(root_ / "proc");
    // procfs sysctls the kernel always has. Without ip_forward here,
    // `routing.enabled` fails its read-back and every commit that
    // touches routing looks like a hardware rejection.
    fs::create_directories(root_ / "proc/sys/net/ipv4");
    std::ofstream(root_ / "proc/sys/net/ipv4/ip_forward") << "0\n";
    util::SetFsRoot(root_.string());
    util::SetCmdRunner([this](const std::string &cmd) {
      commands_.push_back(cmd);
      // Explicit rules win, so a test can still pin any answer.
      for (const auto &[needle, out] : rules_) {
        if (cmd.find(needle) != std::string::npos) return out;
      }
      if (auto svc = FakeService(cmd)) return *svc;
      return std::string();
    });
  }

  /// Whether the fake box currently believes `name` is running.
  auto ServiceRunning(const std::string &name) const -> bool {
    return procs_.contains(name);
  }

  /// Pretend a service is already running before the apply.
  auto StartService(const std::string &name) -> void {
    procs_.insert(name);
  }

  ~FakeBox() {
    service::ResetCachesForTesting();
    util::SetCmdRunner({});
    util::SetFsRoot("");
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  /// Commands containing `needle` return `output`.
  auto On(std::string needle, std::string output) -> void {
    rules_.emplace_back(std::move(needle), std::move(output));
  }

  // Real kernel flag words, measured on the s5 test VM. A routed port
  // reads 0x1003/0x1002 and an enslaved one 0x1303/0x1302 — the bits
  // above IFF_UP move with the port's role, which is why anything that
  // compares whole flag words is wrong.
  static constexpr const char *kFlagsUp = "0x1003";
  static constexpr const char *kFlagsDown = "0x1002";
  static constexpr const char *kFlagsSlaveUp = "0x1303";
  static constexpr const char *kFlagsSlaveDown = "0x1302";

  auto AddPort(const std::string &name, bool up = true) -> void {
    const auto dir = root_ / "sys/class/net" / name;
    fs::create_directories(dir);
    std::ofstream(dir / "operstate") << (up ? "up" : "down");
    SetFlags(name, up ? kFlagsUp : kFlagsDown);
    std::ofstream(dir / "speed") << "1000";
    std::ofstream(dir / "duplex") << "full";
  }

  auto SetSpeed(const std::string &name, const std::string &speed)
      -> void {
    const auto dir = root_ / "sys/class/net" / name;
    fs::create_directories(dir);
    std::ofstream(dir / "speed") << speed;
  }

  auto SetPortAddress(const std::string &name,
                      const std::string &mac) -> void {
    const auto dir = root_ / "sys/class/net" / name;
    fs::create_directories(dir);
    std::ofstream(dir / "address") << mac;
  }

  auto SetMtu(const std::string &name, int mtu) -> void {
    const auto dir = root_ / "sys/class/net" / name;
    fs::create_directories(dir);
    std::ofstream(dir / "mtu") << mtu;
  }

  auto SetCounter(const std::string &name, const std::string &field,
                  std::uint64_t value) -> void {
    const auto dir = root_ / "sys/class/net" / name / "statistics";
    fs::create_directories(dir);
    std::ofstream(dir / field) << value;
  }

  auto SetBridgeAttr(const std::string &attr, const std::string &value)
      -> void {
    const auto dir = root_ / "sys/class/net/br0/bridge";
    fs::create_directories(dir);
    std::ofstream(dir / attr) << value;
  }

  auto SetFlags(const std::string &name, const std::string &flags)
      -> void {
    const auto dir = root_ / "sys/class/net" / name;
    fs::create_directories(dir);
    std::ofstream(dir / "flags") << flags;
  }

  /// A VLAN-aware bridge netdev, as the fabric bootstrap would leave it.
  auto AddBridge(const std::string &name, bool vlan_filtering = true,
                 bool up = true) -> void {
    const auto dir = root_ / "sys/class/net" / name;
    fs::create_directories(dir / "bridge");
    std::ofstream(dir / "flags") << (up ? "0x1003" : "0x1002");
    std::ofstream(dir / "bridge/vlan_filtering")
        << (vlan_filtering ? "1" : "0");
  }

  /// The kernel exposes enslavement as a symlink to the master netdev,
  /// and enslaving also changes the port's flags word.
  auto Enslave(const std::string &port,
               const std::string &bridge) -> void {
    const auto dir = root_ / "sys/class/net" / port;
    fs::create_directories(dir);
    std::error_code ec;
    fs::remove(dir / "master", ec);
    fs::create_symlink("../" + bridge, dir / "master", ec);
    const auto flags = ReadFlags(port);
    SetFlags(port, flags == kFlagsDown ? kFlagsSlaveDown
                                       : kFlagsSlaveUp);
  }

  auto ReadFlags(const std::string &name) -> std::string {
    std::ifstream f(root_ / "sys/class/net" / name / "flags");
    std::string flags;
    std::getline(f, flags);
    return flags;
  }

  /// The s5 fabric as a converged box already holds it: bridge up with
  /// filtering on, lan1..lan4 enslaved and up, lan5 routed and up.
  auto AddConvergedFabric() -> void {
    AddBridge("br0");
    for (const auto &p : {"lan1", "lan2", "lan3", "lan4"}) {
      AddPort(p);
      Enslave(p, "br0");
    }
    AddPort("lan5");
  }

  /// Forget the box's configured state the way a power cut does: the
  /// hostname is gone and every port is back up. The durable confd
  /// store is untouched — that is the whole point of boot-restore.
  auto PowerCycle() -> void {
    std::error_code ec;
    fs::remove(root_ / "etc/hostname", ec);
    fs::remove(root_ / "etc/resolv.conf", ec);
    for (const auto &entry :
         fs::directory_iterator(root_ / "sys/class/net", ec)) {
      const auto name = entry.path().filename().string();
      if (name.rfind("lan", 0) != 0) continue;
      const bool slave = fs::is_symlink(entry.path() / "master", ec);
      std::ofstream(entry.path() / "flags")
          << (slave ? kFlagsSlaveUp : kFlagsUp);
    }
    commands_.clear();
  }

  auto ClearCommands() -> void {
    commands_.clear();
  }

  /// Position of the first command containing `needle`, for ordering
  /// assertions.
  auto IndexOf(const std::string &needle) const
      -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < commands_.size(); ++i) {
      if (commands_[i].find(needle) != std::string::npos) return i;
    }
    return std::nullopt;
  }

  auto WriteEtc(const std::string &name,
                const std::string &content) -> void {
    std::ofstream(root_ / "etc" / name) << content;
  }

  /// A generated apply artifact under /var/run/einheit.
  auto ReadRun(const std::string &name) -> std::string {
    std::ifstream f(root_ / "var/run/einheit" / name);
    std::string all((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    return all;
  }

  auto RunFileExists(const std::string &name) -> bool {
    return fs::exists(root_ / "var/run/einheit" / name);
  }

  auto WriteRun(const std::string &name, const std::string &content)
      -> void {
    fs::create_directories(root_ / "var/run/einheit");
    std::ofstream(root_ / "var/run/einheit" / name) << content;
  }

  auto ReadEtc(const std::string &name) -> std::string {
    std::ifstream f(root_ / "etc" / name);
    std::string all((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    return all;
  }

  auto Ran(const std::string &needle) const -> bool {
    for (const auto &c : commands_) {
      if (c.find(needle) != std::string::npos) return true;
    }
    return false;
  }

  auto CommandCount() const -> std::size_t {
    return commands_.size();
  }

 private:
  /// The background services the fake box knows how to run: display
  /// name, how svc:: looks the process up, and what starting it looks
  /// like on the command line. Mirrors the real table in svc.cc.
  struct FakeSvc {
    const char *name;
    /// pgrep -f pattern, or empty when svc:: uses `pidof <name>`.
    const char *match;
    /// A substring that only appears in this service's start command.
    const char *start_marker;
  };
  static constexpr FakeSvc kServices[] = {
      {"lldp", "[e]inheit_s5 --lldp-daemon", "--lldp-daemon"},
      {"dnsmasq", "", "dnsmasq --conf-file"},
      {"mdns-repeater", "", "mdns-repeater "},
  };

  /// Model start/stop/probe of a background service as STATE rather
  /// than as a canned answer. Without this a test cannot tell "the
  /// apply started the daemon" from "the apply assumed it was there" —
  /// which is exactly the failure mode svc:: exists to prevent.
  auto FakeService(const std::string &cmd) -> std::optional<std::string> {
    const auto has = [&cmd](const std::string &needle) {
      return !needle.empty() && cmd.find(needle) != std::string::npos;
    };
    // Reload first: it mentions the same pattern as a kill but must
    // not change state.
    if (has("pkill -HUP")) return std::string();
    for (const auto &s : kServices) {
      const std::string pgrep =
          s.match[0] == '\0' ? "" : std::format("pgrep -f '{}'", s.match);
      const std::string pkill_f =
          s.match[0] == '\0' ? "" : std::format("pkill -f '{}'", s.match);
      const std::string pkill_x = std::format("pkill -x {}", s.name);
      const std::string pidof = std::format("pidof {}", s.name);
      if (has(pkill_f) || has(pkill_x)) {
        procs_.erase(s.name);
        return std::string();
      }
      if (has(pgrep) || has(pidof)) {
        return procs_.contains(s.name) ? std::string("4242\n")
                                       : std::string();
      }
      if (has(s.start_marker)) {
        procs_.insert(s.name);
        return std::string();
      }
    }
    return std::nullopt;
  }

  fs::path root_;
  std::vector<std::pair<std::string, std::string>> rules_;
  std::vector<std::string> commands_;
  std::set<std::string> procs_;
};

auto MakeCandidate(
    std::vector<std::pair<std::string, std::string>> kv)
    -> Candidate {
  Candidate c;
  for (auto &[k, v] : kv) c.values[k] = v;
  return c;
}

TEST(S5Backend, AppliesHostnameDnsAndPorts) {
  FakeBox box;
  box.AddPort("lan1");
  box.On("hostname ", "");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"hostname", "unit-sw"},
      {"dns.primary", "192.0.2.53"},
      {"dns.secondary", "192.0.2.54"},
      {"ports.lan1.enabled", "false"},
  }));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(box.Ran("hostname unit-sw"));
  EXPECT_TRUE(box.Ran("ip link set lan1 down"));
  const auto resolv = box.ReadEtc("resolv.conf");
  EXPECT_NE(resolv.find("nameserver 192.0.2.53"),
            std::string::npos);
  EXPECT_NE(resolv.find("nameserver 192.0.2.54"),
            std::string::npos);
  EXPECT_EQ(box.ReadEtc("hostname"), "unit-sw\n");
}

TEST(S5Backend, ValidationRejectsBeforeAnyWrite) {
  FakeBox box;
  S5Backend backend(MakeS5Schema());
  const auto before = box.CommandCount();
  auto r = backend.Apply(MakeCandidate({
      {"hostname", "would-apply"},
      {"ports.lan1.vlan.5000", "tagged"},
  }));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  // Nothing may have touched the box.
  EXPECT_EQ(box.CommandCount(), before);
  EXPECT_EQ(box.ReadEtc("hostname"), "");
}

TEST(S5Backend, PoeWithoutBusRejectsAtValidation) {
  FakeBox box;
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(
      MakeCandidate({{"poe.1.enabled", "true"}}));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
}

TEST(S5Backend, FirstFailureIsHardwareRejected) {
  FakeBox box;
  box.On("hostname ", "hostname: you must be root\n");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(
      MakeCandidate({{"hostname", "nope"}}));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::HardwareRejected);
}

TEST(S5Backend, FailureAfterAWriteIsPartialApply) {
  FakeBox box;
  // Hostname (applied first) succeeds; the port write fails.
  box.On("ip link set lan9 down", "Cannot find device \"lan9\"\n");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"hostname", "half"},
      {"ports.lan9.enabled", "false"},
  }));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::PartialApply);
}

TEST(S5Backend, VlanReconciliationAddsUpdatesAndRemoves) {
  FakeBox box;
  box.AddPort("lan1");
  // The box currently holds vid 1 (default) and vid 20 tagged.
  box.On("bridge vlan show",
         "port    vlan-id\n"
         "lan1    1 PVID Egress Untagged\n"
         "        20\n");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      // Desired: keep 20 but as untagged-pvid, add 30; vid 1 goes.
      {"ports.lan1.vlan.20", "untagged-pvid"},
      {"ports.lan1.vlan.30", "tagged"},
  }));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(box.Ran("bridge vlan del dev lan1 vid 1"));
  EXPECT_TRUE(
      box.Ran("bridge vlan add dev lan1 vid 20 untagged pvid"));
  EXPECT_TRUE(box.Ran("bridge vlan add dev lan1 vid 30"));
}

TEST(S5Backend, VlanAlreadyCorrectIsNotRewritten) {
  FakeBox box;
  box.AddPort("lan1");
  box.On("bridge vlan show",
         "port    vlan-id\n"
         "lan1    10 PVID Egress Untagged\n");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate(
      {{"ports.lan1.vlan.10", "untagged-pvid"}}));
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(box.Ran("bridge vlan add"));
  EXPECT_FALSE(box.Ran("bridge vlan del"));
}

TEST(S5Backend, ReadRunningReflectsTheBox) {
  FakeBox box;
  box.AddPort("lan1", /*up=*/true);
  box.AddPort("lan2", /*up=*/false);
  box.On("hostname", "unit-sw\n");
  box.On("bridge vlan show",
         "port    vlan-id\n"
         "lan1    10 PVID Egress Untagged\n"
         "br0     1 PVID Egress Untagged\n");
  box.WriteEtc("resolv.conf",
               "nameserver 192.0.2.53\nnameserver 192.0.2.54\n");
  S5Backend backend(MakeS5Schema());
  const auto running = backend.ReadRunning();
  EXPECT_EQ(running.at("hostname"), "unit-sw");
  EXPECT_EQ(running.at("dns.primary"), "192.0.2.53");
  EXPECT_EQ(running.at("dns.secondary"), "192.0.2.54");
  EXPECT_EQ(running.at("ports.lan1.enabled"), "true");
  EXPECT_EQ(running.at("ports.lan2.enabled"), "false");
  EXPECT_EQ(running.at("ports.lan1.vlan.10"), "untagged-pvid");
  // The bridge device itself is not a switch port; no PoE bus, no
  // poe paths; ntp probe found nothing worth recording.
  EXPECT_FALSE(running.contains("ports.br0.vlan.1"));
  for (const auto &[k, v] : running) {
    EXPECT_EQ(k.rfind("poe.", 0), std::string::npos) << k;
    EXPECT_EQ(k.rfind("ntp.", 0), std::string::npos) << k;
  }
}

// ── Fabric bootstrap ────────────────────────────────────────────

TEST(S5Fabric, ApplyBuildsTheBridgeOnABareBox) {
  FakeBox box;
  for (const auto &p : {"lan1", "lan2", "lan3", "lan4", "lan5"}) {
    box.AddPort(p);
  }
  S5Backend backend(MakeS5Schema());
  // Any candidate at all: the fabric is a precondition of applying
  // anything, not a subtree of the schema.
  auto r = backend.Apply(MakeCandidate({{"hostname", "bare"}}));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(box.Ran("ip link add name br0 type bridge"));
  EXPECT_TRUE(box.Ran("ip link set br0 type bridge vlan_filtering 1"));
  for (const auto &p : {"lan1", "lan2", "lan3", "lan4"}) {
    EXPECT_TRUE(box.Ran(std::format("ip link set {} master br0", p))) << p;
  }
  EXPECT_TRUE(box.Ran("ip link set br0 up"));
  // lan5 is the uplink: it comes up, but it must not be bridged.
  EXPECT_FALSE(box.Ran("ip link set lan5 master br0"));
}

TEST(S5Fabric, ConvergedFabricIsNotRewritten) {
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({{"hostname", "converged"}}));
  ASSERT_TRUE(r.has_value());
  // Idempotence is what makes it safe to run before every apply.
  EXPECT_FALSE(box.Ran("ip link add"));
  EXPECT_FALSE(box.Ran("vlan_filtering"));
  EXPECT_FALSE(box.Ran("master br0"));
  EXPECT_FALSE(box.Ran("nomaster"));
  EXPECT_FALSE(box.Ran("ip link set br0 up"));
}

TEST(S5Fabric, MissingVlanFilteringIsTurnedOn) {
  FakeBox box;
  box.AddConvergedFabric();
  // A bridge without filtering accepts every `bridge vlan` entry and
  // then ignores it — a VLAN config that commits and does nothing.
  box.AddBridge("br0", /*vlan_filtering=*/false);
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({{"hostname", "unfiltered"}}));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(box.Ran("ip link set br0 type bridge vlan_filtering 1"));
}

TEST(S5Fabric, FabricIsBuiltBeforeAnyVlanWrite) {
  FakeBox box;
  for (const auto &p : {"lan1", "lan2", "lan3", "lan4", "lan5"}) {
    box.AddPort(p);
  }
  box.On("bridge vlan show", "port    vlan-id\n");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(
      MakeCandidate({{"ports.lan1.vlan.100", "untagged-pvid"}}));
  ASSERT_TRUE(r.has_value());
  const auto enslave = box.IndexOf("ip link set lan1 master br0");
  const auto vlan = box.IndexOf("bridge vlan add dev lan1");
  ASSERT_TRUE(enslave.has_value());
  ASSERT_TRUE(vlan.has_value());
  // `bridge vlan add` on an unbridged port fails outright, so the order
  // here is the difference between a working commit and a broken one.
  EXPECT_LT(*enslave, *vlan);
}

TEST(S5Fabric, ABridgedUplinkIsReturnedToRouted) {
  FakeBox box;
  box.AddConvergedFabric();
  // Someone bridged the uplink by hand. Converging it back is the
  // point of owning the topology: Phase 2/3 put the WAN on lan5.
  box.Enslave("lan5", "br0");
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({{"hostname", "fixup"}}));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(box.Ran("ip link set lan5 nomaster"));
}

TEST(S5Fabric, BringsTheBridgeUpButNeverTouchesPortAdminState) {
  FakeBox box;
  box.AddBridge("br0", true, /*up=*/false);
  for (const auto &p : {"lan1", "lan2", "lan3", "lan4"}) {
    box.AddPort(p, /*up=*/false);
    box.Enslave(p, "br0");
  }
  box.AddPort("lan5", /*up=*/false);
  box.AddPort("eth0", /*up=*/false);  // the DSA conduit
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({{"hostname", "wake-up"}}));
  ASSERT_TRUE(r.has_value());
  // The bridge and the CPU-path conduit are fabric: not configurable,
  // so the fabric owns them.
  EXPECT_TRUE(box.Ran("ip link set br0 up"));
  EXPECT_TRUE(box.Ran("ip link set eth0 up"));
  // Port admin state is `ports.<p>.enabled` — config. A fabric that
  // brought members up would silently re-enable a port the operator
  // shut, on every single CLI invocation.
  for (const auto &p : {"lan1", "lan2", "lan3", "lan4", "lan5"}) {
    EXPECT_FALSE(box.Ran(std::format("ip link set {} up", p))) << p;
  }
}

TEST(S5Fabric, EnslavingDoesNotReEnableAShutPort) {
  // The regression in full: a down port that still has to be brought
  // into the bridge gets enslaved, and stays down.
  FakeBox box;
  box.AddBridge("br0");
  box.AddPort("lan1", /*up=*/false);  // shut, and not yet enslaved
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({{"hostname", "shut-port"}}));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(box.Ran("ip link set lan1 master br0"));
  EXPECT_FALSE(box.Ran("ip link set lan1 up"));
  EXPECT_EQ(backend.ReadRunning().at("ports.lan1.enabled"), "false");
}

TEST(S5Fabric, AbsentPortsAreSkippedNotFatal) {
  FakeBox box;
  // A dev board with two ports still has to work.
  box.AddBridge("br0");
  box.AddPort("lan1");
  box.Enslave("lan1", "br0");
  box.AddPort("lan2");
  box.Enslave("lan2", "br0");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({{"hostname", "small-box"}}));
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(box.Ran("ip link set lan3"));
  const auto st = fabric::GetStatus(fabric::S5Topology());
  EXPECT_EQ(st.enslaved.size(), 2u);
  ASSERT_EQ(st.absent.size(), 2u);
  EXPECT_EQ(st.absent[0], "lan3");
  EXPECT_EQ(st.absent[1], "lan4");
}

TEST(S5Fabric, AFailedFabricCommandFailsTheApply) {
  FakeBox box;
  for (const auto &p : {"lan1", "lan2", "lan3", "lan4"}) box.AddPort(p);
  box.On("ip link add name br0", "RTNETLINK answers: Operation not "
                                 "permitted\n");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({{"hostname", "no-fabric"}}));
  ASSERT_FALSE(r.has_value());
  // Nothing of the candidate was written, so this is a clean rejection
  // rather than a half-applied box.
  EXPECT_EQ(r.error().code, ApplyError::HardwareRejected);
  EXPECT_NE(r.error().message.find("fabric bootstrap"),
            std::string::npos);
  EXPECT_EQ(box.ReadEtc("hostname"), "");
}

TEST(S5Fabric, StatusReportsDetachedMembersAndTheConduit) {
  FakeBox box;
  box.AddBridge("br0");
  box.AddPort("lan1");
  box.Enslave("lan1", "br0");
  box.AddPort("lan2");  // exists, not enslaved
  box.AddPort("lan3");
  box.Enslave("lan3", "br0");
  box.AddPort("lan4");
  box.Enslave("lan4", "br0");
  box.AddPort("lan5");
  box.AddPort("eth0");  // the DSA conduit candidate

  const auto st = fabric::GetStatus(fabric::S5Topology());
  EXPECT_EQ(st.bridge, "br0");
  EXPECT_TRUE(st.exists);
  EXPECT_TRUE(st.vlan_filtering);
  EXPECT_TRUE(st.up);
  EXPECT_EQ(st.conduit, "eth0");
  ASSERT_EQ(st.detached.size(), 1u);
  EXPECT_EQ(st.detached[0], "lan2");
  EXPECT_TRUE(st.absent.empty());
  ASSERT_EQ(st.routed.size(), 1u);
  EXPECT_EQ(st.routed[0], "lan5");
}

TEST(S5Fabric, EnsureFabricIsCallableWithoutACandidate) {
  // The boot path needs the fabric even on a box with no commit
  // history to restore.
  FakeBox box;
  for (const auto &p : {"lan1", "lan2", "lan3", "lan4", "lan5"}) {
    box.AddPort(p);
  }
  S5Backend backend(MakeS5Schema());
  ASSERT_TRUE(backend.EnsureFabric().has_value());
  EXPECT_TRUE(box.Ran("ip link add name br0 type bridge"));
  EXPECT_TRUE(box.Ran("ip link set br0 type bridge vlan_filtering 1"));
}

// ── Boot-restore over the real backend ──────────────────────────

/// A confd state directory that outlives a simulated reboot.
struct StateDir {
  fs::path path;
  StateDir()
      : path(fs::temp_directory_path() /
             std::format("s5-boot-state-{}", ::getpid())) {
    fs::remove_all(path);
  }
  ~StateDir() {
    fs::remove_all(path);
  }
  auto str() const -> std::string {
    return path.string();
  }
};

TEST(S5BootRestore, ReappliesTheCommittedConfigurationToTheBox) {
  FakeBox box;
  box.AddConvergedFabric();
  StateDir state;
  S5Backend backend(MakeS5Schema());
  cli::confd::RuntimeOptions opts;
  opts.state_dir = state.str();

  {
    cli::confd::Runtime rt(backend, opts);
    cli::protocol::Request req;
    req.user = "root";
    req.role = "admin";
    req.command = "configure";
    auto opened = rt.HandleRequest(req);
    ASSERT_EQ(opened.status, cli::protocol::ResponseStatus::Ok);
    const std::string session(opened.data.begin(), opened.data.end());
    for (const auto &kv : std::vector<std::vector<std::string>>{
             {"hostname", "bootcheck"},
             {"ports.lan2.enabled", "false"}}) {
      cli::protocol::Request set = req;
      set.command = "set";
      set.args = kv;
      set.session_id = session;
      ASSERT_EQ(rt.HandleRequest(set).status,
                cli::protocol::ResponseStatus::Ok)
          << kv[0];
    }
    cli::protocol::Request commit = req;
    commit.command = "commit";
    commit.session_id = session;
    ASSERT_EQ(rt.HandleRequest(commit).status,
              cli::protocol::ResponseStatus::Ok);
  }
  ASSERT_EQ(box.ReadEtc("hostname"), "bootcheck\n");

  // Power cut: the box forgets, the durable store does not.
  box.PowerCycle();
  ASSERT_EQ(box.ReadEtc("hostname"), "");

  cli::confd::Runtime rt(backend, opts);
  // The box came up with lan2 back UP, so constructor reconciliation
  // (reality wins per key) now disagrees with the commit. Boot-restore
  // has to push intent back over it — otherwise a reboot silently
  // un-does every port the operator disabled.
  EXPECT_EQ(rt.Running().at("ports.lan2.enabled"), "true");
  auto restored = rt.ApplyRunningAtBoot();
  ASSERT_TRUE(restored.has_value())
      << (restored ? "" : restored.error().message);
  EXPECT_TRUE(restored->applied);
  EXPECT_EQ(box.ReadEtc("hostname"), "bootcheck\n");
  EXPECT_TRUE(box.Ran("ip link set lan2 down"));
  EXPECT_EQ(rt.Running().at("ports.lan2.enabled"), "false");
}

TEST(S5BootRestore, ABoxWithNoCommitsRestoresNothing) {
  FakeBox box;
  box.AddConvergedFabric();
  StateDir state;
  S5Backend backend(MakeS5Schema());
  cli::confd::RuntimeOptions opts;
  opts.state_dir = state.str();
  cli::confd::Runtime rt(backend, opts);
  auto restored = rt.ApplyRunningAtBoot();
  ASSERT_TRUE(restored.has_value());
  EXPECT_FALSE(restored->applied);
  EXPECT_EQ(box.ReadEtc("hostname"), "");
}

TEST(S5BootRestore, SaveAndLoadRoundTripThroughTheConfigDirectory) {
  FakeBox box;
  box.AddConvergedFabric();
  StateDir state;
  S5Backend backend(MakeS5Schema());
  cli::confd::RuntimeOptions opts;
  opts.state_dir = state.str();
  cli::confd::Runtime rt(backend, opts);

  cli::protocol::Request req;
  req.user = "root";
  req.role = "admin";
  const auto configure = [&]() -> std::string {
    cli::protocol::Request r = req;
    r.command = "configure";
    auto resp = rt.HandleRequest(r);
    EXPECT_EQ(resp.status, cli::protocol::ResponseStatus::Ok);
    return std::string(resp.data.begin(), resp.data.end());
  };
  const auto send = [&](const std::string &command,
                        std::vector<std::string> args,
                        const std::string &session)
      -> cli::protocol::Response {
    cli::protocol::Request r = req;
    r.command = command;
    r.args = std::move(args);
    if (!session.empty()) r.session_id = session;
    return rt.HandleRequest(r);
  };

  auto s1 = configure();
  ASSERT_EQ(send("set", {"hostname", "golden-box"}, s1).status,
            cli::protocol::ResponseStatus::Ok);
  ASSERT_EQ(send("commit", {}, s1).status,
            cli::protocol::ResponseStatus::Ok);
  ASSERT_EQ(send("save", {"golden"}, "").status,
            cli::protocol::ResponseStatus::Ok);

  auto s2 = configure();
  ASSERT_EQ(send("set", {"hostname", "drifted"}, s2).status,
            cli::protocol::ResponseStatus::Ok);
  ASSERT_EQ(send("commit", {}, s2).status,
            cli::protocol::ResponseStatus::Ok);
  ASSERT_EQ(box.ReadEtc("hostname"), "drifted\n");

  auto s3 = configure();
  ASSERT_EQ(send("load_replace", {"golden"}, s3).status,
            cli::protocol::ResponseStatus::Ok);
  ASSERT_EQ(send("commit", {}, s3).status,
            cli::protocol::ResponseStatus::Ok);
  EXPECT_EQ(box.ReadEtc("hostname"), "golden-box\n");
}

TEST(S5Backend, EnslavedDownPortReadsBackAsDisabled) {
  // Regression: admin state was read by comparing the whole sysfs
  // flags word against "0x1002". Enslaving a port changes the bits
  // above IFF_UP (0x1302 down, 0x1303 up), so once the fabric
  // bootstrap started bridging lan1..lan4 that comparison reported
  // every down switch port as enabled. Boot-restore then reconciled
  // the operator's `enabled false` away, and the next commit carried
  // `true` forward — a disabled port silently coming back up.
  FakeBox box;
  box.AddPort("lan1", /*up=*/false);
  box.Enslave("lan1", "br0");
  box.AddPort("lan2", /*up=*/true);
  box.Enslave("lan2", "br0");
  box.AddPort("lan5", /*up=*/false);  // routed: the old code got this right
  S5Backend backend(MakeS5Schema());
  const auto running = backend.ReadRunning();
  EXPECT_EQ(running.at("ports.lan1.enabled"), "false");
  EXPECT_EQ(running.at("ports.lan2.enabled"), "true");
  EXPECT_EQ(running.at("ports.lan5.enabled"), "false");
  EXPECT_FALSE(dsa::IsUp("lan1"));
  EXPECT_TRUE(dsa::IsUp("lan2"));
  // An unreadable or malformed flags word counts as down, so the
  // fabric errs towards issuing a redundant `ip link set ... up`.
  EXPECT_FALSE(dsa::IsUp("no-such-port"));
  box.SetFlags("lan3", "garbage");
  EXPECT_FALSE(dsa::IsUp("lan3"));
}

// ── WP0.6 divergence alarm ──────────────────────────────────────

/// The `config-divergence` row of `show system`, driven by the boot
/// report the framework persists.
auto SystemRow(const std::string &field) -> std::string {
  cli::protocol::Request req;
  req.command = "show_system";
  req.user = "root";
  req.role = "admin";
  auto resp = service::HandleProduct(req);
  EXPECT_TRUE(resp.has_value());
  if (!resp) return {};
  const std::string body(resp->data.begin(), resp->data.end());
  std::istringstream iss(body);
  std::string line;
  while (std::getline(iss, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    if (line.substr(0, tab) == field) return line.substr(tab + 1);
  }
  return {};
}

TEST(S5Divergence, RowReadsNoneWhenTheBoxCameBackAsCommitted) {
  FakeBox box;
  StateDir state;
  service::SetStateDir(state.str());
  cli::confd::BootReport rep;
  rep.boot_id = cli::confd::CurrentBootId();
  rep.ok = true;
  rep.applied_revision = 7;
  rep.reconcile_conflicts = 0;
  ASSERT_TRUE(cli::confd::SaveBootReport(state.str(), rep).has_value());
  EXPECT_EQ(SystemRow("config-divergence"), "none");
  service::SetStateDir({});
}

TEST(S5Divergence, RowNamesHowManyPathsDrifted) {
  FakeBox box;
  StateDir state;
  service::SetStateDir(state.str());
  cli::confd::BootReport rep;
  rep.boot_id = cli::confd::CurrentBootId();
  rep.applied_revision = 12;
  rep.reconcile_conflicts = 3;
  ASSERT_TRUE(cli::confd::SaveBootReport(state.str(), rep).has_value());
  const auto row = SystemRow("config-divergence");
  EXPECT_NE(row.find("3 path(s)"), std::string::npos) << row;
  EXPECT_NE(row.find("12"), std::string::npos) << row;
  service::SetStateDir({});
}

TEST(S5Divergence, RowIsUnknownWhenBootRestoreDidNotRunThisBoot) {
  // The systemd-ordering-cycle case again, seen from `show system`:
  // a stale report must NOT be reported as "none", because that is a
  // claim we cannot make about a boot that never applied anything.
  FakeBox box;
  StateDir state;
  service::SetStateDir(state.str());
  cli::confd::BootReport rep;
  rep.boot_id = "an-earlier-boot";
  rep.applied_revision = 7;
  rep.reconcile_conflicts = 0;
  ASSERT_TRUE(cli::confd::SaveBootReport(state.str(), rep).has_value());
  const auto row = SystemRow("config-divergence");
  EXPECT_NE(row.find("unknown"), std::string::npos) << row;
  EXPECT_NE(row.find("did not run"), std::string::npos) << row;
  service::SetStateDir({});
}

TEST(S5Divergence, RowIsUnknownWithNoBootReportAtAll) {
  FakeBox box;
  StateDir state;
  service::SetStateDir(state.str());
  const auto row = SystemRow("config-divergence");
  EXPECT_NE(row.find("unknown"), std::string::npos) << row;
  service::SetStateDir({});
}

TEST(S5BootRestore, TheFabricStepReachesTheBootReport) {
  FakeBox box;
  box.AddConvergedFabric();
  StateDir state;
  S5Backend backend(MakeS5Schema());
  cli::confd::RuntimeOptions opts;
  opts.state_dir = state.str();
  cli::confd::Runtime rt(backend, opts);

  cli::confd::BootStep fabric;
  fabric.name = "fabric";
  fabric.ok = true;
  fabric.detail = "br0 up, 4 member(s) enslaved";
  ASSERT_TRUE(rt.ApplyRunningAtBoot({fabric}).has_value());
  auto rep = rt.LastBootReport();
  ASSERT_TRUE(rep.has_value());
  ASSERT_FALSE(rep->steps.empty());
  EXPECT_EQ(rep->steps[0].name, "fabric");
  EXPECT_NE(rep->steps[0].detail.find("br0"), std::string::npos);
}

// ── WP1.1 port parameters ───────────────────────────────────────

TEST(S5PortParams, SpeedAndDuplexAreForcedTogether) {
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({
      {"ports.lan1.speed", "100"},
      {"ports.lan1.duplex", "full"},
  }));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(box.Ran("ethtool -s lan1 autoneg off speed 100 duplex full"));
}

TEST(S5PortParams, AutoOnEitherHalfMeansAutonegForBoth) {
  // ethtool will not force one half of the pair, so "auto" anywhere in
  // the pair has to mean autoneg — not a half-configured forced link.
  FakeBox box;
  box.AddConvergedFabric();
  // Start from a FORCED link, so asking for auto is a real change.
  box.SetSpeed("lan1", "100");
  box.On("ethtool lan1", "Auto-negotiation: off\n");
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({{"ports.lan1.speed", "auto"}}));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(box.Ran("ethtool -s lan1 autoneg on"));
  EXPECT_FALSE(box.Ran("autoneg off"));
}

TEST(S5PortParams, UnchangedLinkParametersIssueNoCommands) {
  // The candidate is seeded from running, so every commit carries every
  // port's speed/duplex/mtu. Re-issuing those writes is not merely
  // wasteful: on a driver with no ethtool ops the no-op FAILS, and one
  // unrelated `set hostname` could no longer be committed.
  FakeBox box;
  box.AddConvergedFabric();
  box.SetMtu("lan1", 1500);
  box.On("ethtool -s", "netlink error: Operation not supported\n");
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({
      {"hostname", "unrelated-change"},
      {"ports.lan1.speed", "auto"},
      {"ports.lan1.duplex", "auto"},
      {"ports.lan1.mtu", "1500"},
      {"ports.lan1.flow_control", "false"},
  }));
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  EXPECT_FALSE(box.Ran("ethtool -s"));
  EXPECT_FALSE(box.Ran("ip link set lan1 mtu"));
  EXPECT_FALSE(box.Ran("ethtool -A"));
}

TEST(S5PortParams, MtuIsWrittenAndFlowControlToggled) {
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  box.SetMtu("lan1", 1500);
  auto r = backend.Apply(MakeCandidate({
      {"ports.lan1.mtu", "9000"},
      {"ports.lan1.flow_control", "true"},
  }));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(box.Ran("ip link set lan1 mtu 9000"));
  EXPECT_TRUE(box.Ran("ethtool -A lan1 rx on tx on"));
}

TEST(S5PortParams, RaisingAUserMtuRaisesTheConduitFirst) {
  // The DSA invariant: the conduit carries every user port's frames
  // plus the switch tag. A jumbo user port behind a 1500-byte conduit
  // silently drops on the CPU path.
  FakeBox box;
  box.AddConvergedFabric();
  box.AddPort("eth0");  // the conduit
  box.SetMtu("eth0", 1500);
  box.SetMtu("lan1", 1500);
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({{"ports.lan1.mtu", "9000"}}));
  ASSERT_TRUE(r.has_value());
  const auto conduit = box.IndexOf("ip link set eth0 mtu");
  const auto user = box.IndexOf("ip link set lan1 mtu 9000");
  ASSERT_TRUE(conduit.has_value()) << "conduit MTU was never raised";
  ASSERT_TRUE(user.has_value());
  EXPECT_LT(*conduit, *user) << "conduit must be raised BEFORE the port";
  // And it must clear the tag overhead, not merely match.
  EXPECT_TRUE(box.Ran("ip link set eth0 mtu 9008"));
}

TEST(S5PortParams, AConduitAlreadyLargeEnoughIsLeftAlone) {
  FakeBox box;
  box.AddConvergedFabric();
  box.AddPort("eth0");
  box.SetMtu("eth0", 9216);
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({{"ports.lan1.mtu", "9000"}}));
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(box.Ran("ip link set eth0 mtu"));
}

TEST(S5PortParams, OutOfRangeMtuIsRejectedBeforeAnyWrite) {
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({{"ports.lan1.mtu", "70000"}}));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
}

TEST(S5PortParams, ReadBackAlwaysRoundTripsThroughTheSchema) {
  // Found on the VM: a netdev whose ethtool output has no
  // "Auto-negotiation" line, or whose speed is not one of the
  // configurable values, made ReadRunning report something the schema
  // cannot express (`speed = "-"`). The reconcile overlay put it into
  // running config and EVERY later commit failed validating a value
  // the operator never typed.
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  const auto schema = MakeS5Schema();
  for (const auto &[path, value] : backend.ReadRunning()) {
    auto ok = cli::schema::ValidatePath(schema.Get(), path, value);
    EXPECT_TRUE(ok.has_value())
        << path << " = '" << value << "': "
        << (ok ? "" : ok.error().message);
  }
}

TEST(S5PortParams, AnUnexpressibleLinkSpeedReadsAsAuto) {
  FakeBox box;
  box.AddConvergedFabric();
  // 2.5G is a real link speed and not a configurable one here.
  box.SetSpeed("lan1", "2500");
  box.On("ethtool lan1", "Auto-negotiation: off\n");
  S5Backend backend(MakeS5Schema());
  const auto running = backend.ReadRunning();
  EXPECT_EQ(running.at("ports.lan1.speed"), "auto");
}

TEST(S5PortParams, ReadRunningReportsLinkParameters) {
  FakeBox box;
  box.AddConvergedFabric();
  box.SetMtu("lan1", 9000);
  box.On("ethtool -a lan1", "RX: on\nTX: on\n");
  S5Backend backend(MakeS5Schema());
  const auto running = backend.ReadRunning();
  EXPECT_EQ(running.at("ports.lan1.mtu"), "9000");
  EXPECT_EQ(running.at("ports.lan1.speed"), "auto");
  EXPECT_EQ(running.at("ports.lan1.flow_control"), "true");
}

// ── WP1.4 static MACs + aging ───────────────────────────────────

TEST(S5StaticMac, AddsAnEntryAndSetsAgeing) {
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({
      {"mac.aging_time", "600"},
      {"mac.static.aa:bb:cc:dd:ee:ff.port", "lan1"},
      {"mac.static.aa:bb:cc:dd:ee:ff.vlan", "10"},
  }));
  ASSERT_TRUE(r.has_value());
  // sysfs takes centiseconds; the schema and CLI talk seconds.
  EXPECT_TRUE(box.Ran("ageing_time 60000"));
  EXPECT_TRUE(box.Ran("bridge fdb replace aa:bb:cc:dd:ee:ff dev lan1 "
                      "master static vlan 10"));
}

TEST(S5StaticMac, TheCandidateOwnsTheFullStaticSet) {
  FakeBox box;
  box.AddConvergedFabric();
  // The box already holds a static entry the candidate does not name.
  box.On("bridge fdb show",
         "aa:bb:cc:dd:ee:ff dev lan1 vlan 10 master br0 permanent\n"
         "12:22:33:44:55:66 dev lan2 vlan 20 master br0 permanent\n"
         "de:ad:be:ef:00:01 dev lan3 vlan 1 master br0\n");
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({
      {"mac.static.aa:bb:cc:dd:ee:ff.port", "lan1"},
      {"mac.static.aa:bb:cc:dd:ee:ff.vlan", "10"},
  }));
  ASSERT_TRUE(r.has_value());
  // The unlisted static entry goes...
  EXPECT_TRUE(box.Ran("bridge fdb del 12:22:33:44:55:66"));
  // ...the listed one is already correct and is not rewritten...
  EXPECT_FALSE(box.Ran("bridge fdb replace aa:bb:cc:dd:ee:ff"));
  // ...and a LEARNED entry is not configuration, so it is untouched.
  EXPECT_FALSE(box.Ran("bridge fdb del de:ad:be:ef:00:01"));
}

TEST(S5StaticMac, AMalformedAddressIsRejected) {
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(
      MakeCandidate({{"mac.static.not-a-mac.port", "lan1"}}));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  // A MAC ends up in a shell line; a rejected one must never get there.
  EXPECT_FALSE(box.Ran("bridge fdb"));
}

TEST(S5StaticMac, AnEntryWithoutAPortIsRejected) {
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(
      MakeCandidate({{"mac.static.aa:bb:cc:dd:ee:ff.vlan", "10"}}));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
}

TEST(S5StaticMac, KernelOwnedPermanentEntriesAreNeverReconciled) {
  // Found on the VM: the bridge installs a permanent entry for each
  // port's own address plus the multicast groups it joins. They are
  // indistinguishable from a configured static entry by the
  // `permanent` flag alone, and trying to delete one fails — which
  // failed the whole apply, and therefore every boot.
  FakeBox box;
  box.AddConvergedFabric();
  box.SetPortAddress("lan1", "52:54:00:a5:00:01");
  box.On("bridge fdb show",
         // the port's own address, installed by the bridge
         "52:54:00:a5:00:01 dev lan1 vlan 1 master br0 permanent\n"
         // IPv6 all-nodes and an STP group address
         "33:33:00:00:00:01 dev lan1 master br0 permanent\n"
         "01:80:c2:00:00:00 dev lan1 master br0 permanent\n"
         // the netdev's own RX filter, not the bridge fdb at all
         "33:33:ff:a5:00:01 dev lan1 self permanent\n");
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({{"hostname", "no-mac-config"}}));
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  EXPECT_FALSE(box.Ran("bridge fdb del"));
  // And none of them masquerade as configuration in running.
  for (const auto &[k, v] : backend.ReadRunning()) {
    EXPECT_EQ(k.rfind("mac.static.", 0), std::string::npos) << k;
  }
}

TEST(S5StaticMac, ReadRunningReportsStaticEntriesOnly) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("bridge fdb show",
         "aa:bb:cc:dd:ee:ff dev lan1 vlan 10 master br0 permanent\n"
         "de:ad:be:ef:00:01 dev lan2 vlan 1 master br0\n");
  box.On("ageing_time", "");
  S5Backend backend(MakeS5Schema());
  const auto running = backend.ReadRunning();
  EXPECT_EQ(running.at("mac.static.aa:bb:cc:dd:ee:ff.port"), "lan1");
  EXPECT_EQ(running.at("mac.static.aa:bb:cc:dd:ee:ff.vlan"), "10");
  // Learned entries are not configuration and must not appear.
  EXPECT_FALSE(running.contains("mac.static.de:ad:be:ef:00:01.port"));
}

// ── WP1.7 IGMP snooping ─────────────────────────────────────────

TEST(S5IgmpSnooping, TurnsSnoopingAndQuerierOn) {
  FakeBox box;
  box.AddConvergedFabric();
  box.SetBridgeAttr("multicast_snooping", "0");
  box.SetBridgeAttr("multicast_querier", "0");
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({
      {"igmp_snooping.enabled", "true"},
      {"igmp_snooping.querier", "true"},
  }));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(box.Ran("mcast_snooping 1"));
  EXPECT_TRUE(box.Ran("mcast_querier 1"));
}

TEST(S5IgmpSnooping, AlreadyCorrectStateIsNotRewritten) {
  FakeBox box;
  box.AddConvergedFabric();
  box.SetBridgeAttr("multicast_snooping", "1");
  box.SetBridgeAttr("multicast_querier", "0");
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({
      {"igmp_snooping.enabled", "true"},
      {"igmp_snooping.querier", "false"},
  }));
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(box.Ran("mcast_snooping"));
  EXPECT_FALSE(box.Ran("mcast_querier"));
}

TEST(S5IgmpSnooping, ReadRunningReportsSnoopingState) {
  FakeBox box;
  box.AddConvergedFabric();
  box.SetBridgeAttr("multicast_snooping", "1");
  box.SetBridgeAttr("multicast_querier", "1");
  S5Backend backend(MakeS5Schema());
  const auto running = backend.ReadRunning();
  EXPECT_EQ(running.at("igmp_snooping.enabled"), "true");
  EXPECT_EQ(running.at("igmp_snooping.querier"), "true");
}

// ── WP1.8 counters + clear ──────────────────────────────────────

/// Send one product wire command and return its rows.
auto Product(const std::string &command,
             std::vector<std::string> args = {})
    -> std::vector<std::string> {
  cli::protocol::Request req;
  req.command = command;
  req.args = std::move(args);
  req.user = "root";
  req.role = "admin";
  auto resp = service::HandleProduct(req);
  EXPECT_TRUE(resp.has_value());
  std::vector<std::string> lines;
  if (!resp) return lines;
  const std::string body(resp->data.begin(), resp->data.end());
  std::istringstream iss(body);
  std::string line;
  while (std::getline(iss, line)) lines.push_back(line);
  return lines;
}

TEST(S5Counters, ClearZeroesTheReadingWithoutTouchingTheKernel) {
  FakeBox box;
  box.AddConvergedFabric();
  box.SetCounter("lan1", "rx_bytes", 5000);
  box.SetCounter("lan1", "tx_bytes", 700);

  auto before = Product("show_counters", {"lan1"});
  ASSERT_FALSE(before.empty());
  EXPECT_NE(before[0].find("5000"), std::string::npos) << before[0];

  ASSERT_FALSE(Product("clear_counters", {"lan1"}).empty());
  auto after = Product("show_counters", {"lan1"});
  ASSERT_FALSE(after.empty());
  // The kernel cannot zero these, so "clear" is a baseline the reads
  // subtract from — the sysfs value itself is unchanged.
  EXPECT_NE(after[0].find("\t0\t0"), std::string::npos) << after[0];

  // Traffic after the clear counts from zero.
  box.SetCounter("lan1", "rx_bytes", 5250);
  auto later = Product("show_counters", {"lan1"});
  EXPECT_NE(later[0].find("250"), std::string::npos) << later[0];
}

TEST(S5Counters, ACounterThatWentBackwardsReadsZeroNotGarbage) {
  FakeBox box;
  box.AddConvergedFabric();
  box.SetCounter("lan1", "rx_bytes", 5000);
  ASSERT_FALSE(Product("clear_counters", {"lan1"}).empty());
  // Module reload, counter wrap: the raw value drops below the base.
  box.SetCounter("lan1", "rx_bytes", 10);
  auto rows = Product("show_counters", {"lan1"});
  ASSERT_FALSE(rows.empty());
  EXPECT_EQ(rows[0].find("18446744073709"), std::string::npos)
      << "underflowed: " << rows[0];
}

TEST(S5Counters, DetailShowsConfiguredAndNegotiatedSideBySide) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("ethtool lan1", "Auto-negotiation: on\n");
  auto rows = Product("show_interfaces_detail", {"lan1"});
  bool saw_speed = false;
  for (const auto &r : rows) {
    if (r.find("speed") != std::string::npos) {
      saw_speed = true;
      EXPECT_NE(r.find("negotiated"), std::string::npos) << r;
    }
  }
  EXPECT_TRUE(saw_speed);
}

TEST(S5MacTable, ClearFlushesLearnedEntriesOnly) {
  FakeBox box;
  box.AddConvergedFabric();
  box.ClearCommands();
  ASSERT_FALSE(Product("clear_mac_table", {"lan1"}).empty());
  // `dynamic` is the load-bearing word: a static entry is config, and
  // an operational verb must never delete configuration.
  EXPECT_TRUE(box.Ran("bridge fdb flush dev lan1 master dynamic"));
}

TEST(S5MacTable, ShowMarksStaticEntriesApartFromLearnedOnes) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("bridge fdb show",
         "aa:bb:cc:dd:ee:ff dev lan1 vlan 10 master br0 permanent\n"
         "de:ad:be:ef:00:01 dev lan1 vlan 1 master br0\n");
  auto rows = Product("show_mac_table");
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_NE(rows[0].find("static"), std::string::npos) << rows[0];
  EXPECT_NE(rows[1].find("dynamic"), std::string::npos) << rows[1];
}

// ── WP1.2 RSTP ──────────────────────────────────────────────────

/// mstpctl's `-f json showbridge` document, with the fields the module
/// reads. Values are all strings in mstpd's output; keeping that shape
/// here is the point of the fixture.
auto MstpBridgeJson(const std::string &bridge_id = "8.000.aa:bb:cc:00:00:01",
                    const std::string &root_id = "8.000.aa:bb:cc:00:00:01",
                    const std::string &root_port = "",
                    const std::string &vers = "rstp") -> std::string {
  return std::format(
      R"([{{"bridge":"br0","stp-enabled":"yes","enabled":"yes",)"
      R"("bridge-id":"{}","designated-root":"{}","regional-root":"{}",)"
      R"("root-port":"{}","max-age":"20","bridge-max-age":"20",)"
      R"("forward-delay":"15","bridge-forward-delay":"15",)"
      R"("hello-time":"2","ageing-time":"300",)"
      R"("force-protocol-version":"{}","time-since-topology-change":"42",)"
      R"("topology-change-count":"3","topology-change":"no"}}])",
      bridge_id, root_id, root_id, root_port, vers);
}

/// One port object of `-f json showportdetail`.
auto MstpPortJson(const std::string &port, const std::string &role,
                  const std::string &state,
                  const std::string &admin_cost = "0",
                  const std::string &port_id = "8.001",
                  bool admin_edge = false, bool guard = false,
                  bool guard_error = false) -> std::string {
  return std::format(
      R"({{"port":"{}","bridge":"br0","enabled":"yes","role":"{}",)"
      R"("port-id":"{}","state":"{}","external-port-cost":"2000000",)"
      R"("admin-external-cost":"{}","admin-edge-port":"{}",)"
      R"("oper-edge-port":"{}","bpdu-guard-port":"{}",)"
      R"("bpdu-guard-error":"{}","num-tx-bpdu":"120","num-rx-bpdu":"90",)"
      R"("num-tx-tcn":"2","num-rx-tcn":"4","num-transition-fwd":"1",)"
      R"("num-transition-blk":"1"}})",
      port, role, port_id, state, admin_cost, admin_edge ? "yes" : "no",
      admin_edge ? "yes" : "no", guard ? "yes" : "no",
      guard_error ? "yes" : "no");
}

/// A box with mstpd installed, the bridge in user-space STP mode, and
/// mstpd reporting the given ports.
auto GiveBoxMstpd(FakeBox &box, const std::string &ports_json,
                  const std::string &bridge_json = MstpBridgeJson())
    -> void {
  box.On("command -v mstpd", "/usr/sbin/mstpd\n");
  box.On("command -v mstpctl", "/usr/sbin/mstpctl\n");
  box.On("pidof mstpd", "4242\n");
  box.SetBridgeAttr("stp_state", "2");
  box.On("showbridge", bridge_json);
  box.On("showportdetail", "[" + ports_json + "]");
}

TEST(S5Stp, ModeChangePutsTheKernelBridgeIntoUserSpaceStp) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("command -v mstpd", "/usr/sbin/mstpd\n");
  box.On("command -v mstpctl", "/usr/sbin/mstpctl\n");
  // The box starts with spanning tree OFF: stp_state 0, no mstpd JSON.
  box.SetBridgeAttr("stp_state", "0");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({{"stp.mode", "rstp"}}));
  // The write itself fails because the fake kernel never flips
  // stp_state — what this asserts is the COMMAND SEQUENCE, which is
  // what has to be right for a real kernel to end up in user STP.
  EXPECT_FALSE(r.has_value());
  EXPECT_TRUE(box.Ran("ip link set br0 type bridge stp_state 0"));
  EXPECT_TRUE(box.Ran("ip link set br0 type bridge stp_state 1"));
  EXPECT_TRUE(box.Ran("mstpctl addbridge br0"));
}

TEST(S5Stp, ModeAndParametersProgramMstpd) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"stp.mode", "rstp"},
      {"stp.priority", "4096"},
      {"ports.lan1.stp.cost", "20000"},
      {"ports.lan1.stp.priority", "16"},
      {"ports.lan1.stp.edge", "true"},
      {"ports.lan1.stp.bpdu_guard", "true"},
  }));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  // Operator units on the wire, mstpd's field units in the command.
  EXPECT_TRUE(box.Ran("mstpctl settreeprio br0 0 1"));
  EXPECT_TRUE(box.Ran("mstpctl setportpathcost br0 lan1 20000"));
  EXPECT_TRUE(box.Ran("mstpctl settreeportprio br0 lan1 0 1"));
  EXPECT_TRUE(box.Ran("mstpctl setportadminedge br0 lan1 yes"));
  // An admin edge port must not be dragged back out of edge state by
  // the auto-edge machine.
  EXPECT_TRUE(box.Ran("mstpctl setportautoedge br0 lan1 no"));
  EXPECT_TRUE(box.Ran("mstpctl setbpduguard br0 lan1 yes"));
}

TEST(S5Stp, UnchangedSpanningTreeIsNotRewritten) {
  FakeBox box;
  box.AddConvergedFabric();
  // The box already holds exactly what the candidate asks for.
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding",
                                 "20000", "8.001"));
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({
      {"stp.mode", "rstp"},
      {"stp.priority", "32768"},
      {"stp.hello", "2"},
      {"stp.max_age", "20"},
      {"stp.forward_delay", "15"},
      {"ports.lan1.stp.cost", "20000"},
      {"ports.lan1.stp.priority", "128"},
  }));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  // Re-running setforcevers or bouncing stp_state would flap every
  // port's forwarding state on a live network, for a commit that
  // changed nothing about spanning tree.
  EXPECT_FALSE(box.Ran("stp_state"));
  EXPECT_FALSE(box.Ran("setforcevers"));
  EXPECT_FALSE(box.Ran("settreeprio"));
  EXPECT_FALSE(box.Ran("setportpathcost"));
  EXPECT_FALSE(box.Ran("settreeportprio"));
}

TEST(S5Stp, WithoutMstpdTheCommitFails) {
  FakeBox box;
  box.AddConvergedFabric();
  // No `command -v` rule: the binaries are not on this box.
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({{"stp.mode", "rstp"}}));
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("mstpd"), std::string::npos)
      << r.error().message;
}

TEST(S5Stp, TurningItOffNeedsNoDaemonAtAll) {
  FakeBox box;
  box.AddConvergedFabric();
  box.SetBridgeAttr("stp_state", "0");
  S5Backend backend(MakeS5Schema());
  // A box with no mstpd must still be able to hold `stp.mode off` —
  // otherwise it could never commit anything at all.
  auto r = backend.Apply(MakeCandidate({{"stp.mode", "off"}}));
  EXPECT_TRUE(r.has_value()) << r.error().message;
}

TEST(S5Stp, ImpossibleTimerCombinationIsRejectedBeforeAnyWrite) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  // 802.1D: 2 x (forward_delay - 1) >= max_age. mstpd refuses the
  // individual write, which would land mid-apply; catching it here
  // keeps the box untouched.
  auto r = backend.Apply(MakeCandidate({
      {"stp.mode", "rstp"},
      {"stp.max_age", "40"},
      {"stp.forward_delay", "15"},
  }));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  EXPECT_NE(r.error().message.find("forward_delay"), std::string::npos)
      << r.error().message;
  EXPECT_FALSE(box.Ran("mstpctl"));
}

TEST(S5Stp, TooFastHelloForTheMaxAgeIsRejected) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"stp.mode", "rstp"},
      {"stp.hello", "10"},
      {"stp.max_age", "6"},
      {"stp.forward_delay", "30"},
  }));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  EXPECT_NE(r.error().message.find("hello"), std::string::npos)
      << r.error().message;
}

TEST(S5Stp, WideButLegalTimersAreAccepted) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"stp.mode", "rstp"},
      {"stp.hello", "1"},
      {"stp.max_age", "40"},
      {"stp.forward_delay", "30"},
  }));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_TRUE(box.Ran("mstpctl setmaxage br0 40"));
  EXPECT_TRUE(box.Ran("mstpctl setfdelay br0 30"));
}

TEST(S5Stp, SpanningTreeOnTheRoutedUplinkIsRejected) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  // lan5 is deliberately out of the bridge; spanning tree there is a
  // configuration that can never take effect.
  auto r = backend.Apply(MakeCandidate({{"ports.lan5.stp.edge", "true"}}));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  EXPECT_NE(r.error().message.find("bridged"), std::string::npos)
      << r.error().message;
}

TEST(S5Stp, ReadRunningReportsConfiguredCostNotTheDerivedOne) {
  FakeBox box;
  box.AddConvergedFabric();
  // admin cost 0 ("auto"); mstpd's effective cost is 2000000.
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding", "0"));
  S5Backend backend(MakeS5Schema());
  const auto running = backend.ReadRunning();
  ASSERT_TRUE(running.contains("ports.lan1.stp.cost"));
  // Reading the derived cost back would silently turn the operator's
  // `auto` into a hard-coded 2000000 on the next commit.
  EXPECT_EQ(running.at("ports.lan1.stp.cost"), "0");
  EXPECT_EQ(running.at("stp.mode"), "rstp");
  EXPECT_EQ(running.at("stp.priority"), "32768");
  EXPECT_EQ(running.at("ports.lan1.stp.priority"), "128");
}

TEST(S5Stp, ForeignBridgeMembersAreNotReadBackAsConfiguration) {
  FakeBox box;
  box.AddConvergedFabric();
  // Something else joined the bridge — a veth, a tap, a test harness.
  // mstpd tracks it because it is a bridge port; the switch must not
  // treat it as configuration, because the very next commit would
  // then reject its own read-back as "not a bridged port" and the box
  // could not commit anything at all.
  GiveBoxMstpd(box,
               MstpPortJson("lan1", "Designated", "forwarding") + "," +
                   MstpPortJson("veth0", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  const auto running = backend.ReadRunning();
  EXPECT_TRUE(running.contains("ports.lan1.stp.edge"));
  EXPECT_FALSE(running.contains("ports.veth0.stp.edge"));
  // And the round trip has to close: whatever ReadRunning produced
  // must be committable.
  Candidate c;
  c.values = running;
  auto r = backend.Apply(c);
  EXPECT_TRUE(r.has_value())
      << (r ? std::string() : r.error().message);
}

TEST(S5Stp, ReadRunningReportsOffWhenTheBridgeIsNotInUserStp) {
  FakeBox box;
  box.AddConvergedFabric();
  box.SetBridgeAttr("stp_state", "0");
  S5Backend backend(MakeS5Schema());
  const auto running = backend.ReadRunning();
  EXPECT_EQ(running.at("stp.mode"), "off");
  EXPECT_FALSE(running.contains("stp.priority"));
}

TEST(S5Stp, ShowSpanningTreeNamesTheRootAndEachPortsRole) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(
      box,
      MstpPortJson("lan1", "Root", "forwarding", "0", "8.001") + "," +
          MstpPortJson("lan2", "Alternate", "discarding", "0", "8.002"),
      MstpBridgeJson("8.000.aa:bb:cc:00:00:09", "8.000.aa:bb:cc:00:00:01",
                     "lan1"));
  auto rows = Product("show_spanning_tree");
  std::string all;
  for (const auto &r : rows) all += r + "\n";
  EXPECT_NE(all.find("root port\tlan1"), std::string::npos) << all;
  EXPECT_NE(all.find("port\tlan2\tAlternate\tdiscarding"),
            std::string::npos)
      << all;
  // We are NOT the root here, so the "(this bridge)" note must not be
  // on the root row.
  EXPECT_EQ(all.find("(this bridge)"), std::string::npos) << all;
}

TEST(S5Stp, ShowSpanningTreeSaysSoWhenItIsOff) {
  FakeBox box;
  box.AddConvergedFabric();
  box.SetBridgeAttr("stp_state", "0");
  auto rows = Product("show_spanning_tree");
  std::string all;
  for (const auto &r : rows) all += r + "\n";
  EXPECT_NE(all.find("disabled"), std::string::npos) << all;
  // A box without mstpd must say that, rather than suggesting a
  // setting that cannot work.
  EXPECT_NE(all.find("not installed"), std::string::npos) << all;
}

TEST(S5Stp, BpduGuardViolationShowsAsASystemAlarm) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "discarding", "0",
                                 "8.001", false, true, true));
  auto rows = Product("show_system");
  std::string all;
  for (const auto &r : rows) all += r + "\n";
  EXPECT_NE(all.find("alarms\tbpdu-guard blocked lan1"), std::string::npos)
      << all;
}

TEST(S5Stp, NoViolationMeansNoAlarm) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  auto rows = Product("show_system");
  std::string all;
  for (const auto &r : rows) all += r + "\n";
  EXPECT_NE(all.find("alarms\tnone"), std::string::npos) << all;
}

TEST(S5Stp, ClearBpduGuardBouncesOnlyABlockedPort) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "discarding", "0",
                                 "8.001", false, true, true));
  box.ClearCommands();
  ASSERT_FALSE(Product("clear_bpdu_guard", {"lan1"}).empty());
  EXPECT_TRUE(box.Ran("ip link set lan1 down"));
  EXPECT_TRUE(box.Ran("ip link set lan1 up"));
}

TEST(S5Stp, ClearBpduGuardOnAHealthyPortLeavesItAlone) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  box.ClearCommands();
  auto rows = Product("clear_bpdu_guard", {"lan1"});
  ASSERT_FALSE(rows.empty());
  EXPECT_NE(rows[0].find("not blocked"), std::string::npos) << rows[0];
  // Bouncing a healthy port would be an operational verb causing an
  // outage it was not asked for.
  EXPECT_FALSE(box.Ran("ip link set lan1 down"));
}

TEST(S5Stp, StatisticsBaselineSubtractsWithoutTouchingMstpd) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  auto before = Product("show_spanning_tree_statistics", {"lan1"});
  ASSERT_FALSE(before.empty());
  EXPECT_NE(before[0].find("\t120\t90"), std::string::npos) << before[0];
  ASSERT_FALSE(
      Product("clear_spanning_tree_statistics", {"lan1"}).empty());
  auto after = Product("show_spanning_tree_statistics", {"lan1"});
  ASSERT_FALSE(after.empty());
  EXPECT_NE(after[0].find("lan1\t0\t0\t0\t0"), std::string::npos)
      << after[0];
}

// ── WP2.1 SVIs / WP2.2 routing ──────────────────────────────────

TEST(S5Svi, AnAddressOnAVlanCreatesTheInterfaceAndTheBridgeMembership) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"vlans.10.name", "office"},
      {"vlans.10.address", "10.10.0.1/24"},
  }));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_TRUE(box.Ran("ip link add link br0 name br0.10 type vlan id 10"));
  // The half everyone forgets: without `self` the bridge is not in the
  // VLAN, and the SVI answers nothing while looking perfectly healthy.
  EXPECT_TRUE(box.Ran("bridge vlan add dev br0 vid 10 self"));
  EXPECT_TRUE(box.Ran("ip addr add 10.10.0.1/24 dev br0.10"));
  EXPECT_TRUE(box.Ran("ip link set br0.10 up"));
}

TEST(S5Svi, ANameWithoutAnAddressCreatesNoInterface) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  // A VLAN can be named without the switch routing for it; growing an
  // interface anyway would put the switch in every VLAN it can name.
  auto r = backend.Apply(MakeCandidate({{"vlans.20.name", "guest"}}));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_FALSE(box.Ran("ip link add link br0 name br0.20"));
}

TEST(S5Svi, DeletingTheAddressRemovesTheInterface) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  // The box already holds an SVI the configuration no longer names.
  box.AddPort("br0.30");
  box.On("ip -o -4 addr show dev br0.30",
         "5: br0.30    inet 10.30.0.1/24 scope global br0.30\n");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({{"hostname", "sw"}}));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_TRUE(box.Ran("ip link del br0.30"));
  EXPECT_TRUE(box.Ran("bridge vlan del dev br0 vid 30 self"));
}

TEST(S5Svi, AnUnchangedSviIsNotTornDownAndRebuilt) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  box.AddPort("br0.10");
  box.On("ip -o -4 addr show dev br0.10",
         "5: br0.10    inet 10.10.0.1/24 scope global br0.10\n");
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(
      MakeCandidate({{"vlans.10.address", "10.10.0.1/24"}}));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  // Flushing and re-adding an address that is already right would drop
  // every session riding that SVI for no reason at all.
  EXPECT_FALSE(box.Ran("ip -4 addr flush dev br0.10"));
  EXPECT_FALSE(box.Ran("ip link del br0.10"));
}

TEST(S5Svi, ReadRunningReportsTheAddressBackAndTheNameStaysInConfig) {
  FakeBox box;
  box.AddConvergedFabric();
  box.AddPort("br0.10");
  box.On("ip -o -4 addr show dev br0.10",
         "5: br0.10    inet 10.10.0.1/24 scope global br0.10\n");
  S5Backend backend(MakeS5Schema());
  const auto running = backend.ReadRunning();
  EXPECT_EQ(running.at("vlans.10.address"), "10.10.0.1/24");
  // Names have no counterpart on the box; they survive by living in
  // the commit, not by being read back.
  EXPECT_FALSE(running.contains("vlans.10.name"));
}

TEST(S5Routing, ForwardingAndAStaticRouteAreProgrammed) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"routing.enabled", "true"},
      {"routing.static.office.prefix", "192.168.5.0/24"},
      {"routing.static.office.via", "10.0.0.254"},
  }));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_TRUE(box.Ran(
      "ip route replace 192.168.5.0/24 via 10.0.0.254 proto 201"));
  const auto running = backend.ReadRunning();
  EXPECT_EQ(running.at("routing.enabled"), "true");
}

TEST(S5Routing, TheConfigOwnsTheStaticSetAndNothingElse) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  box.On("ip -4 route show",
         "default via 10.0.0.1 dev lan5 proto dhcp\n"
         "10.0.0.0/24 dev lan5 scope link\n"
         "10.7.0.0/24 via 10.0.0.3 dev lan5 proto static\n"
         "192.168.5.0/24 via 10.0.0.254 dev lan5 proto 201\n"
         "172.16.0.0/16 via 10.0.0.9 dev lan5 proto 201\n");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"routing.static.keep.prefix", "192.168.5.0/24"},
      {"routing.static.keep.via", "10.0.0.254"},
  }));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  // The route WE installed and the config dropped goes.
  EXPECT_TRUE(box.Ran("ip route del 172.16.0.0/16 proto 201"));
  // The one it still names is already right and is not rewritten.
  EXPECT_FALSE(box.Ran("ip route replace 192.168.5.0/24"));
  // Nothing else is ours. `proto static` in particular means "an
  // administrator put this here" and is what ifupdown and
  // systemd-networkd use for the box's own uplink route — reconciling
  // against it cost a test VM its default route.
  EXPECT_FALSE(box.Ran("ip route del 10.7.0.0/24"));
  EXPECT_FALSE(box.Ran("ip route del default"));
  EXPECT_FALSE(box.Ran("ip route del 10.0.0.0/24"));
}

TEST(S5Routing, AnInterfaceGatewayBecomesTheDefaultRoute) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"interfaces.lan5.address", "10.0.0.2/24"},
      {"interfaces.lan5.gateway", "10.0.0.1"},
  }));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_TRUE(box.Ran("ip route replace default via 10.0.0.1 proto 201"));
}

TEST(S5Routing, TwoGatewaysAreRejectedBeforeAnyWrite) {
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"interfaces.lan5.gateway", "10.0.0.1"},
      {"interfaces.br0.gateway", "10.1.0.1"},
  }));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  EXPECT_NE(r.error().message.find("default route"), std::string::npos)
      << r.error().message;
}

TEST(S5Routing, AGatewayAndAStaticDefaultRouteCannotBothBeSet) {
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"interfaces.lan5.gateway", "10.0.0.1"},
      {"routing.static.d.prefix", "0.0.0.0/0"},
      {"routing.static.d.via", "10.0.0.9"},
  }));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  EXPECT_NE(r.error().message.find("pick one"), std::string::npos)
      << r.error().message;
}

TEST(S5Routing, HalfARouteIsRejected) {
  FakeBox box;
  box.AddConvergedFabric();
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(
      MakeCandidate({{"routing.static.half.prefix", "10.9.0.0/24"}}));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  EXPECT_NE(r.error().message.find("no via set"), std::string::npos)
      << r.error().message;
}

TEST(S5Vlans, ShowVlansJoinsNamesAddressesAndMembers) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("bridge vlan show",
         "port    vlan-id\n"
         "lan1    10 PVID Egress Untagged\n"
         "lan4    10\n"
         "        20\n");
  box.AddPort("br0.10");
  box.On("ip -o -4 addr show dev br0.10",
         "5: br0.10    inet 10.10.0.1/24 scope global br0.10\n");
  service::SetRunningConfigReader([] {
    cli::confd::Config c;
    c["vlans.10.name"] = "office";
    c["vlans.20.name"] = "guest";
    return c;
  });
  auto rows = Product("show_vlans");
  std::string all;
  for (const auto &r : rows) all += r + "\n";
  EXPECT_NE(all.find("10\toffice\t10.10.0.1/24\tlan1(u,pvid) lan4(t)"),
            std::string::npos)
      << all;
  // A VLAN that is only switched still appears, with a dash where the
  // address would be.
  EXPECT_NE(all.find("20\tguest\t-\tlan4(t)"), std::string::npos) << all;
}

TEST(S5Vlans, ShowRouteMarksWhereEachRouteCameFrom) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("ip -4 route show",
         "default via 10.0.0.1 dev lan5 proto dhcp\n"
         "192.168.5.0/24 via 10.0.0.254 dev lan5 proto 201\n"
         "10.0.0.0/24 dev lan5 scope link\n");
  auto rows = Product("show_route");
  std::string all;
  for (const auto &r : rows) all += r + "\n";
  EXPECT_NE(all.find("forwarding\tdisabled"), std::string::npos) << all;
  EXPECT_NE(all.find("192.168.5.0/24\t10.0.0.254\tlan5\tconfig"),
            std::string::npos)
      << all;
  EXPECT_NE(all.find("default\t10.0.0.1\tlan5\tdhcp"), std::string::npos)
      << all;
  // A route with no `proto` is the kernel's own; naming it beats a
  // blank column.
  EXPECT_NE(all.find("10.0.0.0/24\t-\tlan5\tkernel"), std::string::npos)
      << all;
}

// ── WP2.6 anti-lockout ──────────────────────────────────────────

/// Pretend the session arrived over ssh from `peer`.
class SshSession {
 public:
  explicit SshSession(const std::string &peer) {
    ::setenv("SSH_CONNECTION",
             std::format("{} 51000 10.10.0.1 22", peer).c_str(), 1);
  }
  ~SshSession() { ::unsetenv("SSH_CONNECTION"); }
};

TEST(S5AntiLockout, ChangingTheAddressTheSessionRidesOnWarns) {
  FakeBox box;
  box.AddConvergedFabric();
  box.AddPort("br0.10");
  box.On("ip -o -4 addr show dev br0.10",
         "5: br0.10    inet 10.10.0.1/24 scope global br0.10\n");
  box.On("ip route get 10.10.0.55",
         "10.10.0.55 dev br0.10 src 10.10.0.1 uid 0\n");
  SshSession session("10.10.0.55");
  S5Backend backend(MakeS5Schema());
  const auto warnings = backend.Warnings(
      MakeCandidate({{"vlans.10.address", "10.99.0.1/24"}}));
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("VLAN 10"), std::string::npos) << warnings[0];
  // Junos-style: suggest the tool that makes the change survivable
  // rather than refusing a legitimate change.
  EXPECT_NE(warnings[0].find("commit confirmed"), std::string::npos)
      << warnings[0];
}

TEST(S5AntiLockout, LeavingThatAddressAloneSaysNothing) {
  FakeBox box;
  box.AddConvergedFabric();
  box.AddPort("br0.10");
  box.On("ip -o -4 addr show dev br0.10",
         "5: br0.10    inet 10.10.0.1/24 scope global br0.10\n");
  box.On("ip route get 10.10.0.55",
         "10.10.0.55 dev br0.10 src 10.10.0.1 uid 0\n");
  SshSession session("10.10.0.55");
  S5Backend backend(MakeS5Schema());
  // Same address, different hostname: nothing about the path changes,
  // and a warning on every commit is a warning nobody reads.
  EXPECT_TRUE(backend
                  .Warnings(MakeCandidate({
                      {"vlans.10.address", "10.10.0.1/24"},
                      {"hostname", "sw-b"},
                  }))
                  .empty());
}

TEST(S5AntiLockout, AConsoleSessionIsNeverWarned) {
  FakeBox box;
  box.AddConvergedFabric();
  box.AddPort("br0.10");
  box.On("ip -o -4 addr show dev br0.10",
         "5: br0.10    inet 10.10.0.1/24 scope global br0.10\n");
  // No SSH_CONNECTION: there is no remote path to lose.
  ::unsetenv("SSH_CONNECTION");
  S5Backend backend(MakeS5Schema());
  EXPECT_TRUE(backend
                  .Warnings(MakeCandidate(
                      {{"vlans.10.address", "10.99.0.1/24"}}))
                  .empty());
}

TEST(S5AntiLockout, ReaddressingTheManagementInterfaceWarns) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("ip -o addr show",
         "3: lan5    inet 10.55.5.1/24 scope global lan5\n");
  box.On("ip route get 10.55.5.9",
         "10.55.5.9 dev lan5 src 10.55.5.1 uid 0\n");
  SshSession session("10.55.5.9");
  S5Backend backend(MakeS5Schema());
  const auto warnings = backend.Warnings(
      MakeCandidate({{"interfaces.lan5.address", "10.77.0.1/24"}}));
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("lan5"), std::string::npos) << warnings[0];
}

TEST(S5AntiLockout, HandingTheManagementInterfaceToDhcpWarns) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("ip -o addr show",
         "3: lan5    inet 10.55.5.1/24 scope global lan5\n");
  box.On("ip route get 10.55.5.9",
         "10.55.5.9 dev lan5 src 10.55.5.1 uid 0\n");
  SshSession session("10.55.5.9");
  S5Backend backend(MakeS5Schema());
  // The address is unchanged in the candidate, but DHCP will flush it
  // — the exact trap the factory-config note warns about.
  const auto warnings = backend.Warnings(MakeCandidate({
      {"interfaces.lan5.address", "10.55.5.1/24"},
      {"interfaces.lan5.dhcp", "true"},
  }));
  EXPECT_EQ(warnings.size(), 1u);
}

TEST(S5AntiLockout, ShuttingThePortTheSessionArrivedOnWarns) {
  FakeBox box;
  box.AddConvergedFabric();
  box.AddPort("br0.10");
  box.On("ip -o -4 addr show dev br0.10",
         "5: br0.10    inet 10.10.0.1/24 scope global br0.10\n");
  box.On("ip route get 10.10.0.55",
         "10.10.0.55 dev br0.10 src 10.10.0.1 uid 0\n");
  box.On("ip neigh show 10.10.0.55",
         "10.10.0.55 dev br0.10 lladdr de:ad:be:ef:00:07 REACHABLE\n");
  box.On("bridge fdb show",
         "de:ad:be:ef:00:07 dev lan2 vlan 10 master br0\n");
  SshSession session("10.10.0.55");
  S5Backend backend(MakeS5Schema());
  const auto warnings = backend.Warnings(MakeCandidate({
      {"vlans.10.address", "10.10.0.1/24"},
      {"ports.lan2.enabled", "false"},
  }));
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("arrived on lan2"), std::string::npos)
      << warnings[0];
}

TEST(S5AntiLockout, ChangingTheDefaultRouteWarnsARoutedSession) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("ip route get 192.0.2.9",
         "192.0.2.9 via 10.55.5.254 dev lan5 src 10.55.5.1 uid 0\n");
  box.On("ip -4 route show",
         "default via 10.55.5.254 dev lan5 proto 201\n");
  box.On("ip -o addr show",
         "3: lan5    inet 10.55.5.1/24 scope global lan5\n");
  SshSession session("192.0.2.9");
  S5Backend backend(MakeS5Schema());
  const auto warnings = backend.Warnings(MakeCandidate({
      {"interfaces.lan5.address", "10.55.5.1/24"},
      {"interfaces.lan5.gateway", "10.55.5.99"},
  }));
  ASSERT_FALSE(warnings.empty());
  bool saw_route = false;
  for (const auto &w : warnings) {
    if (w.find("default route") != std::string::npos) saw_route = true;
  }
  EXPECT_TRUE(saw_route);
}

// ── WP2.3 DHCP + DNS ────────────────────────────────────────────

/// A coherent two-VLAN configuration, as the backend would build it.
auto SampleDnsmasqConfig() -> dnsmasq::Config {
  dnsmasq::Config cfg;
  cfg.dns_enabled = true;
  cfg.local_domain = "office.lan";
  cfg.forwarders = {"9.9.9.9", "1.1.1.1"};
  dnsmasq::Pool office;
  office.vid = 10;
  office.interface = "br0.10";
  office.range_start = "10.10.0.100";
  office.range_end = "10.10.0.200";
  office.netmask = "255.255.255.0";
  office.lease_minutes = 720;
  office.gateway = "10.10.0.1";
  office.dns = "10.10.0.1";
  office.reservations = {{"aa:bb:cc:dd:ee:ff", "10.10.0.50"}};
  dnsmasq::Pool guest;
  guest.vid = 20;
  guest.interface = "br0.20";
  guest.range_start = "10.20.0.100";
  guest.range_end = "10.20.0.150";
  guest.netmask = "255.255.255.0";
  guest.lease_minutes = 60;
  cfg.pools = {office, guest};
  return cfg;
}

TEST(S5Dnsmasq, GoldenConfigForTwoVlans) {
  const auto out = dnsmasq::Render(SampleDnsmasqConfig());
  ASSERT_TRUE(out.has_value()) << out.error().message;
  EXPECT_EQ(*out,
            "# einheit s5 — GENERATED from the committed configuration on\n"
            "# every commit and every boot. Edits here are lost at the next\n"
            "# commit; configure DHCP and DNS through the CLI.\n"
            "bind-interfaces\n"
            "except-interface=lo\n"
            "dhcp-leasefile=/var/run/einheit/dnsmasq.leases\n"
            "no-resolv\n"
            "domain-needed\n"
            "bogus-priv\n"
            "server=9.9.9.9\n"
            "server=1.1.1.1\n"
            "domain=office.lan\n"
            "local=/office.lan/\n"
            "expand-hosts\n"
            "interface=br0.10\n"
            "dhcp-range=set:vlan10,10.10.0.100,10.10.0.200,"
            "255.255.255.0,720m\n"
            "dhcp-option=tag:vlan10,3,10.10.0.1\n"
            "dhcp-option=tag:vlan10,6,10.10.0.1\n"
            "dhcp-host=aa:bb:cc:dd:ee:ff,10.10.0.50\n"
            "interface=br0.20\n"
            "dhcp-range=set:vlan20,10.20.0.100,10.20.0.150,"
            "255.255.255.0,60m\n");
}

TEST(S5Dnsmasq, DhcpOnlyTurnsTheResolverOffRatherThanOmittingIt) {
  auto cfg = SampleDnsmasqConfig();
  cfg.dns_enabled = false;
  const auto out = dnsmasq::Render(cfg);
  ASSERT_TRUE(out.has_value()) << out.error().message;
  // port=0 is the load-bearing line: without it dnsmasq answers DNS
  // on every configured interface whether or not anyone asked it to.
  EXPECT_NE(out->find("port=0\n"), std::string::npos) << *out;
  EXPECT_EQ(out->find("server="), std::string::npos) << *out;
  EXPECT_NE(out->find("dhcp-range=set:vlan10"), std::string::npos) << *out;
}

/// Values that must never reach a line-oriented config file. dnsmasq
/// has no quoting whatsoever, so a newline in ANY value is a new
/// directive — there is no safe escaping of these, only refusal.
auto HostileValues() -> std::vector<std::pair<std::string, std::string>> {
  return {
      {"newline injection", "1.2.3.4\ndhcp-option=6,6.6.6.6"},
      {"carriage return", "1.2.3.4\rdhcp-authoritative"},
      {"comment escape", "1.2.3.4 # \ndhcp-range=0.0.0.0,0.0.0.0"},
      {"shell metacharacters", "1.2.3.4;rm -rf /"},
      {"command substitution", "$(reboot)"},
      {"backtick", "`reboot`"},
      {"config path traversal", "../../etc/passwd"},
      {"nul-ish control char", std::string("1.2.3.4\x01")},
      {"empty", ""},
      {"plain nonsense", "not-an-address"},
  };
}

TEST(S5Dnsmasq, EveryStringFieldRefusesHostileValues) {
  for (const auto &[label, hostile] : HostileValues()) {
    {
      auto cfg = SampleDnsmasqConfig();
      cfg.forwarders = {hostile};
      EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
          << "forwarder accepted " << label;
    }
    {
      auto cfg = SampleDnsmasqConfig();
      cfg.pools[0].range_start = hostile;
      EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
          << "range_start accepted " << label;
    }
    {
      auto cfg = SampleDnsmasqConfig();
      cfg.pools[0].range_end = hostile;
      EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
          << "range_end accepted " << label;
    }
    {
      auto cfg = SampleDnsmasqConfig();
      cfg.pools[0].netmask = hostile;
      EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
          << "netmask accepted " << label;
    }
    // "not-an-address" is a perfectly good interface name and a
    // perfectly good domain label, so it is only hostile to the fields
    // that are supposed to hold addresses.
    if (hostile != "not-an-address") {
      auto cfg = SampleDnsmasqConfig();
      cfg.pools[0].interface = hostile;
      EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
          << "interface accepted " << label;
    }
    {
      auto cfg = SampleDnsmasqConfig();
      cfg.pools[0].reservations[0].mac = hostile;
      EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
          << "reservation mac accepted " << label;
    }
    {
      auto cfg = SampleDnsmasqConfig();
      cfg.pools[0].reservations[0].ip = hostile;
      EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
          << "reservation ip accepted " << label;
    }
    // The gateway, dns and local domain are optional, so empty is a
    // legitimate "not set" for them rather than a hostile value.
    if (hostile.empty()) continue;
    {
      auto cfg = SampleDnsmasqConfig();
      cfg.pools[0].gateway = hostile;
      EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
          << "gateway accepted " << label;
    }
    {
      auto cfg = SampleDnsmasqConfig();
      cfg.pools[0].dns = hostile;
      EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
          << "dhcp dns accepted " << label;
    }
    if (hostile != "not-an-address") {
      auto cfg = SampleDnsmasqConfig();
      cfg.local_domain = hostile;
      EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
          << "local_domain accepted " << label;
    }
  }
}

TEST(S5Dnsmasq, AlmostValidAddressesAreStillRefused) {
  // The near misses a lenient parser waves through.
  for (const auto &bad : {"1.2.3", "1.2.3.4.5", "1.2.3.256", "1.2.3.-1",
                          "01.02.03.0004", " 1.2.3.4", "1.2.3.4 ",
                          "1.2.3.4/24"}) {
    auto cfg = SampleDnsmasqConfig();
    cfg.pools[0].range_start = bad;
    EXPECT_FALSE(dnsmasq::Render(cfg).has_value())
        << "accepted '" << bad << "'";
  }
}

TEST(S5Dnsmasq, AnInvertedRangeIsRefused) {
  auto cfg = SampleDnsmasqConfig();
  cfg.pools[0].range_start = "10.10.0.200";
  cfg.pools[0].range_end = "10.10.0.100";
  const auto out = dnsmasq::Render(cfg);
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().message.find("range_start is above"),
            std::string::npos)
      << out.error().message;
}

TEST(S5Dnsmasq, NetmaskAndSubnetMath) {
  EXPECT_EQ(dnsmasq::NetmaskOf("10.10.0.1/24"), "255.255.255.0");
  EXPECT_EQ(dnsmasq::NetmaskOf("10.10.0.1/16"), "255.255.0.0");
  EXPECT_EQ(dnsmasq::NetmaskOf("10.10.0.1/32"), "255.255.255.255");
  EXPECT_EQ(dnsmasq::NetmaskOf("10.10.0.1"), "");
  EXPECT_TRUE(dnsmasq::InSubnet("10.10.0.1/24", "10.10.0.200"));
  EXPECT_FALSE(dnsmasq::InSubnet("10.10.0.1/24", "10.10.1.200"));
  EXPECT_TRUE(dnsmasq::InSubnet("10.10.0.1/16", "10.10.1.200"));
  EXPECT_FALSE(dnsmasq::InSubnet("10.10.0.1", "10.10.0.2"));
}

TEST(S5Dhcp, CommitGeneratesTheServerAndStartsDnsmasq) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  box.On("command -v dnsmasq", "/usr/sbin/dnsmasq\n");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"vlans.10.address", "10.10.0.1/24"},
      {"vlans.10.dhcp.enabled", "true"},
      {"vlans.10.dhcp.range_start", "10.10.0.100"},
      {"vlans.10.dhcp.range_end", "10.10.0.200"},
      {"vlans.10.dhcp.static.AA:BB:CC:DD:EE:FF.ip", "10.10.0.50"},
      {"dns.serve", "true"},
      {"dns.local_domain", "office.lan"},
      {"dns.primary", "9.9.9.9"},
  }));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_TRUE(box.ServiceRunning("dnsmasq"));
  const auto conf = box.ReadRun("dnsmasq.conf");
  EXPECT_NE(conf.find("interface=br0.10"), std::string::npos) << conf;
  EXPECT_NE(conf.find(
                "dhcp-range=set:vlan10,10.10.0.100,10.10.0.200,"
                "255.255.255.0,720m"),
            std::string::npos)
      << conf;
  // A MAC typed in upper case is the same MAC.
  EXPECT_NE(conf.find("dhcp-host=aa:bb:cc:dd:ee:ff,10.10.0.50"),
            std::string::npos)
      << conf;
  EXPECT_NE(conf.find("domain=office.lan"), std::string::npos) << conf;
}

TEST(S5Dhcp, WithoutDnsmasqTheCommitFails) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"vlans.10.address", "10.10.0.1/24"},
      {"vlans.10.dhcp.enabled", "true"},
      {"vlans.10.dhcp.range_start", "10.10.0.100"},
      {"vlans.10.dhcp.range_end", "10.10.0.200"},
  }));
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("dnsmasq"), std::string::npos)
      << r.error().message;
}

TEST(S5Dhcp, APoolWithoutAnSviIsRejectedBeforeAnyWrite) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("command -v dnsmasq", "/usr/sbin/dnsmasq\n");
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({
      {"vlans.10.dhcp.enabled", "true"},
      {"vlans.10.dhcp.range_start", "10.10.0.100"},
      {"vlans.10.dhcp.range_end", "10.10.0.200"},
  }));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  EXPECT_NE(r.error().message.find("vlans.10.address"), std::string::npos)
      << r.error().message;
  EXPECT_FALSE(box.RunFileExists("dnsmasq.conf"));
}

TEST(S5Dhcp, ARangeOutsideTheVlanSubnetIsRejected) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("command -v dnsmasq", "/usr/sbin/dnsmasq\n");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({
      {"vlans.10.address", "10.10.0.1/24"},
      {"vlans.10.dhcp.enabled", "true"},
      {"vlans.10.dhcp.range_start", "10.10.0.100"},
      {"vlans.10.dhcp.range_end", "10.99.0.200"},
  }));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  EXPECT_NE(r.error().message.find("outside"), std::string::npos)
      << r.error().message;
}

TEST(S5Dhcp, AHostileValueFailsTheCommitAndWritesNothing) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("command -v dnsmasq", "/usr/sbin/dnsmasq\n");
  S5Backend backend(MakeS5Schema());
  box.ClearCommands();
  auto r = backend.Apply(MakeCandidate({
      {"vlans.10.address", "10.10.0.1/24"},
      {"vlans.10.dhcp.enabled", "true"},
      {"vlans.10.dhcp.range_start", "10.10.0.100"},
      {"vlans.10.dhcp.range_end", "10.10.0.200"},
      {"dns.local_domain", "evil.lan\ndhcp-option=6,6.6.6.6"},
  }));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ApplyError::ValidationFailed);
  // Fails CLOSED: nothing was generated, nothing was started.
  EXPECT_FALSE(box.RunFileExists("dnsmasq.conf"));
  EXPECT_FALSE(box.ServiceRunning("dnsmasq"));
}

TEST(S5Dhcp, TurningItAllOffStopsTheDaemon) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  box.On("command -v dnsmasq", "/usr/sbin/dnsmasq\n");
  S5Backend backend(MakeS5Schema());
  ASSERT_TRUE(backend.Apply(MakeCandidate({
      {"vlans.10.address", "10.10.0.1/24"},
      {"vlans.10.dhcp.enabled", "true"},
      {"vlans.10.dhcp.range_start", "10.10.0.100"},
      {"vlans.10.dhcp.range_end", "10.10.0.200"},
  })));
  ASSERT_TRUE(box.ServiceRunning("dnsmasq"));
  auto r = backend.Apply(MakeCandidate({{"hostname", "sw"}}));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_FALSE(box.ServiceRunning("dnsmasq"));
  EXPECT_FALSE(box.RunFileExists("dnsmasq.want"));
}

TEST(S5Dhcp, AnUnchangedServerIsNotRestarted) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  box.On("command -v dnsmasq", "/usr/sbin/dnsmasq\n");
  S5Backend backend(MakeS5Schema());
  const auto pool = MakeCandidate({
      {"vlans.10.address", "10.10.0.1/24"},
      {"vlans.10.dhcp.enabled", "true"},
      {"vlans.10.dhcp.range_start", "10.10.0.100"},
      {"vlans.10.dhcp.range_end", "10.10.0.200"},
  });
  ASSERT_TRUE(backend.Apply(pool));
  box.ClearCommands();
  // Restarting the DHCP server on a commit that changed nothing about
  // it would drop every client's lease renewal in flight.
  ASSERT_TRUE(backend.Apply(pool));
  EXPECT_FALSE(box.Ran("pkill -x dnsmasq"));
  EXPECT_FALSE(box.Ran("setsid dnsmasq"));
}

TEST(S5Dhcp, LeasesAreParsedAndAged) {
  FakeBox box;
  box.AddConvergedFabric();
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  box.WriteRun("dnsmasq.leases",
               std::format("{} aa:bb:cc:dd:ee:01 10.10.0.101 laptop *\n"
                           "{} aa:bb:cc:dd:ee:02 10.10.0.102 * *\n",
                           now + 3600, now - 60));
  auto rows = Product("show_dhcp_leases");
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_NE(rows[0].find("10.10.0.101\taa:bb:cc:dd:ee:01\tlaptop\t"),
            std::string::npos)
      << rows[0];
  EXPECT_TRUE(rows[0].ends_with("m")) << rows[0];
  // A lease past its expiry is shown as expired rather than as a
  // negative number of minutes.
  EXPECT_NE(rows[1].find("expired"), std::string::npos) << rows[1];
}

TEST(S5Dhcp, ClearLeaseReleasesTheAddressAndBouncesTheServer) {
  FakeBox box;
  box.AddConvergedFabric();
  box.StartService("dnsmasq");
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  box.WriteRun("dnsmasq.leases",
               std::format("{} aa:bb:cc:dd:ee:01 10.10.0.101 laptop *\n"
                           "{} aa:bb:cc:dd:ee:02 10.10.0.102 phone *\n",
                           now + 3600, now + 3600));
  auto rows = Product("clear_dhcp_lease", {"10.10.0.101"});
  ASSERT_FALSE(rows.empty());
  EXPECT_NE(rows[0].find("released"), std::string::npos) << rows[0];
  EXPECT_EQ(box.ReadRun("dnsmasq.leases").find("10.10.0.101"),
            std::string::npos);
  EXPECT_NE(box.ReadRun("dnsmasq.leases").find("10.10.0.102"),
            std::string::npos);
  // dnsmasq only reads its lease database at startup, so editing the
  // file without bouncing it would report success and change nothing.
  EXPECT_TRUE(box.Ran("pkill -x dnsmasq"));
  EXPECT_TRUE(box.ServiceRunning("dnsmasq"));
}

TEST(S5Dhcp, ClearingALeaseNobodyHoldsSaysSoAndBouncesNothing) {
  FakeBox box;
  box.AddConvergedFabric();
  box.StartService("dnsmasq");
  box.WriteRun("dnsmasq.leases", "");
  box.ClearCommands();
  auto rows = Product("clear_dhcp_lease", {"10.10.0.9"});
  ASSERT_FALSE(rows.empty());
  EXPECT_NE(rows[0].find("no lease"), std::string::npos) << rows[0];
  EXPECT_FALSE(box.Ran("pkill -x dnsmasq"));
}

TEST(S5Dhcp, ShowDhcpServerReportsPoolsAndUsage) {
  FakeBox box;
  box.AddConvergedFabric();
  box.StartService("dnsmasq");
  const auto rendered = dnsmasq::Render(SampleDnsmasqConfig());
  ASSERT_TRUE(rendered.has_value());
  box.WriteRun("dnsmasq.conf", *rendered);
  box.WriteRun("dnsmasq.leases",
               "99999999999 aa:bb:cc:dd:ee:01 10.10.0.101 laptop *\n");
  auto rows = Product("show_dhcp_server");
  std::string all;
  for (const auto &r : rows) all += r + "\n";
  EXPECT_NE(all.find("server\trunning"), std::string::npos) << all;
  EXPECT_NE(all.find("vlan10\t10.10.0.100 - 10.10.0.200\t720m\t1 lease(s)"),
            std::string::npos)
      << all;
  EXPECT_NE(all.find("vlan20\t10.20.0.100 - 10.20.0.150\t60m\t0 lease(s)"),
            std::string::npos)
      << all;
}

// ── WP1.3 LLDP ──────────────────────────────────────────────────

auto Hex(const std::vector<std::uint8_t> &bytes) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i > 0) out += ' ';
    out += std::format("{:02x}", bytes[i]);
  }
  return out;
}

auto Bytes(const std::string &hex) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  std::istringstream iss(hex);
  std::string tok;
  while (iss >> tok) {
    out.push_back(static_cast<std::uint8_t>(
        std::strtoul(tok.c_str(), nullptr, 16)));
  }
  return out;
}

TEST(S5Lldp, EncodesTheMandatoryTlvsByteForByte) {
  FakeBox box;
  lldp::Config cfg;
  cfg.system_name = "sw-a";
  cfg.system_description = "s5";
  const auto pdu = BuildLldpdu(cfg, "lan1", "aa:bb:cc:dd:ee:01", 120);
  // 802.1AB on the wire, one TLV per line:
  //   chassis id  (type 1, subtype 4 = MAC)
  //   port id     (type 2, subtype 5 = interface name)
  //   ttl         (type 3, 120 seconds)
  //   port desc   (type 4)
  //   system name (type 5)
  //   system desc (type 6)
  //   capabilities(type 7: bridge+router capable, bridge enabled)
  //   end         (type 0)
  EXPECT_EQ(Hex(pdu),
            "02 07 04 aa bb cc dd ee 01 "
            "04 05 05 6c 61 6e 31 "
            "06 02 00 78 "
            "08 04 6c 61 6e 31 "
            "0a 04 73 77 2d 61 "
            "0c 02 73 35 "
            "0e 04 00 14 00 04 "
            "00 00");
}

TEST(S5Lldp, ManagementAddressTlvIsAddedWhenThereIsOne) {
  FakeBox box;
  lldp::Config cfg;
  cfg.system_name = "sw-a";
  cfg.system_description = "s5";
  cfg.management_address = "10.0.0.5";
  const auto pdu = BuildLldpdu(cfg, "lan1", "aa:bb:cc:dd:ee:01", 120);
  // type 8, length 12: addr-string-len 5, IPv4 subtype 1, the address,
  // interface numbering subtype 2 (ifIndex), ifIndex 0, no OID.
  EXPECT_NE(Hex(pdu).find("10 0c 05 01 0a 00 00 05 02 00 00 00 00 00"),
            std::string::npos)
      << Hex(pdu);
}

TEST(S5Lldp, RoundTripsThroughTheParser) {
  FakeBox box;
  lldp::Config cfg;
  cfg.system_name = "core-sw";
  cfg.system_description = "einheit S5";
  cfg.management_address = "192.0.2.7";
  const auto pdu = BuildLldpdu(cfg, "lan3", "aa:bb:cc:00:00:09", 90);
  const auto got = lldp::ParseLldpdu(pdu.data(), pdu.size(), "lan1");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->local_port, "lan1");
  EXPECT_EQ(got->chassis_id, "aa:bb:cc:00:00:09");
  EXPECT_EQ(got->port_id, "lan3");
  EXPECT_EQ(got->system_name, "core-sw");
  EXPECT_EQ(got->system_description, "einheit S5");
  EXPECT_EQ(got->management_address, "192.0.2.7");
  EXPECT_EQ(got->ttl, 90);
  EXPECT_EQ(got->capabilities, "Bridge");
}

TEST(S5Lldp, ATlvLongerThanTheFrameDoesNotReadPastIt) {
  FakeBox box;
  // Chassis id TLV claiming 200 bytes in a 9-byte frame. Nothing here
  // is ours: the length is the sender's claim.
  auto pdu = Bytes("03 c8 04 aa bb cc dd ee 01");
  const auto got = lldp::ParseLldpdu(pdu.data(), pdu.size(), "lan1");
  // No usable identity survived, so this is not a neighbour — and,
  // crucially, we are still here to say so.
  EXPECT_FALSE(got.has_value());
}

TEST(S5Lldp, AHostileSystemNameCannotForgeATableRow) {
  FakeBox box;
  // A neighbour advertising "evil\tsw\nfake" would, unsanitised, add a
  // whole extra row to `show neighbors` — and an ESC could drive the
  // operator's terminal.
  auto pdu = Bytes("02 07 04 aa bb cc dd ee 01 "
                   "04 05 05 6c 61 6e 31 "
                   "06 02 00 78 "
                   "0a 0c 65 76 69 6c 09 73 77 0a 1b 5b 31 6d "
                   "00 00");
  const auto got = lldp::ParseLldpdu(pdu.data(), pdu.size(), "lan1");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->system_name, "evil.sw..[1m");
  // And the rendered row still has exactly the fields it should.
  const auto rendered = lldp::RenderNeighbors({*got});
  EXPECT_EQ(std::count(rendered.begin(), rendered.end(), '\n'), 1);
  EXPECT_EQ(std::count(rendered.begin(), rendered.end(), '\t'), 8);
}

TEST(S5Lldp, AFrameWithNoIdentityIsNotANeighbour) {
  FakeBox box;
  const auto empty = Bytes("00 00");
  EXPECT_FALSE(lldp::ParseLldpdu(empty.data(), empty.size(), "lan1"));
  // TTL only: nothing to name the far end with.
  const auto ttl_only = Bytes("06 02 00 78 00 00");
  EXPECT_FALSE(
      lldp::ParseLldpdu(ttl_only.data(), ttl_only.size(), "lan1"));
  EXPECT_FALSE(lldp::ParseLldpdu(nullptr, 0, "lan1"));
}

TEST(S5Lldp, ConfigFileRoundTrips) {
  FakeBox box;
  lldp::Config cfg;
  cfg.enabled = true;
  cfg.tx_interval = 45;
  cfg.system_name = "sw-a";
  cfg.system_description = "einheit S5";
  cfg.ports = {"lan1", "lan3"};
  const auto back = lldp::ParseConfig(lldp::RenderConfig(cfg));
  EXPECT_TRUE(back.enabled);
  EXPECT_EQ(back.tx_interval, 45);
  EXPECT_EQ(back.system_name, "sw-a");
  EXPECT_EQ(back.system_description, "einheit S5");
  EXPECT_EQ(back.ports, cfg.ports);
}

TEST(S5Lldp, ExpiredNeighboursAreDroppedOnRead) {
  FakeBox box;
  lldp::Neighbor fresh;
  fresh.local_port = "lan1";
  fresh.chassis_id = "aa:bb:cc:00:00:01";
  fresh.port_id = "eth0";
  fresh.ttl = 120;
  fresh.last_seen = 1000;
  auto stale = fresh;
  stale.local_port = "lan2";
  stale.last_seen = 100;
  box.WriteRun("lldp-neighbors", lldp::RenderNeighbors({fresh, stale}));
  // A neighbour that stopped advertising is gone; a table that still
  // lists it is a topology map that is quietly wrong.
  const auto got = lldp::ReadNeighbors(1050);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].local_port, "lan1");
}

TEST(S5Lldp, CommitGeneratesTheConfigAndStartsTheDaemon) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  ASSERT_FALSE(box.ServiceRunning("lldp"));
  auto r = backend.Apply(MakeCandidate({
      {"hostname", "sw-a"},
      {"lldp.enabled", "true"},
      {"lldp.tx_interval", "45"},
      {"lldp.port.lan3.enabled", "false"},
  }));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_TRUE(box.ServiceRunning("lldp"));
  const auto cfg = box.ReadRun("lldp.conf");
  EXPECT_NE(cfg.find("tx_interval 45"), std::string::npos) << cfg;
  EXPECT_NE(cfg.find("system_name sw-a"), std::string::npos) << cfg;
  EXPECT_NE(cfg.find("port lan1"), std::string::npos) << cfg;
  // The port the operator turned off must not be in the file at all.
  EXPECT_EQ(cfg.find("port lan3"), std::string::npos) << cfg;
  // The generated file is an apply artifact and says so, so nobody
  // edits it and wonders why the next commit reverts them.
  EXPECT_NE(cfg.find("GENERATED"), std::string::npos) << cfg;
}

TEST(S5Lldp, DisablingStopsTheDaemonAndClearsTheWantMarker) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  box.StartService("lldp");
  S5Backend backend(MakeS5Schema());
  auto r = backend.Apply(MakeCandidate({{"lldp.enabled", "false"}}));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_FALSE(box.ServiceRunning("lldp"));
  EXPECT_FALSE(box.RunFileExists("lldp.want"));
}

TEST(S5Lldp, AnUnrelatedCommitDoesNotBounceTheDaemon) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  ASSERT_TRUE(backend.Apply(MakeCandidate({{"hostname", "sw-a"}})));
  box.ClearCommands();
  // Same configuration again: the generated file is identical, so
  // there is nothing to tell the daemon and certainly nothing to
  // restart — a bounce would drop every learned neighbour.
  ASSERT_TRUE(backend.Apply(MakeCandidate({{"hostname", "sw-a"}})));
  EXPECT_FALSE(box.Ran("pkill"));
  // `setsid` is the start, not the probe — `pgrep -f` mentions
  // --lldp-daemon too, so matching on that would pass either way.
  EXPECT_FALSE(box.Ran("setsid"));
}

TEST(S5Lldp, ChangingTheAdvertisedNameSignalsRatherThanRestarts) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  ASSERT_TRUE(backend.Apply(MakeCandidate({{"hostname", "sw-a"}})));
  box.ClearCommands();
  ASSERT_TRUE(backend.Apply(MakeCandidate({{"hostname", "sw-b"}})));
  EXPECT_TRUE(box.Ran("pkill -HUP -f '[e]inheit_s5 --lldp-daemon'"));
  EXPECT_TRUE(box.ServiceRunning("lldp"));
}

TEST(S5Lldp, ShowNeighborsRendersWhatTheDaemonHeard) {
  FakeBox box;
  box.AddConvergedFabric();
  lldp::Neighbor n;
  n.local_port = "lan2";
  n.chassis_id = "aa:bb:cc:00:00:07";
  n.port_id = "ge-0/0/1";
  n.system_name = "core-sw";
  n.management_address = "10.0.0.1";
  n.capabilities = "Bridge Router";
  n.ttl = 3600;
  n.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  box.WriteRun("lldp-neighbors", lldp::RenderNeighbors({n}));
  auto rows = Product("show_neighbors");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_NE(rows[0].find("lan2\taa:bb:cc:00:00:07\tcore-sw\tge-0/0/1"),
            std::string::npos)
      << rows[0];
}

TEST(S5Services, AWantedButDeadServiceReadsAsDown) {
  FakeBox box;
  box.AddConvergedFabric();
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  ASSERT_TRUE(backend.Apply(MakeCandidate({{"lldp.enabled", "true"}})));
  ASSERT_TRUE(box.ServiceRunning("lldp"));
  // The daemon dies behind confd's back, the way a real one does.
  box.On("pgrep -f '[e]inheit_s5 --lldp-daemon'", "");
  auto rows = Product("show_system_services");
  bool saw = false;
  for (const auto &r : rows) {
    if (r.starts_with("lldp\t")) {
      saw = true;
      EXPECT_NE(r.find("DOWN"), std::string::npos) << r;
    }
  }
  EXPECT_TRUE(saw);
}

TEST(S5Backend, SchemaExerciserFullSurface) {
  FakeBox box;
  box.AddConvergedFabric();
  box.On("hostname ", "");
  box.On("pidof ntpd", "1234\n");
  box.On("bridge vlan show", "port    vlan-id\n");
  box.On("command -v dnsmasq", "/usr/sbin/dnsmasq\n");
  box.On("command -v mdns-repeater", "/usr/local/sbin/mdns-repeater\n");
  GiveBoxMstpd(box, MstpPortJson("lan1", "Designated", "forwarding"));
  S5Backend backend(MakeS5Schema());
  cli::confd::Runtime rt(backend);

  // A static MAC entry needs BOTH a port and a vlan to be programmable,
  // and the exerciser sets exactly one path per case. Commit the port
  // first so the `.vlan` cases land on a complete entry — the exerciser
  // works on top of running config, so this gives full coverage of the
  // subtree instead of skipping it.
  {
    cli::protocol::Request req;
    req.user = "root";
    req.role = "admin";
    req.command = "configure";
    auto opened = rt.HandleRequest(req);
    ASSERT_EQ(opened.status, cli::protocol::ResponseStatus::Ok);
    const std::string session(opened.data.begin(), opened.data.end());
    cli::protocol::Request set = req;
    set.command = "set";
    set.args = {"mac.static.aa:bb:cc:dd:ee:ff.port", "lan1"};
    set.session_id = session;
    ASSERT_EQ(rt.HandleRequest(set).status,
              cli::protocol::ResponseStatus::Ok);
    // Same reason for a static route, but both halves this time: a
    // route has no defaultable field, so a prefix without a next hop
    // is not programmable in either direction.
    set.args = {"routing.static.k1.prefix", "192.0.2.0/24"};
    ASSERT_EQ(rt.HandleRequest(set).status,
              cli::protocol::ResponseStatus::Ok);
    set.args = {"routing.static.k1.via", "10.0.0.254"};
    ASSERT_EQ(rt.HandleRequest(set).status,
              cli::protocol::ResponseStatus::Ok);
    // A DHCP pool is coherent or it is nothing: it needs the VLAN to
    // have an address, and a range inside it. Seeded in the
    // exerciser's own value space (192.0.2.0/24, 192.0.2.7) so that
    // whichever field a case overwrites, the result is still a pool
    // that could really be served — the object's fields are visited
    // in unspecified order, so start and end are seeded EQUAL and
    // setting either to the generated address keeps start <= end.
    //
    // mDNS and NTP serving are the same story: reflection between one
    // VLAN is nothing, and serving time you never synchronised is
    // worse than not serving it, so both refuse an incoherent
    // candidate. THREE VLANs are reflected so that the generated
    // `mdns.reflect.10 = false` case still leaves a working pair —
    // otherwise the exerciser would be asking the backend to accept a
    // configuration that cannot work.
    for (const auto &[path, value] :
         std::vector<std::pair<std::string, std::string>>{
             {"vlans.10.address", "192.0.2.1/24"},
             {"vlans.10.dhcp.range_start", "192.0.2.7"},
             {"vlans.10.dhcp.range_end", "192.0.2.7"},
             {"vlans.20.address", "198.51.100.1/24"},
             {"vlans.30.address", "203.0.113.1/24"},
             {"mdns.reflect.10", "true"},
             {"mdns.reflect.20", "true"},
             {"mdns.reflect.30", "true"},
             {"ntp.server", "192.0.2.123"}}) {
      set.args = {path, value};
      ASSERT_EQ(rt.HandleRequest(set).status,
                cli::protocol::ResponseStatus::Ok)
          << path;
    }
    cli::protocol::Request commit = req;
    commit.command = "commit";
    commit.session_id = session;
    ASSERT_EQ(rt.HandleRequest(commit).status,
              cli::protocol::ResponseStatus::Ok);
  }

  cli::confd::ExerciseOptions opts;
  opts.map_keys["interfaces"] = "lan5";
  opts.map_keys["ports"] = "lan1";
  opts.map_keys["ports.lan1.vlan"] = "100";
  // The generic "k1" key the exerciser invents is not a MAC address,
  // and the backend is right to reject it — this map is keyed by a
  // domain type the schema cannot express.
  opts.map_keys["mac.static"] = "aa:bb:cc:dd:ee:ff";
  opts.map_keys["vlans"] = "10";
  opts.map_keys["vlans.10.dhcp.static"] = "aa:bb:cc:dd:ee:fe";
  opts.map_keys["lldp.port"] = "lan1";
  opts.map_keys["mdns.reflect"] = "10";
  // No I2C bus on the fake box — the PoE subtree is exercised by
  // PoeWithoutBusRejectsAtValidation instead.
  opts.skip_prefixes.push_back("poe");
  // The three spanning-tree timers are the one place in the schema
  // where a value's validity depends on its SIBLINGS: 802.1D ties them
  // together (2 x (forward_delay - 1) >= max_age >= 2 x (hello + 1)),
  // and mstpd rejects any triple that breaks it. The exerciser's model
  // is one path at a time, so it would generate each field's range
  // maximum against the others' defaults and call the (correct)
  // rejection a bug. Covered instead by S5Stp.{Impossible,TooFast,
  // WideButLegal}Timer* — both sides of the constraint, explicitly.
  opts.skip_prefixes.push_back("stp.hello");
  opts.skip_prefixes.push_back("stp.max_age");
  opts.skip_prefixes.push_back("stp.forward_delay");

  const auto cases =
      cli::confd::GenerateCases(backend.Schema(), opts);
  ASSERT_GT(cases.size(), 20u);
  const auto failures = cli::confd::ExerciseRuntime(rt, cases);
  for (const auto &f : failures) {
    ADD_FAILURE() << std::format("{} = '{}' ({}): {}", f.c.path,
                                 f.c.value, f.c.note, f.detail);
  }
  EXPECT_TRUE(failures.empty());
}

}  // namespace
}  // namespace einheit::s5
