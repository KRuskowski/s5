/// @file lldp.h
/// @brief LLDP (802.1AB) — our own, rather than lldpd.
///
/// DECISION (WP1.3 spike, recorded here because the reasoning outlives
/// the choice): s5 implements LLDP itself instead of packaging lldpd.
///
///  - Size. lldpd is ~1.5 MB plus libevent and its own CLI; the switch
///    image has 128 MB of NAND to hold everything. What follows is
///    under a thousand lines and links into the binary that is already
///    there.
///  - Surface. We need six TLVs out and a neighbour table in. lldpd
///    brings CDP/EDP/FDP/SONMP, SNMP subagent, DBus, and a
///    configuration language, none of which any operator of this
///    switch will use, all of which is attack surface and packages to
///    track.
///  - CLI completeness. Every knob has to be reachable from `set` and
///    every state from `show`. Wrapping lldpd means generating its
///    config AND parsing `lldpcli show neighbors -f json` — two
///    couplings to somebody else's formats. Owning the wire format
///    means the schema IS the configuration.
///  - Testability. TLV encode/decode is a pure function, so the
///    golden tests are exact byte comparisons rather than a mocked
///    subprocess.
///
/// The cost is real and worth naming: we own an on-wire parser that
/// reads bytes from an untrusted network. Hence ParseLldpdu takes a
/// length, never trusts a TLV's own length field, and sanitises every
/// string it hands back — a neighbour must not be able to put a tab or
/// an escape sequence into the operator's terminal.
///
/// There is no s5 daemon, so the transmit/receive loop is a mode of
/// this binary (`einheit_s5 --lldp-daemon`), started at boot and
/// reconfigured on commit through the same generated-file-plus-signal
/// path dnsmasq uses.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_S5_LLDP_H_
#define EINHEIT_S5_LLDP_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace einheit::s5::lldp {

/// The daemon's whole configuration — an apply artifact written by
/// S5Backend, never edited by an operator.
struct Config {
  bool enabled = true;
  /// Seconds between advertisements.
  int tx_interval = 30;
  /// Advertised system name (the switch hostname).
  std::string system_name;
  /// Advertised system description.
  std::string system_description;
  /// Advertised management address, dotted quad; empty to omit the TLV.
  std::string management_address;
  /// Ports to run on, in configuration order.
  std::vector<std::string> ports;
};

/// One neighbour, as `show neighbors` prints it.
struct Neighbor {
  /// Our port the neighbour was heard on.
  std::string local_port;
  std::string chassis_id;
  std::string port_id;
  std::string port_description;
  std::string system_name;
  std::string system_description;
  std::string management_address;
  /// Rendered capability letters, e.g. "Bridge Router".
  std::string capabilities;
  /// Advertised time-to-live in seconds.
  int ttl = 0;
  /// Unix time the last advertisement arrived.
  std::int64_t last_seen = 0;
};

/// Build one LLDPDU payload (no Ethernet header): the mandatory
/// chassis/port/TTL TLVs, the three optional description TLVs the
/// standard's basic set defines, capabilities, a management address
/// when one is configured, and the end marker.
/// @param cfg Advertised identity.
/// @param port Netdev name, used as the port id and description.
/// @param chassis_mac Bridge MAC, as the chassis identifier.
/// @param ttl Seconds a receiver should keep the entry.
/// @returns The LLDPDU payload bytes.
auto BuildLldpdu(const Config &cfg, const std::string &port,
                 const std::string &chassis_mac, int ttl)
    -> std::vector<std::uint8_t>;

/// Decode an LLDPDU payload received on `local_port`.
///
/// Every field here came off an untrusted network. The parser trusts
/// only `len`: a TLV claiming to be longer than the remaining buffer
/// ends the parse rather than reading past it, and every string is
/// sanitised before it can reach a terminal or a table.
/// @param data LLDPDU payload (after the Ethernet header).
/// @param len Bytes actually available.
/// @param local_port Port the frame arrived on.
/// @returns The neighbour, or nullopt when the frame is not a usable
///   LLDPDU (no chassis id, no port id, or malformed).
auto ParseLldpdu(const std::uint8_t *data, std::size_t len,
                 const std::string &local_port) -> std::optional<Neighbor>;

/// Render a string that came off the wire so it is safe to print and
/// safe to store in the tab-separated neighbour table. Control
/// characters (including the tabs and newlines that would forge a row)
/// become '.', and the result is capped.
auto SanitizeWireString(const std::string &raw) -> std::string;

/// Path of the generated daemon configuration.
auto ConfigPath() -> std::string;

/// Path of the neighbour table the daemon maintains.
auto NeighborsPath() -> std::string;

/// Serialise / parse the daemon configuration file.
auto RenderConfig(const Config &cfg) -> std::string;
auto ParseConfig(const std::string &text) -> Config;

/// The generated configuration as it exists on the box, or nullopt
/// when nothing has been applied yet. This is what config read-back
/// uses: the file is the applied intent, whereas whether the daemon is
/// alive is operational state and belongs in `show system services`.
auto ReadConfig() -> std::optional<Config>;

/// Serialise / parse the neighbour table file.
auto RenderNeighbors(const std::vector<Neighbor> &neighbors) -> std::string;
auto ParseNeighbors(const std::string &text) -> std::vector<Neighbor>;

/// Read the neighbour table off the box, dropping entries whose TTL
/// has run out. A neighbour that stopped advertising is gone, and a
/// table that still lists it is worse than an empty one.
/// @param now Unix time to age against.
auto ReadNeighbors(std::int64_t now) -> std::vector<Neighbor>;

/// Run the transmit/receive loop until killed. This is what
/// `einheit_s5 --lldp-daemon` becomes.
/// @returns Process exit status.
auto RunDaemon() -> int;

}  // namespace einheit::s5::lldp

#endif  // EINHEIT_S5_LLDP_H_
