/// @file test_backend.cc
/// @brief S5Backend unit tests against a fake box, plus the
/// schema-driven exerciser over the real backend.
// Copyright (c) 2026 Einheit Networks

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/confd/exerciser.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/s5/backend.h"
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
    util::SetCmdRunner({});
    util::SetFsRoot("");
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  /// Commands containing `needle` return `output`.
  auto On(std::string needle, std::string output) -> void {
    rules_.emplace_back(std::move(needle), std::move(output));
  }

  auto AddPort(const std::string &name, bool up = true) -> void {
    const auto dir = root_ / "sys/class/net" / name;
    fs::create_directories(dir);
    std::ofstream(dir / "operstate") << (up ? "up" : "down");
    std::ofstream(dir / "flags") << (up ? "0x1003" : "0x1002");
    std::ofstream(dir / "speed") << "1000";
    std::ofstream(dir / "duplex") << "full";
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

TEST(S5Backend, SchemaExerciserFullSurface) {
  FakeBox box;
  box.AddPort("lan1");
  box.On("hostname ", "");
  box.On("pidof ntpd", "1234\n");
  box.On("bridge vlan show", "port    vlan-id\n");
  S5Backend backend(MakeS5Schema());
  cli::confd::Runtime rt(backend);

  cli::confd::ExerciseOptions opts;
  opts.map_keys["interfaces"] = "lan5";
  opts.map_keys["ports"] = "lan1";
  opts.map_keys["ports.lan1.vlan"] = "100";
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
