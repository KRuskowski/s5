/// @file util.cc
/// @brief Shared helpers — RunCmd, ReadSysfs, WriteFile.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/util.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <utility>

namespace einheit::s5::util {
namespace {

auto PopenRunner(const std::string &cmd) -> std::string {
  std::string result;
  std::array<char, 256> buf;
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp) return result;
  while (fgets(buf.data(), buf.size(), fp)) {
    result += buf.data();
  }
  pclose(fp);
  return result;
}

CmdRunner runner_;
std::string fs_root_;

}  // namespace

auto SetCmdRunner(CmdRunner runner) -> void {
  runner_ = std::move(runner);
}

auto SetFsRoot(std::string root) -> void {
  fs_root_ = std::move(root);
}

auto FsPath(const std::string &path) -> std::string {
  return fs_root_ + path;
}

auto RunCmd(const std::string &cmd) -> std::string {
  if (runner_) return runner_(cmd);
  return PopenRunner(cmd);
}

auto ReadSysfs(const std::string &path) -> std::string {
  std::ifstream f(FsPath(path));
  std::string val;
  if (f) std::getline(f, val);
  return val;
}

auto ReadUint(const std::string &path) -> std::uint64_t {
  auto s = ReadSysfs(path);
  if (s.empty()) return 0;
  try {
    return std::stoull(s);
  } catch (...) {
    return 0;
  }
}

auto WriteFile(const std::string &path,
               const std::string &content) -> bool {
  std::ofstream f(FsPath(path), std::ios::trunc);
  if (!f) return false;
  f << content;
  return f.good();
}

}  // namespace einheit::s5::util
