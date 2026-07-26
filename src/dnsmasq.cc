/// @file dnsmasq.cc
/// @brief Generated dnsmasq configuration, and the lease database.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/dnsmasq.h"

#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "einheit/s5/svc.h"
#include "einheit/s5/util.h"

namespace einheit::s5::dnsmasq {
namespace {

using cli::Error;

auto Fail(std::string msg) -> std::unexpected<Error<GenError>> {
  return std::unexpected(Error<GenError>{GenError::InvalidValue,
                                         std::move(msg)});
}

/// Strict dotted-quad. Not a convenience check: this value is written
/// into a line-oriented configuration file with no quoting, so
/// anything that is not exactly four decimal octets is refused rather
/// than escaped.
auto ValidIpv4(const std::string &s) -> bool {
  int octets = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t digits = 0;
    int value = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
      value = value * 10 + (s[i] - '0');
      ++digits;
      ++i;
    }
    if (digits == 0 || digits > 3 || value > 255) return false;
    ++octets;
    if (i == s.size()) break;
    if (s[i] != '.') return false;
    ++i;
    if (i == s.size()) return false;
  }
  return octets == 4;
}

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

/// A DNS label set: letters, digits, hyphen, dots between labels. No
/// leading/trailing dot, no empty label, nothing else — a domain is
/// the one free-text-looking field here and the obvious place to try
/// to smuggle a newline into the file.
auto ValidDomain(const std::string &s) -> bool {
  if (s.empty() || s.size() > 253) return false;
  if (s.front() == '.' || s.back() == '.' || s.front() == '-') return false;
  std::size_t label = 0;
  for (char c : s) {
    if (c == '.') {
      if (label == 0) return false;
      label = 0;
      continue;
    }
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) == 0 && c != '-') return false;
    if (++label > 63) return false;
  }
  return label > 0;
}

/// Netdev names reach the file too (`interface=br0.10`).
auto ValidIface(const std::string &s) -> bool {
  if (s.empty() || s.size() > 15) return false;
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) == 0 && c != '.' && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

auto Ipv4ToUint(const std::string &s) -> std::uint32_t {
  std::uint32_t out = 0;
  std::uint32_t octet = 0;
  for (char c : s) {
    if (c == '.') {
      out = (out << 8) | octet;
      octet = 0;
      continue;
    }
    octet = octet * 10 + static_cast<std::uint32_t>(c - '0');
  }
  return (out << 8) | octet;
}

}  // namespace

auto ConfigPath() -> std::string {
  return svc::RunDir() + "/dnsmasq.conf";
}

auto LeasesPath() -> std::string {
  return svc::RunDir() + "/dnsmasq.leases";
}

auto Command() -> std::string {
  // --keep-in-foreground would fight svc::Start's fork-and-verify;
  // dnsmasq's own daemonisation is what the pidof probe checks for.
  return std::format("dnsmasq --conf-file={}", ConfigPath());
}

auto NetmaskOf(const std::string &address) -> std::string {
  const auto slash = address.find('/');
  if (slash == std::string::npos) return "";
  const int bits = std::atoi(address.substr(slash + 1).c_str());
  if (bits < 0 || bits > 32) return "";
  const std::uint32_t mask =
      bits == 0 ? 0 : (0xffffffffU << (32 - bits)) & 0xffffffffU;
  return std::format("{}.{}.{}.{}", (mask >> 24) & 0xff,
                     (mask >> 16) & 0xff, (mask >> 8) & 0xff, mask & 0xff);
}

auto InSubnet(const std::string &address, const std::string &ip) -> bool {
  const auto slash = address.find('/');
  if (slash == std::string::npos) return false;
  const auto network = address.substr(0, slash);
  if (!ValidIpv4(network) || !ValidIpv4(ip)) return false;
  const int bits = std::atoi(address.substr(slash + 1).c_str());
  if (bits < 0 || bits > 32) return false;
  if (bits == 0) return true;
  const std::uint32_t mask = (0xffffffffU << (32 - bits)) & 0xffffffffU;
  return (Ipv4ToUint(network) & mask) == (Ipv4ToUint(ip) & mask);
}

