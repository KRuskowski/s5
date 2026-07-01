/// @file poe.cc
/// @brief TPS23861 PoE PSE controller — I2C register interface.
///
/// Two TPS23861 ICs on the same I2C bus:
///   U9  (addr 0x20) — ports 1-4
///   U13 (addr 0x28) — port 5 (channel 1 only)
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/poe.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

namespace einheit::s5::poe {
namespace {

constexpr std::uint8_t kAddrU9 = 0x20;
constexpr std::uint8_t kAddrU13 = 0x28;

// TPS23861 register addresses (from datasheet).
constexpr std::uint8_t kRegPowerEnable = 0x19;
constexpr std::uint8_t kRegDetectClass = 0x14;
constexpr std::uint8_t kRegPowerStatus = 0x10;
constexpr std::uint8_t kRegPortCurrent1 = 0x30;
constexpr std::uint8_t kRegPortVoltage1 = 0x32;

int i2c_fd_ = -1;

auto SelectDevice(std::uint8_t addr) -> bool {
  return ioctl(i2c_fd_, I2C_SLAVE, addr) >= 0;
}

auto ReadReg(std::uint8_t addr, std::uint8_t reg)
    -> std::uint8_t {
  if (!SelectDevice(addr)) return 0;
  std::uint8_t val = 0;
  if (write(i2c_fd_, &reg, 1) == 1) {
    read(i2c_fd_, &val, 1);
  }
  return val;
}

auto WriteReg(std::uint8_t addr, std::uint8_t reg,
              std::uint8_t val) -> bool {
  if (!SelectDevice(addr)) return false;
  std::uint8_t buf[2] = {reg, val};
  return write(i2c_fd_, buf, 2) == 2;
}

struct ChipChannel {
  std::uint8_t addr;
  int channel;  // 0-3 within the TPS23861
};

auto MapPort(int port) -> ChipChannel {
  if (port >= 1 && port <= 4) {
    return {kAddrU9, port - 1};
  }
  return {kAddrU13, 0};  // port 5 -> U13 channel 0
}

}  // namespace

auto Init(const std::string &bus) -> bool {
  i2c_fd_ = open(bus.c_str(), O_RDWR);
  return i2c_fd_ >= 0;
}

auto GetPortStatus(int port) -> PortPoeStatus {
  PortPoeStatus s;
  s.port = port;

  auto [addr, ch] = MapPort(port);

  auto power_status = ReadReg(addr, kRegPowerStatus);
  s.enabled = (power_status >> (ch * 2)) & 0x01;
  s.delivering = (power_status >> (ch * 2 + 1)) & 0x01;

  // TODO: read actual voltage/current registers and scale
  // per TPS23861 datasheet formulas. These are placeholder
  // reads that need the real register map + scaling.
  s.voltage_v = 0.0;
  s.current_ma = 0.0;
  s.power_w = 0.0;
  s.status = s.delivering ? "delivering" :
             s.enabled ? "searching" : "disabled";
  s.classification = "unknown";

  return s;
}

auto GetAllStatus() -> std::vector<PortPoeStatus> {
  std::vector<PortPoeStatus> result;
  for (int p = 1; p <= 5; ++p) {
    result.push_back(GetPortStatus(p));
  }
  return result;
}

auto SetPortEnabled(int port, bool enabled) -> bool {
  auto [addr, ch] = MapPort(port);
  auto val = ReadReg(addr, kRegPowerEnable);
  if (enabled) {
    val |= (1 << ch);
  } else {
    val &= ~(1 << ch);
  }
  return WriteReg(addr, kRegPowerEnable, val);
}

auto SetPowerLimit(int port, std::uint32_t limit_mw) -> bool {
  // TODO: implement per TPS23861 power management registers.
  (void)port;
  (void)limit_mw;
  return false;
}

auto ResetPort(int port) -> bool {
  if (!SetPortEnabled(port, false)) return false;
  usleep(500000);  // 500ms power-off
  return SetPortEnabled(port, true);
}

auto GetTotalPower() -> double {
  double total = 0.0;
  for (int p = 1; p <= 5; ++p) {
    total += GetPortStatus(p).power_w;
  }
  return total;
}

}  // namespace einheit::s5::poe
