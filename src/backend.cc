/// @file backend.cc
/// @brief S5Backend — schema + apply/read-running over the hardware.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/backend.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "einheit/s5/dnsmasq.h"
#include "einheit/s5/dsa.h"
#include "einheit/s5/fabric.h"
#include "einheit/s5/l3.h"
#include "einheit/s5/lldp.h"
#include "einheit/s5/poe.h"
#include "einheit/s5/stp.h"
#include "einheit/s5/svc.h"
#include "einheit/s5/sys.h"
#include "einheit/s5/util.h"

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
        gateway:
          type: ip
          help: "Default gateway reached through this interface"
          example: "10.0.0.1"

  vlans:
    type: map
    key: integer
    value:
      type: object
      fields:
        name:
          type: string
          help: "What this VLAN is for, e.g. office or guest"
          example: "office"
        address:
          type: cidr
          help: "Switch address in this VLAN; giving one makes it routable"
          example: "10.10.0.1/24"
        dhcp:
          type: object
          fields:
            enabled:
              type: boolean
              default: "false"
              help: "Hand out addresses to clients in this VLAN"
            range_start:
              type: ip
              help: "First address handed out"
              example: "10.10.0.100"
            range_end:
              type: ip
              help: "Last address handed out"
              example: "10.10.0.200"
            lease_time:
              type: integer
              range: [2, 10080]
              default: "720"
              help: "Minutes a client keeps its address"
            gateway:
              type: ip
              help: "Router told to clients; defaults to the switch itself"
            dns:
              type: ip
              help: "Nameserver told to clients; defaults to the switch itself"
            static:
              type: map
              key: string
              value:
                type: object
                fields:
                  ip:
                    type: ip
                    help: "Address always given to this MAC"
                    example: "10.10.0.50"

  routing:
    type: object
    fields:
      enabled:
        type: boolean
        default: "false"
        help: "Forward traffic between VLANs and to the uplink"
      static:
        type: map
        key: string
        value:
          type: object
          fields:
            prefix:
              type: cidr
              help: "Destination network; 0.0.0.0/0 is the default route"
              example: "192.168.5.0/24"
            via:
              type: ip
              help: "Next-hop address"
              example: "10.0.0.254"

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
      serve:
        type: boolean
        default: "false"
        help: "Answer DNS queries from clients, forwarding what we do not know"
      local_domain:
        type: string
        help: "Domain for names the switch serves itself"
        example: "office.lan"

  ntp:
    type: object
    fields:
      server:
        type: string
        help: "NTP server hostname or IP"
        example: "pool.ntp.org"
      serve:
        type: boolean
        default: "false"
        help: "Also answer time queries from clients on this network"

  mdns:
    type: object
    fields:
      enabled:
        type: boolean
        default: "false"
        help: "Repeat mDNS between VLANs so discovery crosses them"
      reflect:
        type: map
        key: integer
        value:
          type: boolean
          help: "Include this VLAN in mDNS reflection"

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
        stp:
          type: object
          fields:
            cost:
              type: integer
              range: [0, 200000000]
              default: "0"
              help: "Spanning-tree path cost; 0 derives it from link speed"
            priority:
              type: enum
              values: ["0", "16", "32", "48", "64", "80", "96", "112",
                       "128", "144", "160", "176", "192", "208", "224",
                       "240"]
              default: "128"
              help: "Port priority; breaks ties between equal-cost paths to the root"
            edge:
              type: boolean
              default: "false"
              help: "Access port: skip the listening delay and forward at once"
            bpdu_guard:
              type: boolean
              default: "false"
              help: "Block this port if another switch sends spanning-tree into it"

  stp:
    type: object
    fields:
      mode:
        type: enum
        values: [rstp, stp, off]
        default: "rstp"
        help: "Loop protection: rapid spanning tree, classic, or none"
      priority:
        type: enum
        values: ["0", "4096", "8192", "12288", "16384", "20480", "24576",
                 "28672", "32768", "36864", "40960", "45056", "49152",
                 "53248", "57344", "61440"]
        default: "32768"
        help: "Bridge priority; the lowest value on the network becomes the root"
      hello:
        type: integer
        range: [1, 10]
        default: "2"
        help: "Seconds between spanning-tree messages from the root"
      max_age:
        type: integer
        range: [6, 40]
        default: "20"
        help: "Seconds before a silent neighbour is assumed gone"
      forward_delay:
        type: integer
        range: [4, 30]
        default: "15"
        help: "Seconds a port waits before forwarding (classic mode only)"

  lldp:
    type: object
    fields:
      enabled:
        type: boolean
        default: "true"
        help: "Announce this switch to its neighbours, and record theirs"
      tx_interval:
        type: integer
        range: [5, 3600]
        default: "30"
        help: "Seconds between announcements"
      port:
        type: map
        key: string
        value:
          type: object
          fields:
            enabled:
              type: boolean
              default: "true"
              help: "Run LLDP on this port"

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
    std::optional<std::string> gateway;
  };
  std::map<std::string, Iface> interfaces;
  /// VLANs with an identity or an address (WP2.1). A VID listed here
  /// with an address gets an SVI; one with only a name is documentation
  /// that still has to survive a reboot.
  struct Vlan {
    std::optional<std::string> name;
    std::optional<std::string> address;
    /// Per-VLAN DHCP pool (WP2.3).
    struct Dhcp {
      bool enabled = false;
      std::string range_start;
      std::string range_end;
      int lease_time = 720;
      std::string gateway;
      std::string dns;
      /// MAC → fixed address.
      std::map<std::string, std::string> reservations;
    };
    Dhcp dhcp;
  };
  std::map<int, Vlan> vlans;
  /// DNS service (WP2.3): the forwarder half of the same dnsmasq.
  bool dns_serve = false;
  std::string dns_local_domain;
  /// Routing (WP2.2). The static set is owned outright by the config.
  std::optional<bool> routing_enabled;
  struct StaticRoute {
    std::string prefix;
    std::string via;
  };
  std::map<std::string, StaticRoute> static_routes;
  std::vector<std::string> dns;
  std::optional<std::string> ntp_server;
  /// NTP server role (WP2.5) — busybox ntpd's `-l`.
  bool ntp_serve = false;
  /// mDNS reflection (WP2.4). The map form is deliberate: the
  /// framework has no list-typed schema values yet, and inventing one
  /// here would be a framework change smuggled in as a feature.
  bool mdns_enabled = false;
  std::map<int, bool> mdns_reflect;
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
  /// Bridge-wide spanning tree (WP1.2). Absent fields are left alone.
  struct Stp {
    std::optional<std::string> mode;
    std::optional<int> priority;
    std::optional<int> hello;
    std::optional<int> max_age;
    std::optional<int> forward_delay;
  };
  Stp stp;
  /// Per-port spanning tree.
  struct PortStp {
    std::optional<int> cost;
    std::optional<int> priority;
    std::optional<bool> edge;
    std::optional<bool> bpdu_guard;
  };
  std::map<std::string, PortStp> port_stp;
  /// LLDP (WP1.3). `ports` holds the per-port enable overrides.
  struct Lldp {
    std::optional<bool> enabled;
    std::optional<int> tx_interval;
    std::map<std::string, bool> ports;
  };
  Lldp lldp;
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

