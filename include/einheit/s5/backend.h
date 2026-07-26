/// @file backend.h
/// @brief S5Backend — the confd ConfigBackend over the real box.
///
/// The daemon-side apply seam (gap #4): committed candidates are
/// programmed onto the hardware here — DSA netdevs, the TPS23861
/// PoE controller, and the host system (hostname / DNS / NTP /
/// interface addressing) — never from the adapter's display-only
/// render surface.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_BACKEND_H_
#define EINHEIT_S5_BACKEND_H_

#include <expected>
#include <mutex>
#include <string>
#include <vector>

#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/error.h"
#include "einheit/cli/schema.h"
#include "einheit/s5/fabric.h"

namespace einheit::s5 {

/// The baked-in s5 config schema. Parsed from an in-code YAML
/// literal (no /tmp round-trip); never null by construction.
auto MakeS5Schema() -> einheit::cli::schema::SchemaHandle;

/// ConfigBackend over the real switch. Thread-safe: Apply and
/// ReadRunning take an internal lock, so the confd runtime's
/// commit-confirmed timer thread and the in-process CLI can share
/// one instance.
class S5Backend : public einheit::cli::confd::ConfigBackend {
 public:
  /// Construct over the shared s5 schema handle.
  /// @param schema Schema backing validation (shared with the
  ///   CLI adapter so both sides agree on paths).
  explicit S5Backend(einheit::cli::schema::SchemaHandle schema);

  auto Apply(const einheit::cli::confd::Candidate &candidate)
      -> std::expected<
          einheit::cli::confd::CommitId,
          einheit::cli::Error<einheit::cli::confd::ApplyError>>
      override;
  auto ReadRunning() -> einheit::cli::confd::Config override;
  auto Schema() const
      -> const einheit::cli::schema::Schema & override;
  auto Warnings(const einheit::cli::confd::Candidate &candidate) const
      -> std::vector<std::string> override;

  /// Bring the switch fabric up (bridge + vlan_filtering + enslaved
  /// ports + conduit), idempotently. Apply calls this before touching
  /// any port or VLAN, because a `bridge vlan` entry on an unbridged
  /// port fails and a VLAN entry on a bridge without vlan_filtering is
  /// silently inert. Exposed separately so the boot path can construct
  /// the fabric on a box with no commit history to restore.
  /// @returns void, or the first fabric command that failed.
  auto EnsureFabric()
      -> std::expected<void, einheit::cli::Error<fabric::FabricError>>;

 private:
  einheit::cli::schema::SchemaHandle schema_;
  std::mutex mu_;
  einheit::cli::confd::CommitId rev_ = 0;
};

}  // namespace einheit::s5

#endif  // EINHEIT_S5_BACKEND_H_
