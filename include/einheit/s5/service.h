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

#include <functional>
#include <optional>
#include <string>

#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/protocol/envelope.h"

namespace einheit::s5::service {

/// Let the service read the running configuration.
///
/// Almost everything a `show` verb prints is read off the box, which
/// is the right default: it reports what IS, not what was asked for.
/// A few things have no counterpart on the box at all — a VLAN's name
/// exists only in the configuration — and printing a VLAN table
/// without names because of an architectural preference would be
/// serving the architecture rather than the operator.
/// @param reader Returns the running config, or empty when unset.
auto SetRunningConfigReader(
    std::function<einheit::cli::confd::Config()> reader) -> void;

/// Point the service at the durable state directory. `show system`
/// reads the framework's boot report from there for its
/// config-divergence row, which is the one piece of framework state a
/// product read needs. Same seam shape as util::SetFsRoot; tests point
/// it at a scratch tree.
/// @param dir State directory, or empty to disable the lookup.
auto SetStateDir(std::string dir) -> void;

/// Drop the process-wide caches (discovered port list, `clear
/// counters` baselines). Tests swap the fake box between cases and
/// would otherwise inherit the previous case's box.
auto ResetCachesForTesting() -> void;

/// Execute one product wire command against the hardware/system.
/// @param req Decoded wire request.
/// @returns The response, or std::nullopt when the command is not
///   a product command (the caller forwards it to the confd
///   runtime).
auto HandleProduct(const einheit::cli::protocol::Request &req)
    -> std::optional<einheit::cli::protocol::Response>;

}  // namespace einheit::s5::service

#endif  // EINHEIT_S5_SERVICE_H_
