/// @file main.cc
/// @brief einheit s5 entry point — single-binary, direct-to-hardware.
///
/// The embedded management-plane wiring: an in-process confd
/// Runtime over S5Backend serves the config lifecycle (configure /
/// set / commit / commit confirmed / rollback), service::
/// HandleProduct serves product reads and operational actions, and
/// the shell reaches both through one InProcTransport — the same
/// Runtime::HandleRequest a standalone daemon would serve. Crash
/// containment mirrors the framework appliance binary: SIGPIPE
/// ignored, fault handlers logging signal + last command +
/// backtrace, and a fork supervisor so a crash never leaves a dead
/// SSH prompt.
///
/// Two modes:
///   (no arguments)  interactive CLI, as above.
///   --apply-boot    oneshot run from init BEFORE login is possible:
///                   build the switch fabric, re-apply the committed
///                   configuration, exit. This is what makes the box
///                   come back as itself after a power cut instead of
///                   as a factory-default switch with its operator's
///                   intent stranded in the commit history.
// Copyright (c) 2026 Einheit Networks

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

#include "einheit/s5/backend.h"
#include "einheit/s5/fabric.h"
#include "einheit/s5/poe.h"
#include "einheit/s5/service.h"
#include "einheit/s5/switch_adapter.h"
#include "einheit/cli/audit.h"
#include "einheit/cli/auth.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/docs.h"
#include "einheit/cli/globals.h"
#include "einheit/cli/render/terminal_caps.h"
#include "einheit/cli/shell.h"
#include "einheit/cli/signals.h"
#include "einheit/cli/supervisor.h"
#include "einheit/cli/transport/inproc.h"

namespace {

using namespace einheit::cli;

/// Durable state root: /var/lib/einheit/s5 when writable (the
/// setuid launcher runs the CLI as root), else per-user fallback.
/// The confd store, audit log, and crash log all live here so a
/// commit-confirmed window survives a reboot.
auto StateDir() -> std::string {
  const std::string system_dir = "/var/lib/einheit/s5";
  std::error_code ec;
  std::filesystem::create_directories(system_dir, ec);
  if (!ec && ::access(system_dir.c_str(), W_OK) == 0) {
    return system_dir;
  }
  if (const char *home = std::getenv("HOME"); home != nullptr) {
    const auto dir = std::format("{}/.einheit/s5", home);
    std::filesystem::create_directories(dir, ec);
    return dir;
  }
  return std::format("/tmp/einheit-s5-{}",
                     static_cast<long>(::getuid()));
}

/// Shipped factory defaults, read by `load factory`. Part of the
/// image, not of the writable state — a factory reset must not be
/// able to consume a file the operator could have edited into the
/// state directory. Absent from the image means "factory is the empty
/// configuration", which resets the box to its own defaults.
constexpr const char *kFactoryConfigPath =
    "/etc/einheit/s5/factory.conf";

/// Append one audit record to <state_dir>/audit.log. The runtime
/// calls this for every mutating request; it must never throw.
auto MakeAuditSink(const std::string &state_dir) -> audit::Sink {
  const auto path = std::format("{}/audit.log", state_dir);
  return [path](const audit::Record &rec) {
    try {
      std::ofstream f(path, std::ios::app);
      if (!f) return;
      std::string args;
      for (const auto &a : rec.args) {
        args += ' ';
        args += a;
      }
      f << std::format("{} user={} role={} cmd={}{} ok={} {}\n",
                       rec.timestamp, rec.user, rec.role,
                       rec.wire_command.empty() ? rec.command
                                                : rec.wire_command,
                       args, rec.ok ? "yes" : "no", rec.outcome);
    } catch (...) {
      // Auditing must never take the control plane down.
    }
  };
}

/// Runtime options every mode shares, so the interactive CLI and the
/// boot oneshot cannot disagree about where state lives.
auto MakeRuntimeOptions(const std::string &state_dir)
    -> confd::RuntimeOptions {
  confd::RuntimeOptions ropts;
  ropts.state_dir = state_dir;
  ropts.audit = MakeAuditSink(state_dir);
  ropts.factory_config = kFactoryConfigPath;
  return ropts;
}

/// Print the generated operator reference (markdown) and exit.
auto RunDumpDocs() -> int {
  auto schema = einheit::s5::MakeS5Schema();
  auto adapter = einheit::s5::MakeSwitchAdapter(schema);
  CommandTree tree;
  if (auto r = RegisterGlobals(tree, GlobalsOptions{}); !r) {
    std::cerr << std::format("register globals: {}\n",
                             r.error().message);
    return 1;
  }
  for (auto &spec : adapter->Commands()) {
    (void)Register(tree, std::move(spec));
  }
  std::cout << docs::GenerateReference(adapter->Metadata(), tree,
                                       schema.Get());
  return 0;
}

/// Boot-restore oneshot: fabric, then the committed configuration.
/// Ordering is the point — a port or VLAN apply against a box with no
/// bridge either errors out or, worse, succeeds and does nothing.
auto RunApplyBoot() -> int {
  if (!einheit::s5::poe::Init("/dev/i2c-0")) {
    std::cerr << "einheit-s5: warning: PoE I2C bus not available\n";
  }
  auto schema = einheit::s5::MakeS5Schema();
  einheit::s5::S5Backend backend(schema);

  // The fabric is not optional here the way it is in an interactive
  // session: this is the process whose whole job is building it, so a
  // failure has to be loud and non-zero for init to report. Timed and
  // recorded as a boot step either way, so `show system boot` can say
  // which half of the boot went wrong.
  const auto fabric_t0 = std::chrono::steady_clock::now();
  auto fabric_result = backend.EnsureFabric();
  const auto fabric_status =
      einheit::s5::fabric::GetStatus(einheit::s5::fabric::S5Topology());
  confd::BootStep fabric_step;
  fabric_step.name = "fabric";
  fabric_step.ok = fabric_result.has_value();
  fabric_step.duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - fabric_t0)
          .count();
  fabric_step.detail =
      fabric_result
          ? std::format("{} up, {} member(s) enslaved",
                        fabric_status.bridge, fabric_status.enslaved.size())
          : fabric_result.error().message;

