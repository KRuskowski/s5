/// @file backend.cc
/// @brief S5Backend — schema + apply/read-running over the hardware.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/backend.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "einheit/s5/dsa.h"
#include "einheit/s5/fabric.h"
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
        speed:
          type: enum
          values: [auto, "10", "100", "1000"]
          default: "auto"
          help: "Link speed in Mbit/s, or auto-negotiate"
        duplex:
          type: enum
          values: [auto, half, full]
          default: "auto"
          help: "Duplex mode, or auto-negotiate"
        mtu:
          type: integer
          range: [1280, 9216]
          default: "1500"
          help: "Maximum transmission unit"
        flow_control:
          type: boolean
          default: "false"
          help: "802.3x pause frames"
        vlan:
          type: map
          key: integer
          value:
            type: enum
            values: [tagged, untagged, pvid, untagged-pvid]
            help: "802.1Q membership mode for this VID"

  mac:
    type: object
    fields:
      aging_time:
        type: integer
        range: [10, 1000000]
        default: "300"
        help: "Seconds before a learned MAC is forgotten"
      static:
        type: map
        key: string
        value:
          type: object
          fields:
            port:
              type: string
              help: "Port the static entry points at"
              example: "lan1"
            vlan:
              type: integer
              range: [1, 4094]
              default: "1"
              help: "VLAN the static entry belongs to"

  igmp_snooping:
    type: object
    fields:
      enabled:
        type: boolean
        default: "true"
        help: "Prune multicast to ports that asked for it"
      querier:
        type: boolean
        default: "false"
        help: "Send IGMP queries (needed with no router on the L2)"

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

/// DSA switch-tag bytes the conduit must carry on top of a user
/// port's MTU. The ksz9477 tag is 4 bytes; 8 leaves room for the
/// VLAN tag that rides with it rather than cutting it fine.
constexpr int kDsaTagOverhead = 8;

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

/// 802.1Q membership mode of one VID on one port, as the flag pair
/// `bridge vlan add` takes.
struct VlanMode {
  bool untagged = false;
  bool pvid = false;
};

auto ParseVlanMode(const std::string &v) -> std::optional<VlanMode> {
  if (v == "tagged") return VlanMode{false, false};
  if (v == "untagged") return VlanMode{true, false};
  if (v == "pvid") return VlanMode{false, true};
  if (v == "untagged-pvid") return VlanMode{true, true};
  return std::nullopt;
}

auto VlanModeName(bool untagged, bool pvid) -> std::string {
  if (untagged && pvid) return "untagged-pvid";
  if (untagged) return "untagged";
  if (pvid) return "pvid";
  return "tagged";
}

auto ParseInt(const std::string &s) -> std::optional<int> {
  if (s.empty() || s.size() > 9) return std::nullopt;
  int out = 0;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return std::nullopt;
    }
    out = out * 10 + (c - '0');
  }
  return out;
}

auto Lower(std::string s) -> std::string {
  for (char &c : s) {
    c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

/// aa:bb:cc:dd:ee:ff. Checked here rather than trusting the schema's
/// string type, because this value is interpolated into a shell line.
auto ValidMac(const std::string &s) -> bool {
  if (s.size() != 17) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i % 3 == 2) {
      if (s[i] != ':') return false;
    } else if (std::isxdigit(static_cast<unsigned char>(s[i])) == 0) {
      return false;
    }
  }
  return true;
}