/// Which switch port a peer's traffic arrives on, via its MAC in the
/// neighbour table and then the forwarding database. Empty when the
/// peer is not on a switch port at all (the management NIC, or an
/// address the box has never talked to).
/// @param peer Address of the far end of the session.
auto IngressPortFor(const std::string &peer) -> std::string {
  if (peer.empty()) return "";
  const auto neigh =
      util::RunCmd(std::format("ip neigh show {} 2>/dev/null", peer));
  std::istringstream ns(neigh);
  std::string token;
  std::string mac;
  while (ns >> token) {
    if (token == "lladdr") ns >> mac;
  }
  if (mac.empty()) return "";
  for (const auto &e : dsa::GetMacTable()) {
    if (Lower(e.mac) == Lower(mac)) return e.port;
  }
  return "";
}

/// Turn the DHCP/DNS half of a plan into the dnsmasq generator's
/// input. Pure, and used twice on purpose: once during validation to
/// prove the file can be rendered at all (so a hostile or incoherent
/// value fails the commit before anything is written), and once during
/// apply to produce it.
auto BuildDnsmasqConfig(const Plan &plan, const std::string &bridge)
    -> dnsmasq::Config {
  dnsmasq::Config cfg;
  cfg.dns_enabled = plan.dns_serve;
  cfg.local_domain = plan.dns_local_domain;
  cfg.forwarders = plan.dns;
  for (const auto &[vid, vlan] : plan.vlans) {
    if (!vlan.dhcp.enabled) continue;
    dnsmasq::Pool pool;
    pool.vid = vid;
    pool.interface = l3::SviName(bridge, vid);
    pool.range_start = vlan.dhcp.range_start;
    pool.range_end = vlan.dhcp.range_end;
    pool.netmask =
        vlan.address ? dnsmasq::NetmaskOf(*vlan.address) : std::string();
    pool.lease_minutes = vlan.dhcp.lease_time;
    pool.gateway = vlan.dhcp.gateway;
    pool.dns = vlan.dhcp.dns;
    for (const auto &[mac, ip] : vlan.dhcp.reservations) {
      pool.reservations.push_back({mac, ip});
    }
    cfg.pools.push_back(pool);
  }
  return cfg;
}

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
      } else if (seg[2] == "gateway") {
        iface.gateway = value;
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
    } else if (seg.size() == 3 && seg[0] == "vlans") {
      const auto vid = ParseVid(seg[1]);
      if (!vid) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': VID must be 1-4094", path));
      }
      auto &vlan = plan.vlans[*vid];
      if (seg[2] == "name") {
        // Names reach `show vlans` and nothing else, but they still
        // must not be able to smuggle a shell metacharacter into a
        // generated dnsmasq stanza later.
        if (!SafeToken(value)) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': letters, digits, . - _ only", path));
        }
        vlan.name = value;
      } else if (seg[2] == "address") {
        if (!SafeToken(value)) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': not an address", path));
        }
        vlan.address = value;
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
      }
    } else if (seg.size() == 4 && seg[0] == "vlans" && seg[2] == "dhcp") {
      const auto vid = ParseVid(seg[1]);
      if (!vid) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': VID must be 1-4094", path));
      }
      auto &dhcp = plan.vlans[*vid].dhcp;
      if (seg[3] == "enabled") {
        const auto b = ParseBool(value);
        if (!b) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected true|false", path));
        }
        dhcp.enabled = *b;
      } else if (seg[3] == "lease_time") {
        const auto mins = ParseInt(value);
        if (!mins || *mins < 2 || *mins > 10080) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': range is 2-10080 minutes", path));
        }
        dhcp.lease_time = *mins;
      } else if (seg[3] == "range_start" || seg[3] == "range_end" ||
                 seg[3] == "gateway" || seg[3] == "dns") {
        // dnsmasq's format has no quoting, so the generator refuses
        // anything that is not an address rather than escaping it.
        // Rejecting here as well keeps the error attached to the path
        // the operator typed.
        if (!SafeToken(value)) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': not an address", path));
        }
        if (seg[3] == "range_start") dhcp.range_start = value;
        if (seg[3] == "range_end") dhcp.range_end = value;
        if (seg[3] == "gateway") dhcp.gateway = value;
        if (seg[3] == "dns") dhcp.dns = value;
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
      }
    } else if (seg.size() == 6 && seg[0] == "vlans" && seg[2] == "dhcp" &&
               seg[3] == "static" && seg[5] == "ip") {
      const auto vid = ParseVid(seg[1]);
      if (!vid) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': VID must be 1-4094", path));
      }
      if (!ValidMac(seg[4])) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': not a MAC address", path));
      }
      if (!SafeToken(value)) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': not an address", path));
      }
      plan.vlans[*vid].dhcp.reservations[Lower(seg[4])] = value;
    } else if (seg.size() == 2 && seg[0] == "routing" &&
               seg[1] == "enabled") {
      const auto b = ParseBool(value);
      if (!b) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': expected true|false", path));
      }
      plan.routing_enabled = *b;
    } else if (seg.size() == 4 && seg[0] == "routing" &&
               seg[1] == "static") {
      if (!SafeToken(seg[2]) || !SafeToken(value)) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("invalid value at '{}'", path));
      }
      auto &route = plan.static_routes[seg[2]];
      if (seg[3] == "prefix") {
        if (value.find('/') == std::string::npos) {
          return Fail(
              ApplyError::ValidationFailed,
              std::format("'{}': expected a CIDR prefix (0.0.0.0/0 is "
                          "the default route)",
                          path));
        }
        route.prefix = value;
      } else if (seg[3] == "via") {
        route.via = value;
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
      }
    } else if (seg.size() == 2 && seg[0] == "dns") {
      if (!SafeToken(value)) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': letters, digits, . - _ only", path));
      }
      if (seg[1] == "primary") {
        dns_primary = value;
      } else if (seg[1] == "secondary") {
        dns_secondary = value;
      } else if (seg[1] == "serve") {
        const auto b = ParseBool(value);
        if (!b) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected true|false", path));
        }
        plan.dns_serve = *b;
      } else if (seg[1] == "local_domain") {
        plan.dns_local_domain = value;
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
    } else if (seg.size() == 2 && seg[0] == "ntp" && seg[1] == "serve") {
      const auto b = ParseBool(value);
      if (!b) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': expected true|false", path));
      }
      plan.ntp_serve = *b;
    } else if (seg.size() == 2 && seg[0] == "mdns" &&
               seg[1] == "enabled") {
      const auto b = ParseBool(value);
      if (!b) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': expected true|false", path));
      }
      plan.mdns_enabled = *b;
    } else if (seg.size() == 3 && seg[0] == "mdns" &&
               seg[1] == "reflect") {
      const auto vid = ParseVid(seg[2]);
      const auto b = ParseBool(value);
      if (!vid) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': VID must be 1-4094", path));
      }
      if (!b) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("'{}': expected true|false", path));
      }
      plan.mdns_reflect[*vid] = *b;
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
    } else if (seg.size() == 2 && seg[0] == "lldp") {
      if (seg[1] == "enabled") {
        const auto b = ParseBool(value);
        if (!b) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected true|false", path));
        }
        plan.lldp.enabled = *b;
      } else if (seg[1] == "tx_interval") {
        const auto secs = ParseInt(value);
        if (!secs || *secs < 5 || *secs > 3600) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': range is 5-3600 seconds", path));
        }
        plan.lldp.tx_interval = *secs;
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
      }
    } else if (seg.size() == 4 && seg[0] == "lldp" && seg[1] == "port" &&
               seg[3] == "enabled") {
      const auto b = ParseBool(value);
      if (!SafeToken(seg[2]) || !b) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("invalid value at '{}'", path));
      }
      plan.lldp.ports[seg[2]] = *b;
    } else if (seg.size() == 2 && seg[0] == "stp") {
      if (seg[1] == "mode") {
        if (value != "rstp" && value != "stp" && value != "off") {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected rstp|stp|off", path));
        }
        plan.stp.mode = value;
      } else if (seg[1] == "priority") {
        // The schema enumerates the sixteen legal values (the standard
        // gives priority four bits, scaled by 4096), so this is a
        // second line of defence rather than the primary check — and
        // the one that holds for a config file loaded off disk.
        const auto prio = ParseInt(value);
        if (!prio || *prio < 0 || *prio > 61440 || *prio % 4096 != 0) {
          return Fail(
              ApplyError::ValidationFailed,
              std::format("'{}': 0-61440 in steps of 4096", path));
        }
        plan.stp.priority = *prio;
      } else if (seg[1] == "hello") {
        const auto secs = ParseInt(value);
        if (!secs || *secs < 1 || *secs > 10) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': range is 1-10 seconds", path));
        }
        plan.stp.hello = *secs;
      } else if (seg[1] == "max_age") {
        const auto secs = ParseInt(value);
        if (!secs || *secs < 6 || *secs > 40) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': range is 6-40 seconds", path));
        }
        plan.stp.max_age = *secs;
      } else if (seg[1] == "forward_delay") {
        const auto secs = ParseInt(value);
        if (!secs || *secs < 4 || *secs > 30) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': range is 4-30 seconds", path));
        }
        plan.stp.forward_delay = *secs;
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
      }
    } else if (seg.size() == 4 && seg[0] == "ports" && seg[2] == "stp") {
      if (!SafeToken(seg[1])) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("invalid port name at '{}'", path));
      }
      auto &port_stp = plan.port_stp[seg[1]];
      if (seg[3] == "cost") {
        const auto cost = ParseInt(value);
        if (!cost || *cost < 0 || *cost > 200000000) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': range is 0-200000000", path));
        }
        port_stp.cost = *cost;
      } else if (seg[3] == "priority") {
        const auto prio = ParseInt(value);
        if (!prio || *prio < 0 || *prio > 240 || *prio % 16 != 0) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': 0-240 in steps of 16", path));
        }
        port_stp.priority = *prio;
      } else if (seg[3] == "edge" || seg[3] == "bpdu_guard") {
        const auto b = ParseBool(value);
        if (!b) {
          return Fail(ApplyError::ValidationFailed,
                      std::format("'{}': expected true|false", path));
        }
        if (seg[3] == "edge") {
          port_stp.edge = *b;
        } else {
          port_stp.bpdu_guard = *b;
        }
      } else {
        return Fail(ApplyError::ValidationFailed,
                    std::format("unknown path '{}'", path));
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
  // 802.1D ties the three bridge timers together; mstpd enforces the
  // relationship and rejects the individual write, which would surface
  // as an opaque mid-apply failure. Catch it here, where the candidate
  // can still be rejected without touching the box, and say which
  // constraint the operator broke. Unmentioned timers are checked
  // against the schema defaults — a candidate is seeded from running,
  // so in practice either all three are present or none is.
  if (plan.stp.hello || plan.stp.max_age || plan.stp.forward_delay) {
    const int hello = plan.stp.hello.value_or(2);
    const int max_age = plan.stp.max_age.value_or(20);
    const int fwd = plan.stp.forward_delay.value_or(15);
    if (2 * (fwd - 1) < max_age) {
      return Fail(ApplyError::ValidationFailed,
                  std::format("stp: 2 x (forward_delay - 1) must be >= "
                              "max_age ({} vs {})",
                              2 * (fwd - 1), max_age));
    }
    if (max_age < 2 * (hello + 1)) {
      return Fail(ApplyError::ValidationFailed,
                  std::format("stp: max_age must be >= 2 x (hello + 1) "
                              "({} vs {})",
                              max_age, 2 * (hello + 1)));
    }
  }
  // A static route needs both halves to be programmable, and the two
  // ways of asking for a default route must not disagree.
  {
    int gateways = 0;
    for (const auto &[iface, cfg] : plan.interfaces) {
      if (cfg.gateway) ++gateways;
    }
    if (gateways > 1) {
      return Fail(ApplyError::ValidationFailed,
                  "more than one interface has a gateway; a box has one "
                  "default route");
    }
    for (const auto &[name, route] : plan.static_routes) {
      if (route.prefix.empty()) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("routing.static.{}: no prefix set", name));
      }
      if (route.via.empty()) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("routing.static.{}: no via set", name));
      }
      // Two ways to say "default route" that would fight each other on
      // every apply. Reject the pair rather than let the last writer
      // win differently depending on map iteration order.
      if (route.prefix == "0.0.0.0/0" && gateways > 0) {
        return Fail(
            ApplyError::ValidationFailed,
            std::format("routing.static.{} is a default route and an "
                        "interface gateway is also set; pick one",
                        name));
      }
    }
  }
  // DHCP: a pool has to sit in a subnet the switch is actually in, or
  // dnsmasq refuses to start and takes the whole apply with it — after
  // the box has already been half-written. Everything below is checked
  // here, before any write.
  for (const auto &[vid, vlan] : plan.vlans) {
    if (!vlan.dhcp.enabled) continue;
    if (!vlan.address) {
      return Fail(
          ApplyError::ValidationFailed,
          std::format("vlans.{}.dhcp needs vlans.{}.address — a switch "
                      "cannot serve a subnet it is not in",
                      vid, vid));
    }
    if (vlan.dhcp.range_start.empty() || vlan.dhcp.range_end.empty()) {
      return Fail(ApplyError::ValidationFailed,
                  std::format("vlans.{}.dhcp needs both range_start and "
                              "range_end",
                              vid));
    }
    for (const auto &ip : {vlan.dhcp.range_start, vlan.dhcp.range_end}) {
      if (!dnsmasq::InSubnet(*vlan.address, ip)) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("vlans.{}.dhcp: {} is outside {}", vid, ip,
                                *vlan.address));
      }
    }
    for (const auto &[mac, ip] : vlan.dhcp.reservations) {
      if (!dnsmasq::InSubnet(*vlan.address, ip)) {
        return Fail(ApplyError::ValidationFailed,
                    std::format("vlans.{}.dhcp.static.{}: {} is outside {}",
                                vid, mac, ip, *vlan.address));
      }
    }
  }
  // mDNS reflection needs at least two VLANs to reflect BETWEEN, and
  // each of them needs an interface to listen on. A configuration
  // naming one VLAN would start a repeater that repeats nothing while
  // reading as enabled.
  if (plan.mdns_enabled) {
    std::vector<int> reflected;
    for (const auto &[vid, on] : plan.mdns_reflect) {
      if (!on) continue;
      const auto vlan = plan.vlans.find(vid);
      if (vlan == plan.vlans.end() || !vlan->second.address) {
        return Fail(
            ApplyError::ValidationFailed,
            std::format("mdns.reflect.{} needs vlans.{}.address — there "
                        "is no interface in that VLAN to reflect on",
                        vid, vid));
      }
      reflected.push_back(vid);
    }
    if (reflected.size() < 2) {
      return Fail(ApplyError::ValidationFailed,
                  "mdns.enabled needs at least two VLANs in "
                  "mdns.reflect; reflection between one is nothing");
    }
    // mdns-repeater's own limit, and a real one: it opens a socket per
    // interface and the array is fixed.
    if (reflected.size() > 5) {
      return Fail(ApplyError::ValidationFailed,
                  "mdns.reflect: at most five VLANs can be reflected");
    }
  }
  // The generator is the authority on what can be written safely, so
  // run it here: a value it refuses must fail the commit, not fail the
  // apply half-way through.
  if (auto rendered = dnsmasq::Render(
          BuildDnsmasqConfig(plan, fabric::S5Topology().bridge));
      !rendered) {
    return Fail(ApplyError::ValidationFailed, rendered.error().message);
  }
  // Spanning tree is a property of the bridge, so it only means
  // anything on a port that is IN the bridge. Configuring it on the
  // routed uplink would be accepted here and then rejected by mstpd
  // half-way through an apply.
  for (const auto &[port, unused] : plan.port_stp) {
    (void)unused;
    const auto &members = fabric::S5Topology().members;
    if (std::find(members.begin(), members.end(), port) == members.end()) {
      return Fail(
          ApplyError::ValidationFailed,
          std::format("ports.{}.stp: spanning tree applies to bridged "
                      "ports only",
                      port));
    }
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
    // Client and server are the same busybox daemon: `-l` makes it
    // answer as well as ask, so both halves have to be decided
    // together or turning serving on would silently drop the upstream
    // server.
    if (!sys::SetNtpServer(*plan->ntp_server, plan->ntp_serve)) {
      return fail(plan->ntp_serve
                      ? "NTP write failed (does this ntpd support -l?)"
                      : "NTP write failed");
    }
    applied = true;
  } else if (plan->ntp_serve) {
    return fail("ntp.serve needs ntp.server — a switch that serves time "
                "it never synchronised is worse than no server at all");
  }
  svc::SetWanted("ntpd", plan->ntp_server.has_value(),
                 plan->ntp_server
                     ? std::format("time from {}{}", *plan->ntp_server,
                                   plan->ntp_serve ? ", serving clients"
                                                   : "")
                     : "");
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

  // SVIs: a VLAN with an address becomes a routed interface on the
  // bridge. The configuration owns the full set, so an SVI whose
  // address the operator deleted goes away rather than lingering until
  // the next reboot.
  {
    const auto bridge = fabric::S5Topology().bridge;
    const auto live = l3::GetSvis(bridge);
    for (const auto &svi : live) {
      const auto want = plan->vlans.find(svi.vid);
      const bool keep =
          want != plan->vlans.end() && want->second.address.has_value();
      if (keep) continue;
      if (!l3::DelSvi(bridge, svi.vid)) {
        return fail(std::format("removing SVI {} failed", svi.device));
      }
      applied = true;
    }
    for (const auto &[vid, vlan] : plan->vlans) {
      if (!vlan.address) continue;
      const auto dev = l3::SviName(bridge, vid);
      const auto cur = std::find_if(
          live.begin(), live.end(),
          [vid](const auto &s) { return s.vid == vid; });
      if (cur == live.end()) {
        if (!l3::AddSvi(bridge, vid)) {
          return fail(std::format(
              "creating SVI for VLAN {} failed (is the bridge up?)", vid));
        }
        applied = true;
      }
      if (cur == live.end() || cur->address != *vlan.address) {
        if (!l3::SetSviAddress(dev, *vlan.address)) {
          return fail(std::format("address {} on {} failed", *vlan.address,
                                  dev));
        }
        applied = true;
      }
    }
  }

  // Routing. Forwarding first: installing routes on a box that will not
  // forward is a configuration that looks right and does nothing.
  {
    if (plan->routing_enabled &&
        *plan->routing_enabled != l3::GetForwarding()) {
      if (!l3::SetForwarding(*plan->routing_enabled)) {
        return fail("IP forwarding could not be changed");
      }
      applied = true;
    }
    std::map<std::string, std::string> desired;
    for (const auto &[name, route] : plan->static_routes) {
      desired[route.prefix] = route.via;
    }
    for (const auto &[iface, cfg] : plan->interfaces) {
      // `ip route` prints the all-zero prefix as `default`, so that is
      // the key the live table will be compared against.
      if (cfg.gateway) desired["default"] = *cfg.gateway;
    }
    if (auto it = desired.find("0.0.0.0/0"); it != desired.end()) {
      desired["default"] = it->second;
      desired.erase(it);
    }
    const auto live = l3::GetRoutes();
    for (const auto &r : live) {
      // Only routes WE installed are eligible for removal, identified
      // by our own protocol number rather than by `proto static`.
      // Reconciling away the kernel's connected routes, the default
      // route a DHCP client acquired, or the uplink route the box's
      // network configuration installed would cut it off on the first
      // commit — which it did, on a test VM, before this was `owned`.
      if (!r.owned) continue;
      if (desired.contains(r.prefix)) continue;
      if (!l3::DelRoute(r.prefix)) {
        return fail(std::format("removing route {} failed", r.prefix));
      }
      applied = true;
    }
    for (const auto &[prefix, via] : desired) {
      const auto cur = std::find_if(
          live.begin(), live.end(), [&prefix](const auto &r) {
            return r.prefix == prefix && r.owned;
          });
      if (cur != live.end() && cur->via == via) continue;
      if (!l3::AddRoute(prefix, via)) {
        return fail(std::format("route {} via {} failed (is the next hop "
                                "reachable?)",
                                prefix, via));
      }
      applied = true;
    }
  }

  // DHCP + DNS. One dnsmasq for every VLAN, driven entirely by the
  // generated file. It comes after the SVIs because dnsmasq binds to
  // those interfaces and will not start if they are not there yet.
  {
    const auto cfg =
        BuildDnsmasqConfig(*plan, fabric::S5Topology().bridge);
    const bool wanted = cfg.dns_enabled || !cfg.pools.empty();
    auto rendered = dnsmasq::Render(cfg);
    if (!rendered) {
      // BuildPlan already ran this; reaching here means the box
      // changed under us, which is still not something to half-apply.
      return fail(rendered.error().message);
    }
    if (wanted && !svc::BinaryAvailable("dnsmasq")) {
      // The SetNtpServer lesson: a service that is not installed is a
      // failed commit. An operator who configured DHCP and got a clean
      // commit will not go looking for why nothing gets an address.
      return fail("DHCP/DNS needs dnsmasq, which this box does not have");
    }
    if (!svc::EnsureRunDir()) {
      return fail("cannot create the runtime directory for services");
    }
    bool changed = false;
    if (!svc::WriteGenerated(dnsmasq::ConfigPath(), *rendered, &changed)) {
      return fail("writing the dnsmasq configuration failed");
    }
    svc::SetWanted("dnsmasq", wanted,
                   wanted ? std::format("{} DHCP pool(s), DNS {}",
                                        cfg.pools.size(),
                                        cfg.dns_enabled ? "on" : "off")
                          : "");
    const bool running = svc::Running("dnsmasq");
    if (!wanted) {
      if (running && !svc::Stop("dnsmasq")) {
        return fail("stopping dnsmasq failed");
      }
      if (running) applied = true;
    } else if (!running || changed) {
      // A restart, not a signal: dnsmasq re-reads /etc/hosts on SIGHUP
      // but NOT its configuration file, so a reload here would report
      // success and leave the old pools serving.
      if (!svc::Restart({.name = "dnsmasq", .command = dnsmasq::Command()})) {
        return fail("dnsmasq did not start — check the generated config");
      }
      applied = true;
    }
  }

  // mDNS reflection. Nothing is generated for this one: mdns-repeater
  // takes its whole configuration on the command line, so the argument
  // list IS the artifact, and it is rebuilt from the candidate every
  // time.
  {
    const auto bridge = fabric::S5Topology().bridge;
    std::string ifaces;
    std::size_t count = 0;
    for (const auto &[vid, on] : plan->mdns_reflect) {
      if (!on) continue;
      ifaces += ' ';
      ifaces += l3::SviName(bridge, vid);
      ++count;
    }
    const bool wanted = plan->mdns_enabled && count >= 2;
    const auto command = "mdns-repeater -p " + svc::RunDir() +
                         "/mdns-repeater.pid" + ifaces;
    if (wanted && !svc::BinaryAvailable("mdns-repeater")) {
      return fail("mDNS reflection needs mdns-repeater, which this box "
                  "does not have");
    }
    bool changed = false;
    if (!svc::WriteGenerated(svc::RunDir() + "/mdns-repeater.args",
                             wanted ? command + "\n" : "", &changed)) {
      return fail("writing the mDNS reflection arguments failed");
    }
    svc::SetWanted("mdns-repeater", wanted,
                   wanted ? std::format("reflecting across {} VLAN(s)",
                                        count)
                          : "");
    const bool running = svc::Running("mdns-repeater");
    if (!wanted) {
      if (running && !svc::Stop("mdns-repeater")) {
        return fail("stopping mdns-repeater failed");
      }
      if (running) applied = true;
    } else if (!running || changed) {
      if (!svc::Restart({.name = "mdns-repeater", .command = command})) {
        return fail("mdns-repeater did not start");
      }
      applied = true;
    }
  }

  // Spanning tree. The mode decides whether the rest is even
  // programmable: with it off there is no mstpd to talk to, and the
  // per-port values stay configuration that takes effect the moment it
  // is turned back on.
  {
    const auto bridge = fabric::S5Topology().bridge;
    const auto live_mode = stp::GetMode(bridge);
    const auto want = plan->stp.mode
                          ? stp::ParseMode(*plan->stp.mode)
                          : live_mode;
    const bool touches_stp =
        plan->stp.mode || plan->stp.priority || plan->stp.hello ||
        plan->stp.max_age || plan->stp.forward_delay ||
        !plan->port_stp.empty();
    if (touches_stp && want != stp::Mode::Off && !stp::Available()) {
      // Fail the commit rather than leave the operator believing a loop
      // is guarded against. This is the SetNtpServer lesson: a service
      // that is not on the box is a failed apply, never a silent
      // success.
      return fail(
          "spanning tree needs mstpd + mstpctl, which this box does "
          "not have");
    }
    svc::SetWanted("mstpd", want != stp::Mode::Off,
                   want == stp::Mode::Off
                       ? ""
                       : std::format("{} loop protection on {}",
                                     stp::ModeToken(want), bridge));
    if (want != live_mode) {
      if (!stp::SetMode(bridge, want)) {
        return fail(std::format("spanning tree mode {} failed",
                                plan->stp.mode.value_or("off")));
      }
      applied = true;
    }
    if (want != stp::Mode::Off) {
      const auto live = stp::GetBridgeState(bridge);
      if (plan->stp.priority && *plan->stp.priority != live.priority) {
        if (!stp::SetBridgePriority(bridge, *plan->stp.priority)) {
          return fail("spanning tree bridge priority failed");
        }
        applied = true;
      }
      if (plan->stp.hello &&
          std::to_string(*plan->stp.hello) != live.hello) {
        if (!stp::SetHello(bridge, *plan->stp.hello)) {
          return fail("spanning tree hello time failed");
        }
        applied = true;
      }
      if (plan->stp.max_age &&
          std::to_string(*plan->stp.max_age) != live.admin_max_age) {
        if (!stp::SetMaxAge(bridge, *plan->stp.max_age)) {
          return fail("spanning tree max age failed");
        }
        applied = true;
      }
      if (plan->stp.forward_delay &&
          std::to_string(*plan->stp.forward_delay) !=
              live.admin_forward_delay) {
        if (!stp::SetForwardDelay(bridge, *plan->stp.forward_delay)) {
          return fail("spanning tree forward delay failed");
        }
        applied = true;
      }
      const auto live_ports = stp::GetPortStates(bridge);
      const auto find_port = [&live_ports](const std::string &name) {
        return std::find_if(
            live_ports.begin(), live_ports.end(),
            [&name](const auto &p) { return p.port == name; });
      };
      for (const auto &[port, want_port] : plan->port_stp) {
        const auto cur = find_port(port);
        const bool known = cur != live_ports.end();
        if (want_port.cost &&
            (!known || std::to_string(*want_port.cost) != cur->admin_cost)) {
          if (!stp::SetPortCost(bridge, port, *want_port.cost)) {
            return fail(std::format("spanning tree cost on {} failed", port));
          }
          applied = true;
        }
        if (want_port.priority &&
            (!known || *want_port.priority != cur->priority)) {
          if (!stp::SetPortPriority(bridge, port, *want_port.priority)) {
            return fail(
                std::format("spanning tree priority on {} failed", port));
          }
          applied = true;
        }
        if (want_port.edge && (!known || *want_port.edge != cur->edge)) {
          if (!stp::SetPortEdge(bridge, port, *want_port.edge)) {
            return fail(std::format("spanning tree edge on {} failed", port));
          }
          applied = true;
        }
        if (want_port.bpdu_guard &&
            (!known || *want_port.bpdu_guard != cur->bpdu_guard)) {
          if (!stp::SetPortBpduGuard(bridge, port, *want_port.bpdu_guard)) {
            return fail(
                std::format("spanning tree bpdu guard on {} failed", port));
          }
          applied = true;
        }
      }
    }
  }

  // LLDP. The daemon is this binary in another mode; the generated
  // config file is the whole interface between a commit and it.
  {
    lldp::Config cfg;
    cfg.enabled = plan->lldp.enabled.value_or(true);
    cfg.tx_interval = plan->lldp.tx_interval.value_or(30);
    cfg.system_name = plan->hostname.value_or(Trim(sys::GetHostname()));
    cfg.system_description = "einheit S5 5-port managed PoE switch";
    for (const auto &port : dsa::DiscoverPorts()) {
      // Per-port default is on: a switch you cannot see in a neighbour
      // table is a switch somebody will mis-cable.
      const auto it = plan->lldp.ports.find(port);
      if (it != plan->lldp.ports.end() && !it->second) continue;
      cfg.ports.push_back(port);
    }
    if (!svc::EnsureRunDir()) {
      return fail("cannot create the runtime directory for services");
    }
    bool changed = false;
    if (!svc::WriteGenerated(lldp::ConfigPath(), lldp::RenderConfig(cfg),
                             &changed)) {
      return fail("writing the LLDP configuration failed");
    }
    svc::SetWanted("lldp", cfg.enabled,
                   cfg.enabled
                       ? std::format("advertising on {} port(s) every {}s",
                                     cfg.ports.size(), cfg.tx_interval)
                       : "");
    if (!cfg.enabled) {
      if (!svc::Stop("lldp")) return fail("stopping the LLDP daemon failed");
    } else if (!svc::Running("lldp")) {
      const auto self = svc::SelfExe();
      if (self.empty() || !SafeToken(self)) {
        return fail("cannot locate this binary to start the LLDP daemon");
      }
      if (!svc::Start({.name = "lldp",
                       .command = std::format("{} --lldp-daemon", self)})) {
        return fail("the LLDP daemon did not start");
      }
      applied = true;
    } else if (changed) {
      // Running already: a signal, not a restart. Bouncing it would
      // drop every learned neighbour for a commit that only renamed
      // the box.
      if (!svc::Reload("lldp")) {
        return fail("reloading the LLDP daemon failed");
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
    running["ntp.serve"] = sys::GetNtpServing() ? "true" : "false";
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
    const auto mode = stp::GetMode(topo.bridge);
    running["stp.mode"] =
        mode == stp::Mode::Off ? "off" : stp::ModeToken(mode);
    if (mode != stp::Mode::Off) {
      const auto st = stp::GetBridgeState(topo.bridge);
      running["stp.priority"] = std::to_string(st.priority);
      if (!st.hello.empty()) running["stp.hello"] = st.hello;
      if (!st.admin_max_age.empty()) {
        running["stp.max_age"] = st.admin_max_age;
      }
      if (!st.admin_forward_delay.empty()) {
        running["stp.forward_delay"] = st.admin_forward_delay;
      }
      for (const auto &p : stp::GetPortStates(topo.bridge)) {
        // Only the switch ports the fabric owns. mstpd reports every
        // member of the bridge, and anything else that ends up in it —
        // a veth, a tap, a test harness's namespace link — is not
        // configuration. Reading those back made them part of the
        // candidate, which the next commit then rejected as "spanning
        // tree applies to bridged ports only": the box could not
        // commit ANYTHING until the foreign interface went away.
        if (std::find(topo.members.begin(), topo.members.end(), p.port) ==
            topo.members.end()) {
          continue;
        }
        // The CONFIGURED cost, not the effective one: mstpd derives a
        // cost from link speed when the admin value is 0, and reading
        // that back would silently turn `auto` into a hard number the
        // operator never typed (the GetPortParams lesson).
        if (!p.admin_cost.empty()) {
          running[std::format("ports.{}.stp.cost", p.port)] = p.admin_cost;
        }
        running[std::format("ports.{}.stp.priority", p.port)] =
            std::to_string(p.priority);
        running[std::format("ports.{}.stp.edge", p.port)] =
            p.edge ? "true" : "false";
        running[std::format("ports.{}.stp.bpdu_guard", p.port)] =
            p.bpdu_guard ? "true" : "false";
      }
    }
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
  // SVIs. Names are pure configuration with nothing on the box to read
  // them back from, so they survive by living in the commit — which is
  // exactly what makes `show vlans` still say "guest" after a reboot.
  for (const auto &svi : l3::GetSvis(topo.bridge)) {
    if (svi.address.empty()) continue;
    running[std::format("vlans.{}.address", svi.vid)] = svi.address;
  }
  running["routing.enabled"] = l3::GetForwarding() ? "true" : "false";
  // LLDP read-back comes from the generated config, which IS the
  // applied intent. Whether the daemon is alive is operational state
  // and belongs to `show system services`, not to running config —
  // reporting a crashed daemon as `lldp.enabled false` would let a
  // reconcile quietly turn the feature off.
  if (const auto lldp_cfg = lldp::ReadConfig()) {
    running["lldp.enabled"] = lldp_cfg->enabled ? "true" : "false";
    running["lldp.tx_interval"] = std::to_string(lldp_cfg->tx_interval);
    for (const auto &name : ports) {
      const bool on = std::find(lldp_cfg->ports.begin(),
                                lldp_cfg->ports.end(),
                                name) != lldp_cfg->ports.end();
      running[std::format("lldp.port.{}.enabled", name)] =
          on ? "true" : "false";
    }
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

auto S5Backend::Warnings(const Candidate &candidate) const
    -> std::vector<std::string> {
  std::vector<std::string> out;
  const auto path = sys::GetManagementPath();
  // A console operator cannot be locked out by a commit, and a box
  // that cannot work out its own management path must not invent a
  // warning about it.
  if (path.peer.empty() || path.device.empty()) return out;

  // Compare against the box, not against running config: the question
  // is what this commit changes about the path that is CARRYING it.
  const auto changes = [&candidate](const std::string &key,
                                    const std::string &live) {
    const auto it = candidate.values.find(key);
    if (it == candidate.values.end()) return !live.empty();
    return it->second != live;
  };

  const auto bridge = fabric::S5Topology().bridge;
  const std::string svi_prefix = bridge + ".";
  if (path.device.starts_with(svi_prefix)) {
    // The session rides an SVI, so the VLAN's address is the path.
    const auto vid = path.device.substr(svi_prefix.size());
    std::string live;
    for (const auto &svi : l3::GetSvis(bridge)) {
      if (svi.device == path.device) live = svi.address;
    }
    if (changes(std::format("vlans.{}.address", vid), live)) {
      out.push_back(std::format(
          "this session reaches the switch on VLAN {} ({}), and this "
          "commit changes vlans.{}.address — consider `commit confirmed "
          "5`",
          vid, path.address, vid));
    }
  } else {
    // A plain interface: its own address, or DHCP taking it over.
    std::string live;
    for (const auto &iface : sys::GetInterfaces()) {
      if (iface.name == path.device && iface.address.find(':') ==
                                           std::string::npos) {
        live = iface.address;
      }
    }
    const auto addr_key = std::format("interfaces.{}.address", path.device);
    const auto dhcp_key = std::format("interfaces.{}.dhcp", path.device);
    const auto dhcp = candidate.values.find(dhcp_key);
    if (changes(addr_key, live) ||
        (dhcp != candidate.values.end() && dhcp->second == "true")) {
      out.push_back(std::format(
          "this session reaches the switch on {} ({}), and this commit "
          "changes its addressing — consider `commit confirmed 5`",
          path.device, path.address));
    }
  }

  // A routed session also depends on the route back. Changing the
  // default route while the operator is on the far side of it is the
  // other half of the same mistake.
  if (path.routed) {
    std::string live_gateway;
    for (const auto &r : l3::GetRoutes()) {
      if (r.prefix == "default") live_gateway = r.via;
    }
    bool touches_default = false;
    for (const auto &[key, value] : candidate.values) {
      if (key.ends_with(".gateway") && value != live_gateway) {
        touches_default = true;
      }
      if (key.ends_with(".prefix") && value == "0.0.0.0/0") {
        touches_default = true;
      }
    }
    if (touches_default) {
      out.push_back(std::format(
          "this session is routed via {} and this commit changes the "
          "default route — consider `commit confirmed 5`",
          live_gateway.empty() ? "an unknown gateway" : live_gateway));
    }
  }

  // Shutting the port the session came in on, or taking it out of the
  // VLAN it arrived on, is the classic version of this mistake.
  const auto ingress = IngressPortFor(path.peer);
  if (!ingress.empty()) {
    const auto key = std::format("ports.{}.enabled", ingress);
    const auto it = candidate.values.find(key);
    if (it != candidate.values.end() && it->second == "false") {
      out.push_back(std::format(
          "this session arrived on {}, and this commit shuts it — there "
          "is no way back in through a disabled port",
          ingress));
    }
  }
  return out;
}

auto S5Backend::EnsureFabric()
    -> std::expected<void, Error<fabric::FabricError>> {
  std::lock_guard<std::mutex> lk(mu_);
  return fabric::Ensure(fabric::S5Topology());
}

}  // namespace einheit::s5
