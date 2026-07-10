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
// Copyright (c) 2026 Einheit Networks

#include <unistd.h>

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
#include "einheit/s5/poe.h"
#include "einheit/s5/service.h"
#include "einheit/s5/switch_adapter.h"
#include "einheit/cli/audit.h"
#include "einheit/cli/auth.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/confd/runtime.h"
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
  einheit::s5::S5Backend backend(schema);
  confd::RuntimeOptions ropts;
  ropts.state_dir = state_dir;
  ropts.audit = MakeAuditSink(state_dir);
  confd::Runtime runtime(backend, ropts);

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
  (void)argc;
  // Contain half of crash safety, installed before anything else
  // can fault: no SIGPIPE kill on peer disconnect, and every
  // SEGV/ABRT/BUS/ILL/FPE is diagnosed (signal + last command +
  // backtrace) then re-raised for a core dump.
  signals::IgnoreSigpipe();
  const auto crash_log = std::format("{}/crash.log", StateDir());
  signals::InstallFaultHandlers(crash_log);

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
