/// @file test_backend.cc
/// @brief S5Backend unit tests against a fake box, plus the
/// schema-driven exerciser over the real backend.
// Copyright (c) 2026 Einheit Networks

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
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
#include "einheit/s5/dsa.h"
#include "einheit/s5/fabric.h"
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
    util::SetFsRoot(root_.string());
    util::SetCmdRunner([this](const std::string &cmd) {
      commands_.push_back(cmd);
      for (const auto &[needle, out] : rules_) {
        if (cmd.find(needle) != std::string::npos) return out;
      }
      return std::string();
    });
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
  fs::path root_;
  std::vector<std::pair<std::string, std::string>> rules_;
  std::vector<std::string> commands_;
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

TEST(S5Backend, SchemaExerciserFullSurface) {
  FakeBox box;
  box.AddPort("lan1");
  box.On("hostname ", "");
  box.On("pidof ntpd", "1234\n");
  box.On("bridge vlan show", "port    vlan-id\n");
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
  // No I2C bus on the fake box — the PoE subtree is exercised by
  // PoeWithoutBusRejectsAtValidation instead.
  opts.skip_prefixes.push_back("poe");

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