auto Render(const Config &cfg)
    -> std::expected<std::string, Error<GenError>> {
  std::string out =
      "# einheit s5 — GENERATED from the committed configuration on\n"
      "# every commit and every boot. Edits here are lost at the next\n"
      "# commit; configure DHCP and DNS through the CLI.\n";

  // dnsmasq answers on the interfaces it is told about and nowhere
  // else. Without bind-interfaces it opens a wildcard socket and would
  // answer DHCP on the uplink — a switch that hands out addresses to
  // somebody else's network.
  out += "bind-interfaces\n";
  out += "except-interface=lo\n";
  out += std::format("dhcp-leasefile={}\n", LeasesPath());

  if (cfg.dns_enabled) {
    // Never fall back to /etc/resolv.conf: the box's own resolver is
    // configured separately, and inheriting it here is how a
    // forwarding loop starts.
    out += "no-resolv\n";
    out += "domain-needed\n";
    out += "bogus-priv\n";
    for (const auto &f : cfg.forwarders) {
      if (!ValidIpv4(f)) {
        return Fail(std::format("dns forwarder '{}' is not an IPv4 address",
                                f));
      }
      out += std::format("server={}\n", f);
    }
    if (!cfg.local_domain.empty()) {
      if (!ValidDomain(cfg.local_domain)) {
        return Fail(std::format("dns.local_domain '{}' is not a domain name",
                                cfg.local_domain));
      }
      out += std::format("domain={}\n", cfg.local_domain);
      // Answer for the local domain ourselves rather than forwarding
      // names that upstream cannot know about.
      out += std::format("local=/{}/\n", cfg.local_domain);
      out += "expand-hosts\n";
    }
  } else {
    // port=0 turns the DNS server off while leaving DHCP running.
    out += "port=0\n";
  }

  for (const auto &pool : cfg.pools) {
    if (!ValidIface(pool.interface)) {
      return Fail(std::format("'{}' is not an interface name",
                              pool.interface));
    }
    if (!ValidIpv4(pool.range_start)) {
      return Fail(std::format("vlans.{}.dhcp.range_start '{}' is not an "
                              "IPv4 address",
                              pool.vid, pool.range_start));
    }
    if (!ValidIpv4(pool.range_end)) {
      return Fail(std::format("vlans.{}.dhcp.range_end '{}' is not an IPv4 "
                              "address",
                              pool.vid, pool.range_end));
    }
    if (!ValidIpv4(pool.netmask)) {
      return Fail(std::format("vlans.{}: no usable netmask; the VLAN needs "
                              "an address before it can serve DHCP",
                              pool.vid));
    }
    if (Ipv4ToUint(pool.range_start) > Ipv4ToUint(pool.range_end)) {
      return Fail(std::format("vlans.{}.dhcp: range_start is above "
                              "range_end",
                              pool.vid));
    }
    if (pool.lease_minutes < 2) {
      return Fail(std::format("vlans.{}.dhcp.lease_time is too short",
                              pool.vid));
    }
    const auto tag = std::format("vlan{}", pool.vid);
    out += std::format("interface={}\n", pool.interface);
    out += std::format("dhcp-range=set:{},{},{},{},{}m\n", tag,
                       pool.range_start, pool.range_end, pool.netmask,
                       pool.lease_minutes);
    if (!pool.gateway.empty()) {
      if (!ValidIpv4(pool.gateway)) {
        return Fail(std::format("vlans.{}.dhcp.gateway '{}' is not an IPv4 "
                                "address",
                                pool.vid, pool.gateway));
      }
      out += std::format("dhcp-option=tag:{},3,{}\n", tag, pool.gateway);
    }
    if (!pool.dns.empty()) {
      if (!ValidIpv4(pool.dns)) {
        return Fail(std::format("vlans.{}.dhcp.dns '{}' is not an IPv4 "
                                "address",
                                pool.vid, pool.dns));
      }
      out += std::format("dhcp-option=tag:{},6,{}\n", tag, pool.dns);
    }
    for (const auto &r : pool.reservations) {
      if (!ValidMac(r.mac)) {
        return Fail(std::format("vlans.{}.dhcp.static.{}: not a MAC address",
                                pool.vid, r.mac));
      }
      if (!ValidIpv4(r.ip)) {
        return Fail(std::format("vlans.{}.dhcp.static.{}.ip '{}' is not an "
                                "IPv4 address",
                                pool.vid, r.mac, r.ip));
      }
      out += std::format("dhcp-host={},{}\n", r.mac, r.ip);
    }
  }
  return out;
}

auto ParseLeases(const std::string &text) -> std::vector<Lease> {
  std::vector<Lease> out;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    // <expiry> <mac> <ip> <hostname> <client-id>
    std::istringstream ls(line);
    Lease l;
    std::string expiry;
    if (!(ls >> expiry >> l.mac >> l.ip)) continue;
    if (!(ls >> l.hostname)) l.hostname = "*";
    l.expires = std::strtoll(expiry.c_str(), nullptr, 10);
    out.push_back(l);
  }
  return out;
}

auto ReadLeases() -> std::vector<Lease> {
  std::ifstream f(util::FsPath(LeasesPath()));
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ParseLeases(ss.str());
}

auto RemoveLease(const std::string &ip_or_mac, bool *removed) -> bool {
  if (removed != nullptr) *removed = false;
  std::ifstream f(util::FsPath(LeasesPath()));
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  const auto leases = ParseLeases(ss.str());
  std::string kept;
  for (const auto &l : leases) {
    if (l.ip == ip_or_mac || l.mac == ip_or_mac) {
      if (removed != nullptr) *removed = true;
      continue;
    }
    kept += std::format("{} {} {} {} *\n", l.expires, l.mac, l.ip,
                        l.hostname.empty() ? "*" : l.hostname);
  }
  return util::WriteFile(LeasesPath(), kept);
}

}  // namespace einheit::s5::dnsmasq