auto ParseVid(const std::string &s) -> std::optional<int> {
  if (s.empty() || s.size() > 4) return std::nullopt;
  int vid = 0;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return std::nullopt;
    }
    vid = vid * 10 + (c - '0');
  }
  if (vid < 1 || vid > 4094) return std::nullopt;
  return vid;
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
  /// Per-port link parameters (WP1.1). Only the fields the candidate
  /// mentions are set; the rest stay nullopt and are left alone.
  struct PortLink {
    std::optional<std::string> speed;
    std::optional<std::string> duplex;
    std::optional<int> mtu;
    std::optional<bool> flow_control;
  };
  std::map<std::string, PortLink> port_links;
  /// Static fdb entries: mac → (port, vid). The candidate owns the
  /// full set, so entries on the box that are not listed are removed.
  struct StaticMac {
    std::string port;
    int vid = 1;
  };
  std::map<std::string, StaticMac> static_macs;
  std::optional<int> mac_aging;
  std::optional<bool> igmp_snooping;
  std::optional<bool> igmp_querier;
  /// Desired VLAN membership per port: vid → mode. A port present
  /// here owns its full membership set — VIDs on the box but not
  /// listed are removed on apply.
  std::map<std::string, std::map<int, VlanMode>> port_vlans;
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
    } else if (seg.size() == 3 && seg[0] == "ports" &&
               (seg[2] == "speed" || seg[2] == "duplex" ||
                seg[2] == "mtu" || seg[2] == "flow_control")) {
      if (!SafeToken(seg[1])) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("invalid port name at '{}'", path));
      }
      auto &link = plan.port_links[seg[1]];
      if (seg[2] == "speed") {
        if (value != "auto" && value != "10" && value != "100" &&
            value != "1000") {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected auto|10|100|1000",
                                  path));
        }
        link.speed = value;
      } else if (seg[2] == "duplex") {
        if (value != "auto" && value != "half" && value != "full") {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected auto|half|full", path));
        }
        link.duplex = value;
      } else if (seg[2] == "mtu") {
        const auto mtu = ParseInt(value);
        if (!mtu || *mtu < 1280 || *mtu > 9216) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': MTU range is 1280-9216", path));
        }
        link.mtu = *mtu;
      } else {
        const auto b = ParseBool(value);
        if (!b) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected true|false", path));
        }
        link.flow_control = *b;
      }
    } else if (seg.size() == 2 && seg[0] == "mac" &&
               seg[1] == "aging_time") {
      const auto secs = ParseInt(value);
      if (!secs || *secs < 10 || *secs > 1000000) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': range is 10-1000000", path));
      }
      plan.mac_aging = *secs;
    } else if (seg.size() == 4 && seg[0] == "mac" &&
               seg[1] == "static") {
      if (!ValidMac(seg[2])) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': not a MAC address", path));
      }
      auto &entry = plan.static_macs[Lower(seg[2])];
      if (seg[3] == "port") {
        if (!SafeToken(value)) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("invalid port at '{}'", path));
        }
        entry.port = value;
      } else if (seg[3] == "vlan") {
        const auto vid = ParseVid(value);
        if (!vid) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': VID must be 1-4094", path));
        }
        entry.vid = *vid;
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
      }
    } else if (seg.size() == 2 && seg[0] == "igmp_snooping") {
      const auto b = ParseBool(value);
      if (!b) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': expected true|false", path));
      }
      if (seg[1] == "enabled") {
        plan.igmp_snooping = *b;
      } else if (seg[1] == "querier") {
        plan.igmp_querier = *b;
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
      }
    } else if (seg.size() == 4 && seg[0] == "ports" &&
               seg[2] == "vlan") {
      const auto vid = ParseVid(seg[3]);
      const auto mode = ParseVlanMode(value);
      if (!SafeToken(seg[1]) || !vid) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': VID must be 1-4094", path));
      }
      if (!mode) {
        return Fail(
            ApplyError::ValidationFailed,
            std::format("'{}': expected tagged|untagged|pvid|"
                        "untagged-pvid",
                        path));
      }
      plan.port_vlans[seg[1]][*vid] = *mode;
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

  // The fabric first: enslaving a port and turning vlan_filtering on
  // are preconditions for every port and VLAN write below, and the
  // failure modes without them are ugly (`bridge vlan add` on an
  // unbridged port errors out; on a bridge without vlan_filtering it
  // succeeds and does nothing). Deliberately does NOT set `applied`:
  // the fabric is infrastructure the box needs whichever candidate is
  // being applied, so bringing it up cannot leave a candidate
  // half-applied.
  if (auto f = fabric::Ensure(fabric::S5Topology()); !f) {
    return fail(std::format("fabric bootstrap: {}", f.error().message));
  }

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
  if (!plan->port_vlans.empty()) {
    // Reconcile each configured port's membership against the box:
    // the candidate owns the full set, so stale VIDs are removed
    // and missing / flag-changed ones (re-)added — `bridge vlan
    // add` on an existing VID updates its flags.
    const auto live = dsa::GetVlans();
    for (const auto &[port, desired] : plan->port_vlans) {
      for (const auto &entry : live) {
        if (entry.port != port) continue;
        if (!desired.contains(entry.vid)) {
          if (!dsa::DelVlan(port, entry.vid)) {
            return fail(std::format(
                "removing VID {} from {} failed", entry.vid, port));
          }
          applied = true;
        }
      }
      for (const auto &[vid, mode] : desired) {
        const auto match = std::find_if(
            live.begin(), live.end(), [&](const auto &e) {
              return e.port == port && e.vid == vid;
            });
        const bool up_to_date =
            match != live.end() &&
            match->untagged == mode.untagged &&
            match->pvid == mode.pvid;
        if (up_to_date) continue;
        if (!dsa::AddVlan(port, static_cast<std::uint16_t>(vid),
                          mode.untagged, mode.pvid)) {
          return fail(std::format(
              "setting VID {} on {} failed (is the port bridged?)",
              vid, port));
        }
        applied = true;
      }
    }
  }
  // Port link parameters. The conduit-MTU invariant comes first: on
  // DSA the CPU-port netdev carries every user port's traffic plus the
  // switch tag, so a user MTU above the conduit's is silently dropped
  // on the CPU path. Raise the conduit before raising a user port, and
  // never lower it below what a user port already needs.
  if (!plan->port_links.empty()) {
    int max_user_mtu = 0;
    for (const auto &[name, link] : plan->port_links) {
      if (link.mtu) max_user_mtu = std::max(max_user_mtu, *link.mtu);
    }
    const auto topo = fabric::S5Topology();
    for (const auto &member : topo.members) {
      if (plan->port_links.contains(member)) continue;
      // Ports the candidate does not mention still ride the conduit.
      max_user_mtu = std::max(max_user_mtu,
                              dsa::GetPortParams(member).mtu);
    }
    if (max_user_mtu > 0) {
      const auto conduit = fabric::GetStatus(topo).conduit;
      if (!conduit.empty()) {
        const int needed = max_user_mtu + kDsaTagOverhead;
        if (dsa::GetPortParams(conduit).mtu < needed) {
          if (!dsa::SetPortMtu(conduit, needed)) {
            return fail(std::format(
                "raising conduit {} MTU to {} failed", conduit, needed));
          }
          applied = true;
        }
      }
    }
    for (const auto &[name, link] : plan->port_links) {
      // Every write below is guarded by a read: the candidate is seeded
      // from running, so a commit that changes one unrelated path would
      // otherwise re-issue an ethtool call for every port on the box.
      // Beyond being wasteful, that fails outright on hardware whose
      // driver has no ethtool ops — an `auto` -> `auto` no-op must
      // never be able to fail a commit.
      const auto current = dsa::GetPortParams(name);
      if (link.mtu && *link.mtu != current.mtu) {
        if (!dsa::SetPortMtu(name, *link.mtu)) {
          return fail(std::format("MTU on {} failed", name));
        }
        applied = true;
      }
      if (link.speed || link.duplex) {
        // ethtool will not force one half of the pair; resolve both
        // from the candidate, defaulting the unmentioned half to what
        // the box already has.
        const auto speed = link.speed.value_or(current.speed);
        const auto duplex = link.duplex.value_or(current.duplex);
        if (speed != current.speed || duplex != current.duplex) {
          if (!dsa::SetPortSpeedDuplex(name, speed, duplex)) {
            return fail(std::format(
                "speed/duplex on {} failed (forcing needs both)", name));
          }
          applied = true;
        }
      }
      if (link.flow_control && *link.flow_control != current.flow_control) {
        if (!dsa::SetPortFlowControl(name, *link.flow_control)) {
          return fail(std::format("flow control on {} failed", name));
        }
        applied = true;
      }
    }
  }

  // MAC ageing and static entries.
  if (plan->mac_aging) {
    if (!dsa::SetMacAging(fabric::S5Topology().bridge, *plan->mac_aging)) {
      return fail("MAC ageing time write failed");
    }
    applied = true;
  }
  {
    // The candidate owns the full static set: entries on the box that
    // the configuration no longer names are removed, the same way VLAN
    // membership reconciles.
    const auto live = dsa::GetMacTable();
    for (const auto &e : live) {
      if (!e.is_static) continue;
      // The bridge installs permanent entries of its own — each port's
      // own address, and the multicast groups it joins. They look
      // exactly like configured static entries. Trying to reconcile
      // them away fails the delete and takes the whole apply (and
      // therefore every boot) down with it.
      if (e.is_local || e.is_multicast) continue;
      const auto want = plan->static_macs.find(Lower(e.mac));
      const bool keep = want != plan->static_macs.end() &&
                        want->second.port == e.port &&
                        want->second.vid == e.vid;
      if (keep) continue;
      if (!dsa::DelStaticMac(e.mac, e.port, e.vid)) {
        return fail(std::format("removing static MAC {} failed", e.mac));
      }
      applied = true;
    }
    for (const auto &[mac, entry] : plan->static_macs) {
      if (entry.port.empty()) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("mac.static.{}: no port set", mac));
      }
      const bool present = std::any_of(
          live.begin(), live.end(), [&](const auto &e) {
            return e.is_static && Lower(e.mac) == mac &&
                   e.port == entry.port && e.vid == entry.vid;
          });
      if (present) continue;
      if (!dsa::AddStaticMac(mac, entry.port,
                             static_cast<std::uint16_t>(entry.vid))) {
        return fail(std::format("adding static MAC {} on {} failed", mac,
                                entry.port));
      }
      applied = true;
    }
  }

  // IGMP snooping.
  if (plan->igmp_snooping || plan->igmp_querier) {
    const auto bridge = fabric::S5Topology().bridge;
    const auto live = dsa::GetSnooping(bridge);
    if (plan->igmp_snooping && *plan->igmp_snooping != live.enabled) {
      if (!dsa::SetSnooping(bridge, *plan->igmp_snooping)) {
        return fail("IGMP snooping write failed");
      }
      applied = true;
    }
    if (plan->igmp_querier && *plan->igmp_querier != live.querier) {
      if (!dsa::SetQuerier(bridge, *plan->igmp_querier)) {
        return fail("IGMP querier write failed");
      }
      applied = true;
    }
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
  const auto ports = dsa::DiscoverPorts();
  const auto topo = fabric::S5Topology();
  for (const auto &name : ports) {
    const auto st = dsa::GetPortStatus(name);
    running[std::format("ports.{}.enabled", name)] =
        st.enabled ? "true" : "false";
    // Link parameters read back so boot-restore and the reconcile
    // overlay see them like every other config family.
    const auto link = dsa::GetPortParams(name);
    running[std::format("ports.{}.speed", name)] = link.speed;
    running[std::format("ports.{}.duplex", name)] = link.duplex;
    if (link.mtu > 0) {
      running[std::format("ports.{}.mtu", name)] =
          std::to_string(link.mtu);
    }
    running[std::format("ports.{}.flow_control", name)] =
        link.flow_control ? "true" : "false";
  }
  if (const auto aging = dsa::GetMacAging(topo.bridge); aging > 0) {
    running["mac.aging_time"] = std::to_string(aging);
  }
  for (const auto &e : dsa::GetMacTable()) {
    if (!e.is_static || e.is_local || e.is_multicast) continue;
    // Only switch ports: the bridge's own permanent entry is not a
    // configured static MAC.
    if (std::find(ports.begin(), ports.end(), e.port) == ports.end()) {
      continue;
    }
    running[std::format("mac.static.{}.port", Lower(e.mac))] = e.port;
    running[std::format("mac.static.{}.vlan", Lower(e.mac))] =
        std::to_string(e.vid);
  }
  if (fabric::GetStatus(topo).exists) {
    const auto snoop = dsa::GetSnooping(topo.bridge);
    running["igmp_snooping.enabled"] = snoop.enabled ? "true" : "false";
    running["igmp_snooping.querier"] = snoop.querier ? "true" : "false";
  }
  for (const auto &v : dsa::GetVlans()) {
    // Only switch ports — `bridge vlan show` also lists the bridge
    // device itself.
    if (std::find(ports.begin(), ports.end(), v.port) ==
        ports.end()) {
      continue;
    }
    running[std::format("ports.{}.vlan.{}", v.port, v.vid)] =
        VlanModeName(v.untagged, v.pvid);
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

auto S5Backend::EnsureFabric()
    -> std::expected<void, Error<fabric::FabricError>> {
  std::lock_guard<std::mutex> lk(mu_);
  return fabric::Ensure(fabric::S5Topology());
}

}  // namespace einheit::s5
