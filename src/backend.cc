/// @file backend.cc
/// @brief S5Backend — schema + apply/read-running over the hardware.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/backend.h"

#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "einheit/s5/dsa.h"
#include "einheit/s5/poe.h"
#include "einheit/s5/sys.h"

namespace einheit::s5 {
namespace {

using cli::Error;
using cli::confd::ApplyError;
using cli::confd::Candidate;
using cli::confd::CommitId;
using cli::confd::Config;

constexpr const char *kSchemaYaml = R"yaml(
version: 1
product: s5

config:
  hostname:
    type: string
    help: "Switch hostname"
    example: "s5-rack01"

  interfaces:
    type: map
    key: string
    value:
      type: object
      fields:
        address:
          type: cidr
          help: "Static address + prefix"
          example: "10.0.0.2/24"
        dhcp:
          type: boolean
          default: "false"
          help: "Acquire the address via DHCP instead"

  dns:
    type: object
    fields:
      primary:
        type: ip
        help: "Primary DNS nameserver"
        example: "9.9.9.9"
      secondary:
        type: ip
        help: "Secondary DNS nameserver"

  ntp:
    type: object
    fields:
      server:
        type: string
        help: "NTP server hostname or IP"
        example: "pool.ntp.org"

  ports:
    type: map
    key: string
    value:
      type: object
      fields:
        enabled:
          type: boolean
          default: "true"
          help: "Administrative port state (lan1..lan5, wan)"

  poe:
    type: map
    key: string
    value:
      type: object
      fields:
        enabled:
          type: boolean
          default: "true"
          help: "PoE power delivery on this port (1-5)"
        power_limit_mw:
          type: integer
          range: [0, 30000]
          help: "Per-port power limit in milliwatts"

types: {}
)yaml";

auto Trim(std::string s) -> std::string {
  while (!s.empty() && std::isspace(
             static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  std::size_t i = 0;
  while (i < s.size() && std::isspace(
             static_cast<unsigned char>(s[i]))) {
    ++i;
  }
  return s.substr(i);
}

/// Split a dotted candidate path into segments.
auto Split(const std::string &path) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::string cur;
  for (char c : path) {
    if (c == '.') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

auto ParseBool(const std::string &v) -> std::optional<bool> {
  if (v == "true") return true;
  if (v == "false") return false;
  return std::nullopt;
}

auto ParsePoePort(const std::string &key) -> std::optional<int> {
  if (key.size() != 1 || key[0] < '1' || key[0] > '5') {
    return std::nullopt;
  }
  return key[0] - '0';
}

/// Values that end up inside RunCmd shell lines must never carry
/// shell metacharacters. The schema validates types; this guards
/// the string-typed leaves (hostname, ntp server, iface names).
auto SafeToken(const std::string &v) -> bool {
  if (v.empty()) return false;
  for (char c : v) {
    const bool ok = std::isalnum(static_cast<unsigned char>(c)) ||
                    c == '.' || c == ':' || c == '-' || c == '_' ||
                    c == '/';
    if (!ok) return false;
  }
  return true;
}

auto Fail(ApplyError code, std::string msg)
    -> std::unexpected<Error<ApplyError>> {
  return std::unexpected(Error<ApplyError>{code, std::move(msg)});
}

/// The candidate regrouped into per-subsystem write plans, so the
/// apply order is deterministic regardless of map iteration order
/// (e.g. an interface's dhcp flag is considered together with its
/// address).
struct Plan {
  std::optional<std::string> hostname;
  struct Iface {
    std::optional<std::string> address;
    bool dhcp = false;
  };
  std::map<std::string, Iface> interfaces;
  std::vector<std::string> dns;
  std::optional<std::string> ntp_server;
  std::map<std::string, bool> ports;
  struct Poe {
    std::optional<bool> enabled;
    std::optional<int> power_limit_mw;
  };
  std::map<int, Poe> poe;
};

/// Validate the candidate and regroup it into a Plan without
/// touching hardware, so a rejected candidate leaves the box
/// untouched (ValidationFailed, not PartialApply).
auto BuildPlan(const Candidate &candidate)
    -> std::expected<Plan, Error<ApplyError>> {
  Plan plan;
  std::string dns_primary;
  std::string dns_secondary;
  for (const auto &[path, value] : candidate.values) {
    const auto seg = Split(path);
    if (seg.size() == 1 && seg[0] == "hostname") {
      if (!SafeToken(value)) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("invalid hostname '{}'", value));
      }
      plan.hostname = value;
    } else if (seg.size() == 3 && seg[0] == "interfaces") {
      if (!SafeToken(seg[1]) || !SafeToken(value)) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("invalid value at '{}'", path));
      }
      auto &iface = plan.interfaces[seg[1]];
      if (seg[2] == "address") {
        iface.address = value;
      } else if (seg[2] == "dhcp") {
        const auto b = ParseBool(value);
        if (!b) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected true|false", path));
        }
        iface.dhcp = *b;
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
      }
    } else if (seg.size() == 2 && seg[0] == "dns") {
      if (!SafeToken(value)) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("invalid nameserver '{}'", value));
      }
      if (seg[1] == "primary") {
        dns_primary = value;
      } else if (seg[1] == "secondary") {
        dns_secondary = value;
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
      }
    } else if (seg.size() == 2 && seg[0] == "ntp" &&
               seg[1] == "server") {
      if (!SafeToken(value)) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("invalid NTP server '{}'", value));
      }
      plan.ntp_server = value;
    } else if (seg.size() == 3 && seg[0] == "ports" &&
               seg[2] == "enabled") {
      const auto b = ParseBool(value);
      if (!SafeToken(seg[1]) || !b) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("invalid value at '{}'", path));
      }
      plan.ports[seg[1]] = *b;
    } else if (seg.size() == 3 && seg[0] == "poe") {
      if (!poe::Available()) {
        // Rejected at validation, before any write, so a PoE-less
        // box (or a dead bus) never half-applies a candidate.
        return Fail(ApplyError::ValidationFailed,
                    "PoE bus unavailable on this box");
      }
      const auto port = ParsePoePort(seg[1]);
      if (!port) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': PoE port must be 1-5", path));
      }
      auto &poe = plan.poe[*port];
      if (seg[2] == "enabled") {
        const auto b = ParseBool(value);
        if (!b) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected true|false", path));
        }
        poe.enabled = *b;
      } else if (seg[2] == "power_limit_mw") {
        int mw = 0;
        try {
          mw = std::stoi(value);
        } catch (...) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected integer", path));
        }
        if (mw < 0 || mw > 30000) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': range is 0-30000", path));
        }
        poe.power_limit_mw = mw;
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
      }
    } else {
      return Fail(ApplyError::ValidationFailed,
                  std::format("unknown path '{}'", path));
    }
  }
  if (!dns_primary.empty() || !dns_secondary.empty()) {
    if (!dns_primary.empty()) plan.dns.push_back(dns_primary);
    if (!dns_secondary.empty()) plan.dns.push_back(dns_secondary);
  }
  return plan;
}

}  // namespace

