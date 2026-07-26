/// @file svc.cc
/// @brief Background-service supervision — start, verify, report.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/svc.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "einheit/s5/util.h"

namespace einheit::s5::svc {
namespace {

namespace fs = std::filesystem;
using util::RunCmd;

/// Services this switch knows how to run, in the order an operator
/// reads them. `show system services` walks this list, so a service
/// missing from it would be invisible however broken it was.
///
/// `match` exists because not every service is findable by process
/// name. The LLDP daemon is this very binary in another mode, so
/// `pidof einheit_s5` would match the CLI the operator is typing into;
/// only the command line distinguishes them.
///
/// The bracket in `[e]inheit_s5` is load-bearing, not a typo. pgrep and
/// pkill take an extended regex and match every process's command
/// line — including the shell we spawned to run them, whose own
/// command line contains the pattern verbatim. Without the bracket,
/// `pgrep -f` always reports a match (so the daemon is never started)
/// and `pkill -f` kills its own parent shell. `[e]inheit_s5` matches
/// the daemon's `einheit_s5` while the searching shell's literal
/// `[e]inheit_s5` does not match the regex.
struct Known {
  const char *name;
  const char *match;
  const char *what;
};
constexpr Known kKnown[] = {
    {"mstpd", "", "spanning tree"},
    {"lldp", "[e]inheit_s5 --lldp-daemon", "LLDP neighbour discovery"},
    {"dnsmasq", "", "DHCP server and DNS forwarder"},
    {"mdns-repeater", "", "mDNS reflection between VLANs"},
    {"ntpd", "", "time"},
};

auto MatchFor(const std::string &name) -> std::string {
  for (const auto &k : kKnown) {
    if (name == k.name) return k.match;
  }
  return "";
}

}  // namespace

auto RunDir() -> std::string { return "/var/run/einheit"; }

auto EnsureRunDir() -> bool {
  std::error_code ec;
  fs::create_directories(util::FsPath(RunDir()), ec);
  return !ec;
}

auto BinaryAvailable(const std::string &binary) -> bool {
  return !RunCmd(std::format("command -v {} 2>/dev/null", binary)).empty();
}

auto SelfExe() -> std::string {
  std::error_code ec;
  const auto p = fs::read_symlink("/proc/self/exe", ec);
  if (ec) return "";
  return p.string();
}

auto Running(const std::string &name) -> bool {
  const auto match = MatchFor(name);
  if (match.empty()) {
    return !RunCmd(std::format("pidof {} 2>/dev/null", name)).empty();
  }
  return !RunCmd(std::format("pgrep -f '{}' 2>/dev/null", match)).empty();
}

auto Stop(const std::string &name) -> bool {
  if (!Running(name)) return true;
  const auto match = MatchFor(name);
  const auto kill = match.empty()
                        ? std::format("pkill -x {}", name)
                        : std::format("pkill -f '{}'", match);
  RunCmd(std::format("{} 2>/dev/null; sleep 1", kill));
  if (!Running(name)) return true;
  // A daemon that ignores SIGTERM would otherwise leave the next start
  // fighting the old process over a socket or a lease file.
  RunCmd(std::format("{} -9 2>/dev/null; sleep 1", kill));
  return !Running(name);
}

auto Start(const Spec &spec) -> bool {
  // setsid, or the daemon dies with the CLI session that committed it
  // into existence. Backgrounding and redirection belong here rather
  // than in each caller's command string: a caller that appended its
  // own `&` would produce `... & ; sleep 1`, which is a shell syntax
  // error that silently starts nothing.
  RunCmd(std::format("setsid {} </dev/null >/dev/null 2>&1 &",
                     spec.command));
  // Not politeness: every daemon here forks, so the launch command
  // returning says nothing about whether the child survived reading
  // its configuration. The probe after the wait is the only honest
  // success test.
  RunCmd("sleep 1");
  return Running(spec.name);
}

auto Restart(const Spec &spec) -> bool {
  if (!Stop(spec.name)) return false;
  return Start(spec);
}

auto Reload(const std::string &name) -> bool {
  if (!Running(name)) return false;
  const auto match = MatchFor(name);
  const auto cmd = match.empty()
                       ? std::format("pkill -HUP -x {} 2>&1", name)
                       : std::format("pkill -HUP -f '{}' 2>&1", match);
  return RunCmd(cmd).empty();
}

auto WriteGenerated(const std::string &path, const std::string &content,
                    bool *changed) -> bool {
  std::string existing;
  {
    std::ifstream f(util::FsPath(path));
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      existing = ss.str();
    }
  }
  if (changed != nullptr) *changed = existing != content;
  if (existing == content) return true;
  std::error_code ec;
  fs::create_directories(
      fs::path(util::FsPath(path)).parent_path(), ec);
  return util::WriteFile(path, content);
}

auto WantPath(const std::string &name) -> std::string {
  return std::format("{}/{}.want", RunDir(), name);
}

auto SetWanted(const std::string &name, bool wanted,
               const std::string &detail) -> void {
  EnsureRunDir();
  if (!wanted) {
    std::error_code ec;
    fs::remove(util::FsPath(WantPath(name)), ec);
    return;
  }
  util::WriteFile(WantPath(name), detail + "\n");
}

auto GetAll() -> std::vector<Status> {
  std::vector<Status> out;
  for (const auto &k : kKnown) {
    Status s;
    s.name = k.name;
    s.running = Running(k.name);
    // Whether a service is WANTED cannot be read off the box — a
    // dnsmasq that died leaves nothing behind saying it should be
    // there. So the apply records the intent in a marker file next to
    // the generated config, rebuilt on every boot from the committed
    // configuration. That is what makes "configured but down" a state
    // the switch can report instead of one an operator discovers when
    // clients stop getting addresses.
    std::ifstream f(util::FsPath(WantPath(k.name)));
    s.wanted = f.good();
    std::string detail;
    if (s.wanted) std::getline(f, detail);
    s.detail = detail.empty() ? k.what : detail;
    out.push_back(s);
  }
  return out;
}

}  // namespace einheit::s5::svc
