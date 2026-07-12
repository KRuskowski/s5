/// @file util.h
/// @brief Shared helpers — RunCmd, ReadSysfs, ReadUint.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_UTIL_H_
#define EINHEIT_S5_UTIL_H_

#include <cstdint>
#include <functional>
#include <string>

namespace einheit::s5::util {

/// Signature of the shell runner behind RunCmd.
using CmdRunner = std::function<std::string(const std::string &)>;

/// Replace the shell runner. Tests inject a fake box that records
/// mutations and serves canned command output; passing an empty
/// function restores the real popen runner.
auto SetCmdRunner(CmdRunner runner) -> void;

/// Root prefixed onto every file path the helpers touch (/sys,
/// /proc, /etc). Tests point this at a scratch tree so reads and
/// writes hit the fake box instead of the machine; empty (the
/// default) means the real filesystem.
auto SetFsRoot(std::string root) -> void;

/// `path` under the configured filesystem root.
auto FsPath(const std::string &path) -> std::string;

/// Run a shell command and return stdout.
auto RunCmd(const std::string &cmd) -> std::string;

/// Read a single line from a sysfs file.
auto ReadSysfs(const std::string &path) -> std::string;

/// Read an unsigned integer from a sysfs file.
auto ReadUint(const std::string &path) -> std::uint64_t;

/// Write a string to a file (e.g. /etc/hostname).
auto WriteFile(const std::string &path,
               const std::string &content) -> bool;

}  // namespace einheit::s5::util

#endif  // EINHEIT_S5_UTIL_H_
