/// @file util.h
/// @brief Shared helpers — RunCmd, ReadSysfs, ReadUint.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_UTIL_H_
#define EINHEIT_S5_UTIL_H_

#include <cstdint>
#include <string>

namespace einheit::s5::util {

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