  const auto state_dir = StateDir();
  einheit::s5::service::SetStateDir(state_dir);
  if (!fabric_result) {
    // Record the failure before giving up, or the one boot an operator
    // most needs explained is the one with no report.
    confd::Runtime runtime(backend, MakeRuntimeOptions(state_dir));
    (void)runtime.ApplyRunningAtBoot({fabric_step});
    std::cerr << std::format("einheit-s5: fabric bootstrap failed: {}\n",
                             fabric_result.error().message);
    return 1;
  }
  std::cout << std::format("einheit-s5: fabric {}\n", fabric_step.detail);
  if (!fabric_status.detached.empty() || !fabric_status.absent.empty()) {
    std::cerr << std::format(
        "einheit-s5: warning: {} detached, {} absent — see "
        "`show fabric`\n",
        fabric_status.detached.size(), fabric_status.absent.size());
  }

  confd::Runtime runtime(backend, MakeRuntimeOptions(state_dir));
  auto restored = runtime.ApplyRunningAtBoot({fabric_step});
  if (!restored) {
    std::cerr << std::format("einheit-s5: boot apply failed: {}\n",
                             restored.error().message);
    return 1;
  }
  if (restored->reverted_pending) {
    std::cout << "einheit-s5: unconfirmed commit-confirmed window "
                 "reverted (boot is the deadline)\n";
  }
  if (restored->seeded_factory) {
    // First boot of a factory-fresh box: the shipped defaults become
    // commit 1. Worth saying out loud — it is the one boot that
    // changes the configuration rather than restoring it.
    std::cout << std::format(
        "einheit-s5: seeded factory defaults as commit {} ({} paths)\n",
        restored->commit, restored->paths);
  } else if (restored->applied) {
    std::cout << std::format(
        "einheit-s5: restored commit {} ({} paths)\n", restored->commit,
        restored->paths);
  } else {
    std::cout << "einheit-s5: no committed configuration to restore\n";
  }
  return 0;
}

