/// @file switch_adapter.h
/// @brief Factory for the s5 CLI-side product adapter.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_SWITCH_ADAPTER_H_
#define EINHEIT_S5_SWITCH_ADAPTER_H_

#include <memory>

#include "einheit/cli/adapter.h"
#include "einheit/cli/schema.h"

namespace einheit::s5 {

/// Build the s5 ProductAdapter. Declarative + render-only: it
/// contributes command specs and decodes response data; all
/// hardware access lives in the service (reads / operational
/// actions) and S5Backend (config apply).
/// @param schema The shared s5 schema handle (same one backing
///   the S5Backend, so completion and validation agree).
auto MakeSwitchAdapter(einheit::cli::schema::SchemaHandle schema)
    -> std::unique_ptr<einheit::cli::ProductAdapter>;

}  // namespace einheit::s5

#endif  // EINHEIT_S5_SWITCH_ADAPTER_H_
