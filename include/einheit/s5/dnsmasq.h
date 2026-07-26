/// @file dnsmasq.h
/// @brief DHCP server and DNS forwarder — the generated dnsmasq config.
///
/// One dnsmasq instance serves every VLAN, driven by a file this module
/// renders from the committed configuration. That file is an **apply
/// artifact**: it is rewritten on every commit and on every boot, it
/// says so in its first line, and anything an operator edits into it is
/// gone at the next commit. The CLI is the whole product surface, so
/// there is no supported way to configure dnsmasq except through the
/// schema.
///
/// Render() is a pure function returning either the file or an error,
/// which is what makes the hostile-input rule testable: every value
/// that reaches the file is validated here, and a value that fails
/// validation fails the COMMIT rather than being escaped, quoted, or
/// silently dropped. dnsmasq's format is line-oriented with no quoting
/// at all, so a newline inside any value would be a new directive —
/// there is no safe escaping, only rejection.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_DNSMASQ_H_
#define EINHEIT_S5_DNSMASQ_H_

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "einheit/cli/error.h"

namespace einheit::s5::dnsmasq {

/// A fixed address for one client.
struct Reservation {
  std::string mac;
  std::string ip;
};

/// One VLAN's DHCP pool.
struct Pool {
  int vid = 0;
  /// SVI the pool is served on; dnsmasq matches the request's ingress
  /// interface to this, which is what keeps VLANs apart.
  std::string interface;
  std::string range_start;
  std::string range_end;
  /// Dotted-quad netmask, derived from the SVI's prefix length.
  std::string netmask;
  int lease_minutes = 720;
  /// Router option; empty means "the switch's own address in this
  /// VLAN", which dnsmasq supplies by default.
  std::string gateway;
  /// DNS option; empty means "the switch itself".
  std::string dns;
  std::vector<Reservation> reservations;
};

/// Everything the generated file has to say.
struct Config {
  /// Whether to answer DNS at all. Off still serves DHCP.
  bool dns_enabled = true;
  /// Domain appended to unqualified names, and served for local ones.
  std::string local_domain;
  /// Upstream resolvers (the existing dns.primary / dns.secondary).
  std::vector<std::string> forwarders;
  std::vector<Pool> pools;
};

/// Why a configuration could not be rendered.
enum class GenError {
  /// A value would not survive being written into a line-oriented
  /// configuration file, or is not the kind of value it claims to be.
  InvalidValue,
};

/// Render the dnsmasq configuration file.
/// @param cfg What the committed configuration asks for.
/// @returns The file contents, or the first value that was refused.
auto Render(const Config &cfg)
    -> std::expected<std::string, cli::Error<GenError>>;

/// Paths of the generated config, the lease database and the pid file.
auto ConfigPath() -> std::string;
auto LeasesPath() -> std::string;

/// The command line that starts dnsmasq against the generated config.
auto Command() -> std::string;

/// Convert a CIDR prefix length to a dotted-quad netmask.
/// @param address An address with prefix, e.g. "10.10.0.1/24".
/// @returns The netmask, or empty when the input is not a CIDR.
auto NetmaskOf(const std::string &address) -> std::string;

/// Whether `ip` falls inside the network `address` describes.
/// @param address Network address with prefix, e.g. "10.10.0.1/24".
/// @param ip A bare address.
auto InSubnet(const std::string &address, const std::string &ip) -> bool;

/// One row of the lease database.
struct Lease {
  /// Unix time the lease runs out.
  std::int64_t expires = 0;
  std::string mac;
  std::string ip;
  std::string hostname;
};

/// Parse dnsmasq's lease file format.
auto ParseLeases(const std::string &text) -> std::vector<Lease>;

/// Read the lease database off the box.
auto ReadLeases() -> std::vector<Lease>;

/// Drop one lease, named by address or by MAC.
///
/// dnsmasq only reads its lease database at startup, so releasing a
/// lease means editing the file and bouncing the daemon. That is a
/// real (brief) interruption and the honest implementation: the
/// alternative, editing the file and leaving dnsmasq holding the old
/// lease in memory, would report success and change nothing.
/// @param ip_or_mac Address or MAC of the lease to drop.
/// @param removed Set to whether a matching lease was found.
/// @returns Whether the rewrite succeeded.
auto RemoveLease(const std::string &ip_or_mac, bool *removed) -> bool;

}  // namespace einheit::s5::dnsmasq

#endif  // EINHEIT_S5_DNSMASQ_H_