auto MakeS5Schema() -> cli::schema::SchemaHandle {
  auto s = cli::schema::LoadSchemaFromString(kSchemaYaml);
  // A parse failure of the baked-in literal falls back to the
  // empty DefaultSchema instead of a null handle (gap #5).
  if (!s) return {};
  return cli::schema::SchemaHandle(*s);
}

S5Backend::S5Backend(cli::schema::SchemaHandle schema)
    : schema_(std::move(schema)) {}

auto S5Backend::Apply(const Candidate &candidate)
    -> std::expected<CommitId, Error<ApplyError>> {
  std::lock_guard<std::mutex> lk(mu_);
  auto plan = BuildPlan(candidate);
  if (!plan) return std::unexpected(plan.error());

  // Everything below mutates the box. The first failure before any
  // write is HardwareRejected (running state unchanged); a failure
  // after a successful write is PartialApply (the box may be
  // inconsistent) — the runtime treats both as a failed commit and
  // does not advance its running config.
  bool applied = false;
  auto fail = [&applied](const std::string &what)
      -> std::unexpected<Error<ApplyError>> {
    return std::unexpected(Error<ApplyError>{
        applied ? ApplyError::PartialApply
                : ApplyError::HardwareRejected,
        what});
  };

  if (plan->hostname) {
    if (!sys::SetHostname(*plan->hostname)) {
      return fail("hostname write failed");
    }
    applied = true;
  }
  for (const auto &[name, iface] : plan->interfaces) {
    if (iface.dhcp) {
      if (!sys::SetInterfaceDhcp(name)) {
        return fail(std::format("DHCP start on {} failed", name));
      }
    } else if (iface.address) {
      if (!sys::SetInterfaceAddr(name, *iface.address)) {
        return fail(std::format("address set on {} failed", name));
      }
    }
    applied = true;
  }
  if (!plan->dns.empty()) {
    if (!sys::SetDnsServers(plan->dns)) {
      return fail("DNS write failed");
    }
    applied = true;
  }
  if (plan->ntp_server) {
    if (!sys::SetNtpServer(*plan->ntp_server)) {
      return fail("NTP write failed");
    }
    applied = true;
  }
  for (const auto &[name, enabled] : plan->ports) {
    if (!dsa::SetPortEnabled(name, enabled)) {
      return fail(std::format("port {} admin state failed", name));
    }
    applied = true;
  }
  for (const auto &[port, poe] : plan->poe) {
    if (poe.enabled) {
      if (!poe::SetPortEnabled(port, *poe.enabled)) {
        return fail(std::format("PoE enable on port {} failed", port));
      }
      applied = true;
    }
    if (poe.power_limit_mw) {
      if (!poe::SetPowerLimit(
              port,
              static_cast<std::uint32_t>(*poe.power_limit_mw))) {
        return fail(std::format("PoE limit on port {} failed", port));
      }
      applied = true;
    }
  }

  return ++rev_;
}

auto S5Backend::ReadRunning() -> Config {
  std::lock_guard<std::mutex> lk(mu_);
  Config running;
  if (auto h = Trim(sys::GetHostname()); !h.empty()) {
    running["hostname"] = h;
  }
  const auto dns = sys::GetDnsServers();
  if (dns.size() > 0) running["dns.primary"] = Trim(dns[0]);
  if (dns.size() > 1) running["dns.secondary"] = Trim(dns[1]);
  // Only record a plausible server name — on a box without ntpd
  // the probe returns an error string, not a value.
  if (auto ntp = Trim(sys::GetNtpStatus().server); SafeToken(ntp)) {
    running["ntp.server"] = ntp;
  }
  for (const auto &name : dsa::DiscoverPorts()) {
    const auto st = dsa::GetPortStatus(name);
    running[std::format("ports.{}.enabled", name)] =
        st.enabled ? "true" : "false";
  }
  // Only seed poe paths when the bus exists — otherwise every
  // commit would re-apply phantom poe values and fail mid-apply
  // on a PoE-less box.
  if (poe::Available()) {
    for (int port = 1; port <= 5; ++port) {
      const auto st = poe::GetPortStatus(port);
      running[std::format("poe.{}.enabled", port)] =
          st.enabled ? "true" : "false";
    }
  }
  return running;
}

auto S5Backend::Schema() const -> const cli::schema::Schema & {
  return schema_.Get();
}

}  // namespace einheit::s5
