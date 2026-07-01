/// @file poe.h
/// @brief TPS23861 PoE PSE controller interface over I2C.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_POE_H_
#define EINHEIT_S5_POE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace einheit::s5::poe {

struct PortPoeStatus {
  int port = 0;
  bool enabled = false;
  bool delivering = false;
  double voltage_v = 0.0;
  double current_ma = 0.0;
  double power_w = 0.0;
  std::string classification;
  std::string status;
};

/// Initialize the I2C bus for TPS23861 communication.
/// bus: e.g. "/dev/i2c-0"
auto Init(const std::string &bus) -> bool;

/// Get PoE status for a single port (1-5).
auto GetPortStatus(int port) -> PortPoeStatus;

/// Get PoE status for all ports.
auto GetAllStatus() -> std::vector<PortPoeStatus>;

/// Enable or disable PoE on a port (1-5).
auto SetPortEnabled(int port, bool enabled) -> bool;

/// Set power limit in milliwatts for a port (1-5).
auto SetPowerLimit(int port, std::uint32_t limit_mw) -> bool;

/// Reset a port (power cycle).
auto ResetPort(int port) -> bool;

/// Get total PoE power budget consumed (watts).
auto GetTotalPower() -> double;

}  // namespace einheit::s5::poe

#endif  // EINHEIT_S5_POE_H_
