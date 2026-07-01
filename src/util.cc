/// @file util.cc
/// @brief Shared helpers — RunCmd, ReadSysfs, WriteFile.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/util.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace einheit::s5::util {

auto RunCmd(const std::string &cmd) -> std::string {
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

auto ReadSysfs(const std::string &path) -> std::string {
  std::ifstream f(path);
  std::string val;
  if (f) std::getline(f, val);
  return val;
}

auto ReadUint(const std::string &path) -> std::uint64_t {
  auto s = ReadSysfs(path);
  if (s.empty()) return 0;
  return std::stoull(s);
}

auto WriteFile(const std::string &path,
               const std::string &content) -> bool {
  std::ofstream f(path, std::ios::trunc);
  if (!f) return false;
  f << content;
  return f.good();
}

}  // namespace einheit::s5::util