auto RunCli() -> int {
  // PoE is optional at runtime (the switch still forwards without
  // it); warn instead of refusing to start.
  if (!einheit::s5::poe::Init("/dev/i2c-0")) {
    std::cerr << "warning: PoE I2C bus not available\n";
  }

  // One schema instance backs both sides: the adapter (completion,
  // client-side validation) and the backend (runtime validation).
  auto schema = einheit::s5::MakeS5Schema();
  auto adapter = einheit::s5::MakeSwitchAdapter(schema);

  const auto state_dir = StateDir();
  // `show system` reads the boot report from here for its
  // config-divergence row.
  einheit::s5::service::SetStateDir(state_dir);
  einheit::s5::S5Backend backend(schema);
  // Best-effort here, unlike --apply-boot: an operator running the CLI
  // unprivileged, or on a dev box with no switch ports, should still
  // get a shell. It matters that this runs before the Runtime is
  // built, because ReadRunning reads VLANs off the bridge.
  if (auto f = backend.EnsureFabric(); !f) {
    std::cerr << std::format("warning: fabric bootstrap: {}\n",
                             f.error().message);
  }
  confd::Runtime runtime(backend, MakeRuntimeOptions(state_dir));

  CommandTree tree;
  // Core verbs are non-optional; the config verbs are justified
  // here because a real backend holds and applies the candidate.
  if (auto r = RegisterGlobals(tree, GlobalsOptions{}); !r) {
    std::cerr << std::format("register globals: {}\n",
                             r.error().message);
    return 1;
  }
  for (auto &spec : adapter->Commands()) {
    (void)Register(tree, std::move(spec));
  }

  auto caller_result = auth::ResolveLocal();
  auth::CallerIdentity caller;
  if (caller_result) {
    caller = *caller_result;
  } else {
    caller.user = "admin";
    caller.transport = "console";
  }
  // Root gets admin. Everyone else keeps their resolved role.
  if (caller.user == "root") {
    caller.role = RoleGate::AdminOnly;
  }

  // Product commands first, lifecycle commands to the runtime —
  // both behind the one Transport interface the shell knows.
  auto tx = transport::NewInProcTransport(
      [&runtime](const protocol::Request &req) {
        if (auto r = einheit::s5::service::HandleProduct(req)) {
          return *r;
        }
        return runtime.HandleRequest(req);
      });
  if (auto r = tx->Connect(); !r) {
    std::cerr << std::format("transport: {}\n", r.error().message);
    return 1;
  }

  shell::Shell sh;
  sh.tx = std::move(tx);
  sh.adapter = std::move(adapter);
  sh.tree = std::move(tree);
  sh.caps = render::DetectTerminal();
  sh.caller = caller;
  sh.target_name = "local";

  auto result = shell::RunShell(sh);
  // Drop the transport before the runtime goes out of scope: its
  // handler captured the runtime by reference, and the runtime's
  // commit-confirmed timer thread must join while the backend and
  // schema are still alive.
  sh.tx.reset();
  if (!result) {
    std::cerr << std::format("shell error: {}\n",
                             result.error().message);
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
  // Contain half of crash safety, installed before anything else
  // can fault: no SIGPIPE kill on peer disconnect, and every
  // SEGV/ABRT/BUS/ILL/FPE is diagnosed (signal + last command +
  // backtrace) then re-raised for a core dump.
  signals::IgnoreSigpipe();
  const auto crash_log = std::format("{}/crash.log", StateDir());
  signals::InstallFaultHandlers(crash_log);

  bool apply_boot = false;
  bool dump_docs = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--apply-boot") {
      apply_boot = true;
    } else if (arg == "--dump-docs") {
      dump_docs = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: einheit_s5 [--apply-boot|--dump-docs]\n"
                   "  (no arguments)  interactive CLI\n"
                   "  --apply-boot    build the fabric and re-apply the "
                   "committed\n"
                   "                  configuration, then exit (run "
                   "from init)\n"
                   "  --dump-docs     print the generated operator "
                   "reference\n"
                   "                  (markdown) and exit\n";
      return 0;
    } else {
      std::cerr << std::format(
          "einheit-s5: unknown argument '{}' (try --help)\n", arg);
      return 2;
    }
  }

  if (dump_docs) {
    // Reference docs from the same structures the binary runs on —
    // the repo checks the output in and a test diffs the two, so
    // docs/reference.md cannot go stale (see docs/README.md).
    try {
      return RunDumpDocs();
    } catch (const std::exception &e) {
      std::cerr << std::format("einheit-s5: fatal: {}\n", e.what());
      return 1;
    }
  }

  if (apply_boot) {
    // No supervisor and no shell: init wants a process that does one
    // thing and reports an exit code.
    try {
      return RunApplyBoot();
    } catch (const std::exception &e) {
      std::cerr << std::format("einheit-s5: fatal: {}\n", e.what());
      return 1;
    } catch (...) {
      std::cerr << "einheit-s5: fatal: unknown error\n";
      return 1;
    }
  }

  // Interactive crash supervision: fork a thin supervisor that
  // re-execs this binary; if the shell dies from a fault signal
  // the parent prints a clear notice naming the last command and
  // the crash log instead of leaving a dead SSH prompt.
  if (std::getenv("EINHEIT_SUPERVISED") == nullptr &&
      ::isatty(STDIN_FILENO) == 1) {
    ::setenv("EINHEIT_SUPERVISED", "1", 1);
    SupervisorOptions sopts;
    sopts.crash_log_path = crash_log;
    return RunSupervised(
        [argv]() -> int {
          ::execv("/proc/self/exe", argv);
          std::perror("einheit-s5: exec (supervisor)");
          return 127;
        },
        sopts);
  }

  // Last net: nothing that escapes the shell may reach
  // std::terminate. Turn it into a clean message + non-zero exit.
  try {
    return RunCli();
  } catch (const std::exception &e) {
    std::cerr << std::format("fatal: {}\n", e.what());
    return 1;
  } catch (...) {
    std::cerr << "fatal: unknown error\n";
    return 1;
  }
}
