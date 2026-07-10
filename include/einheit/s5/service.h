/// @file service.h
/// @brief Daemon-side execution of s5 product wire commands.
///
/// The product half of the in-process request handler: reads
/// (port/PoE/system state) and operational actions (PoE reset,
/// ping, reboot, user management) execute here, on the apply side
/// of the wire — the adapter's render surface only decodes what
/// this returns. Config mutations do NOT pass through here; they
/// go through the confd runtime and S5Backend::Apply.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_SERVICE_H_
#define EINHEIT_S5_SERVICE_H_

#include <optional>

#include "einheit/cli/protocol/envelope.h"

namespace einheit::s5::service {

/// Execute one product wire command against the hardware/system.
/// @param req Decoded wire request.
/// @returns The response, or std::nullopt when the command is not
///   a product command (the caller forwards it to the confd
///   runtime).
auto HandleProduct(const einheit::cli::protocol::Request &req)
    -> std::optional<einheit::cli::protocol::Response>;

}  // namespace einheit::s5::service

#endif  // EINHEIT_S5_SERVICE_H_
