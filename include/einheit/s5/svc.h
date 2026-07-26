/// @file svc.h
/// @brief Supervision of the background services the switch runs.
///
/// From Phase 2 on, several features are a daemon plus a generated
/// configuration file: dnsmasq for DHCP and DNS, mdns-repeater for
/// cross-VLAN discovery, busybox ntpd for time, and the s5's own LLDP
/// daemon. They all need the same four things, and getting any of them
/// wrong is how a feature "succeeds" without working:
///
///  1. The binary must EXIST before a commit that needs it is allowed
///     to succeed. A missing dnsmasq is a failed commit, not a silent
///     no-op (the SetNtpServer lesson: the apply that reported success
///     while nothing was listening).
///  2. Starting must be VERIFIED, not assumed — daemons fork, so the
///     launch command exiting 0 says nothing about whether the process
///     survived reading its config.
///  3. Restarts must be idempotent and quiet: an unrelated commit must
///     not bounce a running DHCP server and take every lease with it.
///  4. The state must be visible: `show system services` is where an
///     operator finds out that DHCP is configured but dnsmasq died.
///
/// Deliberately NOT a service manager. Restart-on-crash belongs to
/// init; this is the apply-time seam plus a health read.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_SVC_H_
#define EINHEIT_S5_SVC_H_

#include <string>
#include <vector>

namespace einheit::s5::svc {

/// How to launch one background service.
struct Spec {
  /// Process name, as `pidof` sees it. Also the name shown by
  /// `show system services`.
  std::string name;
  /// Command line, binary first. Every interpolated value must already
  /// have been validated — this string reaches a shell.
  std::string command;
};

/// Directory for generated configuration and runtime files. A tmpfs
/// path on the real image: these are apply artifacts rebuilt from the
/// committed configuration on every boot, never operator-edited state.
auto RunDir() -> std::string;

/// Ensure RunDir() exists.
auto EnsureRunDir() -> bool;

/// Whether `binary` is on the PATH.
auto BinaryAvailable(const std::string &binary) -> bool;

/// Absolute path of the running executable. The LLDP daemon is this
/// binary in another mode, so starting it means naming ourselves —
/// and it has to be the resolved path, not `/proc/self/exe`, because
/// the shell that launches it has its own `/proc/self`.
/// @returns The path, or empty when it cannot be resolved.
auto SelfExe() -> std::string;

/// Whether a process of that name is running.
auto Running(const std::string &name) -> bool;

/// Stop every process of that name. Succeeds when none is left,
/// including when none was running to begin with.
auto Stop(const std::string &name) -> bool;

/// Start a service and confirm it is still alive afterwards. A daemon
/// that forks and then dies on a bad config file would otherwise look
/// exactly like a successful start.
/// @param spec What to launch.
/// @returns Whether the process is running when this returns.
auto Start(const Spec &spec) -> bool;

/// Stop then start. Used when a service has no reload path, or when
/// its configuration changed in a way SIGHUP does not pick up.
auto Restart(const Spec &spec) -> bool;

/// Ask a running service to re-read its configuration (SIGHUP).
auto Reload(const std::string &name) -> bool;

/// Write `content` to `path` and report whether it CHANGED. Callers
/// use the answer to decide whether the service has to be bounced at
/// all: rewriting an identical dnsmasq config and restarting anyway
/// would drop every DHCP lease on a commit that touched the hostname.
/// @param path Absolute path of the generated file.
/// @param content Full desired file contents.
/// @param changed Set to whether the file's content differs from what
///   was already there (true when the file did not exist).
/// @returns Whether the write succeeded.
auto WriteGenerated(const std::string &path, const std::string &content,
                    bool *changed) -> bool;

/// Record whether the committed configuration asks for this service,
/// so `show system services` can tell "not configured" from
/// "configured and dead". Written as a marker file beside the
/// generated config — an apply artifact, rebuilt at every boot.
/// @param name Process name.
/// @param wanted Whether the configuration calls for it.
/// @param detail One line describing what it is doing for us.
auto SetWanted(const std::string &name, bool wanted,
               const std::string &detail) -> void;

/// One row of `show system services`.
struct Status {
  std::string name;
  /// Whether the configuration asks for this service at all.
  bool wanted = false;
  /// Whether it is actually running.
  bool running = false;
  /// What it is doing, or why it is not.
  std::string detail;
};

/// Read the state of every service the switch knows how to run.
/// Derived from the box, not from configuration, so a service that
/// died since the last commit shows up as down.
auto GetAll() -> std::vector<Status>;

}  // namespace einheit::s5::svc

#endif  // EINHEIT_S5_SVC_H_
