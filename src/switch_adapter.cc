/// @file switch_adapter.cc
/// @brief KSZ9477 switch adapter — declarative specs + rendering.
///
/// Display-only by construction (gap #4): this file never touches
/// hardware. Reads and operational actions execute in
/// service::HandleProduct on the daemon side of the in-proc
/// transport; config mutations go through the confd runtime into
/// S5Backend::Apply. RenderResponse only decodes the returned data
/// blob into semantic tables.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/switch_adapter.h"

#include <format>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "einheit/cli/command_tree.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/render/config_tree.h"
#include "einheit/cli/render/table.h"

namespace einheit::s5 {
namespace {

using cli::CommandSpec;
using cli::ProductAdapter;
using cli::ProductMetadata;
using cli::protocol::Response;
using cli::protocol::ResponseStatus;
using cli::render::Renderer;
using cli::schema::Schema;

/// Split the service's line-oriented data blob into rows of
/// tab-separated fields.
auto DecodeRows(const Response &r)
    -> std::vector<std::vector<std::string>> {
  std::vector<std::vector<std::string>> rows;
  const std::string body(r.data.begin(), r.data.end());
  std::istringstream iss(body);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    std::vector<std::string> fields;
    std::string field;
    std::istringstream ls(line);
    while (std::getline(ls, field, '\t')) {
      fields.push_back(field);
    }
    rows.push_back(std::move(fields));
  }
  return rows;
}

auto Field(const std::vector<std::string> &row, std::size_t idx)
    -> std::string {
  return idx < row.size() ? row[idx] : "";
}

class SwitchAdapter : public ProductAdapter {
 public:
  explicit SwitchAdapter(cli::schema::SchemaHandle schema)
      : schema_(std::move(schema)) {}

  auto Metadata() const -> ProductMetadata override {
    return {
        .id = "s5",
        .display_name = "einheit S5",
        .version = "0.2.1",
        .banner = "einheit S5 — 5-port managed gigabit PoE switch",
        .prompt = "S5",
    };
  }

  auto GetSchema() const -> const Schema & override {
    return schema_.Get();
  }

  auto ControlSocketPath() const -> std::string override {
    return "";
  }

  auto EventSocketPath() const -> std::string override {
    return "";
  }

  auto Commands() const
      -> std::vector<CommandSpec> override {
    // Reads and operational actions only. Config mutations
    // (hostname, interface addressing, DNS, NTP, port admin
    // state, PoE enable/limits) live in the schema and go through
    // the framework's configure / set / commit lifecycle.
    return {
        CommandSpec{
            .path = "show interfaces",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_interfaces",
            .help = "Show port status",
        },
        CommandSpec{
            .path = "show counters",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_counters",
            .help = "Show port counters",
        },
        CommandSpec{
            .path = "show interfaces detail",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_interfaces_detail",
            .help = "Show configured vs negotiated link parameters "
                    "and counters",
        },
        CommandSpec{
            .path = "clear counters",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "clear_counters",
            .help = "Zero the counter baseline (all ports, or one)",
        },
        CommandSpec{
            .path = "show mac-table",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_mac_table",
            .help = "Show the MAC table (static and learned)",
        },
        CommandSpec{
            .path = "clear mac-table",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "clear_mac_table",
            .help = "Flush learned MACs; static entries are config and "
                    "stay",
        },
        CommandSpec{
            .path = "show igmp-snooping",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_igmp_snooping",
            .help = "Show multicast snooping state and groups",
        },
        CommandSpec{
            .path = "show neighbors",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_neighbors",
            .help = "Show LLDP neighbours heard on each port",
        },
        CommandSpec{
            .path = "show system services",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_system_services",
            .help = "Show the background services the switch runs, and "
                    "whether they are up",
        },
        CommandSpec{
            .path = "show spanning-tree",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_spanning_tree",
            .help = "Show the spanning tree: root, and each port's role "
                    "and state",
        },
        CommandSpec{
            .path = "show spanning-tree statistics",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_spanning_tree_statistics",
            .help = "Show per-port BPDU counters and state transitions",
        },
        CommandSpec{
            .path = "clear spanning-tree statistics",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "clear_spanning_tree_statistics",
            .help = "Zero the spanning-tree counter baseline",
        },
        CommandSpec{
            .path = "clear spanning-tree bpdu-guard",
            .args = {{.name = "port", .help = "Port name",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "clear_bpdu_guard",
            .help = "Return a port the BPDU guard blocked to service",
        },
        CommandSpec{
            .path = "show vlans",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_vlans",
            .help = "Show VLANs: name, switch address, and member ports",
        },
        CommandSpec{
            .path = "show dhcp leases",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_dhcp_leases",
            .help = "Show which clients hold which addresses",
        },
        CommandSpec{
            .path = "show dhcp server",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_dhcp_server",
            .help = "Show the DHCP pools being served, and their usage",
        },
        CommandSpec{
            .path = "clear dhcp lease",
            .args = {{.name = "client", .help = "Address or MAC",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "clear_dhcp_lease",
            .help = "Return one client's address to the pool",
        },
        CommandSpec{
            .path = "show route",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_route",
            .help = "Show the routing table and whether forwarding is on",
        },
        // Served by the confd runtime, not by service::HandleProduct:
        // commits, the open session, the edit-lock holder and any
        // pending commit-confirmed countdown. The framework's config
        // globals deliberately leave this verb to the adapter, so
        // without it that state has no CLI surface at all.
        CommandSpec{
            .path = "show status",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_status",
            .help = "Show config-plane status: commits, session, edit "
                    "lock, pending commit-confirmed",
        },
        // Boot APPLY needs no verb — `rollback to <id>` is the
        // operator equivalent — but the boot OUTCOME is state, and
        // state gets a show verb. Served by the confd runtime from the
        // report persisted beside the store, so an interactive session
        // that never ran a boot apply can still answer.
        CommandSpec{
            .path = "show system boot",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_system_boot",
            .help = "Show what the last boot-restore did",
        },
        CommandSpec{
            .path = "show fabric",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_fabric",
            .help = "Show the switch fabric (bridge, members, conduit)",
        },
        CommandSpec{
            .path = "show version",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_version",
            .help = "Show switch information",
        },
        CommandSpec{
            .path = "show system",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_system",
            .help = "Show system info",
        },
        CommandSpec{
            .path = "show ip",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_ip",
            .help = "Show IP addresses",
        },
        CommandSpec{
            .path = "show dns",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_dns",
            .help = "Show DNS servers",
        },
        CommandSpec{
            .path = "show ntp",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_ntp",
            .help = "Show NTP status",
        },
        CommandSpec{
            .path = "show log",
            .args = {{.name = "lines", .help = "Number of lines",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_log",
            .help = "Show syslog",
        },
        CommandSpec{
            .path = "show users",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_users",
            .help = "Show user accounts",
        },
        CommandSpec{
            .path = "show poe",
            .args = {{.name = "port", .help = "Port number 1-5",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show_poe",
            .help = "Show PoE status",
        },
        CommandSpec{
            .path = "poe reset",
            .args = {{.name = "port", .help = "Port number 1-5",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "poe_reset",
            .help = "Power-cycle a PoE port",
        },
        CommandSpec{
            .path = "user add",
            .args = {{.name = "name", .help = "Username",
                      .required = true},
                     {.name = "role", .help = "admin|operator",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "user_add",
            .help = "Add or update a user",
        },
        CommandSpec{
            .path = "user remove",
            .args = {{.name = "name", .help = "Username",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "user_remove",
            .help = "Remove a user",
        },
        CommandSpec{
            .path = "ping",
            .args = {{.name = "host", .help = "Host or IP",
                      .required = true}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "ping",
            .help = "Ping a host",
        },
        CommandSpec{
            .path = "traceroute",
            .args = {{.name = "host", .help = "Host or IP",
                      .required = true}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "traceroute",
            .help = "Trace route to host",
        },
        CommandSpec{
            .path = "reboot",
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "reboot",
            .help = "Reboot the system",
        },
    };
  }

  auto RenderResponse(const CommandSpec &cmd,
                      const Response &response,
                      Renderer &renderer) const
      -> void override {
    using namespace cli::render;
    if (response.status != ResponseStatus::Ok) {
      if (response.error) {
        RenderError(response.error->code,
                    response.error->message,
                    response.error->hint, renderer);
      }
      return;
    }

    const auto &wire = cmd.wire_command;

    if (wire == "show_interfaces") {
      Table t;
      AddColumn(t, "port", Align::Left, Priority::High);
      AddColumn(t, "link", Align::Left, Priority::High);
      AddColumn(t, "speed", Align::Left, Priority::Medium);
      AddColumn(t, "duplex", Align::Left, Priority::Medium);
      for (const auto &row : DecodeRows(response)) {
        const bool up = Field(row, 1) == "up";
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1),
                 up ? Semantic::Good : Semantic::Bad},
            Cell{Field(row, 2)},
            Cell{Field(row, 3)},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_counters") {
      Table t;
      AddColumn(t, "port", Align::Left, Priority::High);
      AddColumn(t, "rx_bytes", Align::Right, Priority::High);
      AddColumn(t, "tx_bytes", Align::Right, Priority::High);
      AddColumn(t, "rx_pkts", Align::Right, Priority::Medium);
      AddColumn(t, "tx_pkts", Align::Right, Priority::Medium);
      AddColumn(t, "rx_err", Align::Right, Priority::Low);
      for (const auto &row : DecodeRows(response)) {
        const bool errors = Field(row, 5) != "0";
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1)},
            Cell{Field(row, 2)},
            Cell{Field(row, 3)},
            Cell{Field(row, 4)},
            Cell{Field(row, 5),
                 errors ? Semantic::Warn : Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_mac_table") {
      const auto rows = DecodeRows(response);
      if (rows.empty()) {
        renderer.Out() << "  (empty)\n";
        return;
      }
      Table t;
      AddColumn(t, "mac", Align::Left, Priority::High);
      AddColumn(t, "port", Align::Left, Priority::High);
      AddColumn(t, "vlan", Align::Right, Priority::Medium);
      AddColumn(t, "type", Align::Left, Priority::Medium);
      for (const auto &row : rows) {
        // Static entries are configuration; colouring them apart is
        // what stops an operator reading a configured entry as
        // something the switch happened to learn.
        const bool is_static = Field(row, 3) == "static";
        AddRow(t, {
            Cell{Field(row, 0)},
            Cell{Field(row, 1), Semantic::Emphasis},
            Cell{Field(row, 2), Semantic::Dim},
            Cell{Field(row, 3),
                 is_static ? Semantic::Info : Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_interfaces_detail") {
      Table t;
      AddColumn(t, "port", Align::Left, Priority::High);
      AddColumn(t, "field", Align::Left, Priority::High);
      AddColumn(t, "value", Align::Left, Priority::High);
      for (const auto &row : DecodeRows(response)) {
        auto sem = Semantic::Default;
        if (Field(row, 1) == "link" || Field(row, 1) == "admin") {
          sem = Field(row, 2) == "up" ? Semantic::Good : Semantic::Bad;
        }
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1)},
            Cell{Field(row, 2), sem},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_igmp_snooping") {
      Table t;
      AddColumn(t, "field", Align::Left, Priority::High);
      AddColumn(t, "value", Align::Left, Priority::High);
      for (const auto &row : DecodeRows(response)) {
        auto sem = Semantic::Default;
        if (Field(row, 0) == "snooping") {
          sem = Field(row, 1) == "enabled" ? Semantic::Good
                                           : Semantic::Warn;
        }
        AddRow(t, {Cell{Field(row, 0), Semantic::Emphasis},
                   Cell{Field(row, 1), sem}});
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_neighbors") {
      const auto rows = DecodeRows(response);
      if (rows.empty()) {
        renderer.Out() << "  (no neighbours heard)\n";
        return;
      }
      Table t;
      AddColumn(t, "port", Align::Left, Priority::High);
      AddColumn(t, "chassis", Align::Left, Priority::Medium);
      AddColumn(t, "system", Align::Left, Priority::High);
      AddColumn(t, "their port", Align::Left, Priority::High);
      AddColumn(t, "mgmt address", Align::Left, Priority::Medium);
      AddColumn(t, "capabilities", Align::Left, Priority::Low);
      AddColumn(t, "age", Align::Right, Priority::Low);
      for (const auto &row : rows) {
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1), Semantic::Dim},
            Cell{Field(row, 2), Semantic::Info},
            Cell{Field(row, 3)},
            Cell{Field(row, 4)},
            Cell{Field(row, 5), Semantic::Dim},
            Cell{Field(row, 6), Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_system_services") {
      Table t;
      AddColumn(t, "service", Align::Left, Priority::High);
      AddColumn(t, "state", Align::Left, Priority::High);
      AddColumn(t, "role", Align::Left, Priority::Medium);
      for (const auto &row : DecodeRows(response)) {
        const auto &state = Field(row, 1);
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            // "DOWN" is the row this command exists for: something the
            // configuration asks for is not running.
            Cell{state, state == "running"        ? Semantic::Good
                        : state == "DOWN"         ? Semantic::Bad
                        : state == "not configured" ? Semantic::Dim
                                                  : Semantic::Warn},
            Cell{Field(row, 2), Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_spanning_tree") {
      // Two tables from one blob: the bridge's summary, then the port
      // roles. A single field/value table would bury the per-port state
      // an operator is actually reading this command for.
      Table summary;
      AddColumn(summary, "field", Align::Left, Priority::High);
      AddColumn(summary, "value", Align::Left, Priority::High);
      Table ports;
      AddColumn(ports, "port", Align::Left, Priority::High);
      AddColumn(ports, "role", Align::Left, Priority::High);
      AddColumn(ports, "state", Align::Left, Priority::High);
      AddColumn(ports, "cost", Align::Right, Priority::Medium);
      AddColumn(ports, "edge", Align::Left, Priority::Low);
      AddColumn(ports, "bpdu-guard", Align::Left, Priority::Medium);
      bool have_ports = false;
      for (const auto &row : DecodeRows(response)) {
        if (Field(row, 0) == "bridge") {
          auto sem = Semantic::Default;
          if (Field(row, 1) == "state") sem = Semantic::Warn;
          AddRow(summary, {Cell{Field(row, 1), Semantic::Emphasis},
                           Cell{Field(row, 2), sem}});
          continue;
        }
        have_ports = true;
        const auto &state = Field(row, 3);
        const auto &guard = Field(row, 6);
        // A discarding port is the spanning tree DOING ITS JOB, so it
        // is informational, not bad. A guard-blocked port is bad: it is
        // out of service until somebody acts.
        AddRow(ports, {
            Cell{Field(row, 1), Semantic::Emphasis},
            Cell{Field(row, 2)},
            Cell{state, state == "forwarding" ? Semantic::Good
                        : state == "discarding" ? Semantic::Info
                                                : Semantic::Warn},
            Cell{Field(row, 4), Semantic::Dim},
            Cell{Field(row, 5), Semantic::Dim},
            Cell{guard, guard == "BLOCKED" ? Semantic::Bad
                        : guard == "on"    ? Semantic::Info
                                           : Semantic::Dim},
        });
      }
      RenderFormatted(summary, renderer);
      if (have_ports) {
        renderer.Out() << "\n";
        RenderFormatted(ports, renderer);
      }
    } else if (wire == "show_spanning_tree_statistics") {
      const auto rows = DecodeRows(response);
      if (rows.empty()) {
        renderer.Out() << "  (spanning tree is not running)\n";
        return;
      }
      Table t;
      AddColumn(t, "port", Align::Left, Priority::High);
      AddColumn(t, "tx_bpdu", Align::Right, Priority::High);
      AddColumn(t, "rx_bpdu", Align::Right, Priority::High);
      AddColumn(t, "tx_tcn", Align::Right, Priority::Medium);
      AddColumn(t, "rx_tcn", Align::Right, Priority::Medium);
      AddColumn(t, "→fwd", Align::Right, Priority::Low);
      AddColumn(t, "→blk", Align::Right, Priority::Low);
      for (const auto &row : rows) {
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1)},
            Cell{Field(row, 2)},
            Cell{Field(row, 3), Semantic::Dim},
            Cell{Field(row, 4), Semantic::Dim},
            Cell{Field(row, 5), Semantic::Dim},
            Cell{Field(row, 6), Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_vlans") {
      const auto rows = DecodeRows(response);
      if (rows.empty()) {
        renderer.Out() << "  (no VLANs)\n";
        return;
      }
      Table t;
      AddColumn(t, "vid", Align::Right, Priority::High);
      AddColumn(t, "name", Align::Left, Priority::High);
      AddColumn(t, "address", Align::Left, Priority::High);
      AddColumn(t, "members", Align::Left, Priority::Medium);
      for (const auto &row : rows) {
        const auto &addr = Field(row, 2);
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1), Field(row, 1) == "-" ? Semantic::Dim
                                                     : Semantic::Info},
            // An address is what makes a VLAN routed rather than
            // merely switched, so it should not read as just another
            // column of text.
            Cell{addr, addr == "-" ? Semantic::Dim : Semantic::Good},
            Cell{Field(row, 3)},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_dhcp_leases") {
      const auto rows = DecodeRows(response);
      if (rows.empty()) {
        renderer.Out() << "  (no leases)\n";
        return;
      }
      Table t;
      AddColumn(t, "address", Align::Left, Priority::High);
      AddColumn(t, "mac", Align::Left, Priority::High);
      AddColumn(t, "hostname", Align::Left, Priority::High);
      AddColumn(t, "expires in", Align::Right, Priority::Medium);
      for (const auto &row : rows) {
        const auto &left = Field(row, 3);
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1), Semantic::Dim},
            Cell{Field(row, 2), Semantic::Info},
            Cell{left, left == "expired" ? Semantic::Warn
                                         : Semantic::Default},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_dhcp_server") {
      Table t;
      AddColumn(t, "pool", Align::Left, Priority::High);
      AddColumn(t, "range", Align::Left, Priority::High);
      AddColumn(t, "lease", Align::Right, Priority::Medium);
      AddColumn(t, "in use", Align::Right, Priority::Medium);
      for (const auto &row : DecodeRows(response)) {
        if (Field(row, 0) == "server") {
          const auto &state = Field(row, 1);
          AddRow(t, {Cell{"server", Semantic::Emphasis},
                     Cell{state, state == "running" ? Semantic::Good
                                                    : Semantic::Bad},
                     Cell{""}, Cell{""}});
          continue;
        }
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1)},
            Cell{Field(row, 2), Semantic::Dim},
            Cell{Field(row, 3), Semantic::Info},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_route") {
      Table t;
      AddColumn(t, "destination", Align::Left, Priority::High);
      AddColumn(t, "via", Align::Left, Priority::High);
      AddColumn(t, "interface", Align::Left, Priority::Medium);
      AddColumn(t, "origin", Align::Left, Priority::Low);
      for (const auto &row : DecodeRows(response)) {
        if (Field(row, 0) == "forwarding") {
          // Routes with forwarding off are a configuration that looks
          // right and moves no packets; it belongs at the top of the
          // table, not in a different command.
          AddRow(t, {Cell{"forwarding", Semantic::Emphasis},
                     Cell{Field(row, 1), Field(row, 1) == "enabled"
                                             ? Semantic::Good
                                             : Semantic::Warn},
                     Cell{""}, Cell{""}});
          continue;
        }
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1)},
            Cell{Field(row, 2)},
            Cell{Field(row, 3), Field(row, 3) == "static" ? Semantic::Info
                                                          : Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_system" || wire == "show_ntp" ||
               wire == "show_fabric") {
      Table t;
      AddColumn(t, "field", Align::Left, Priority::High);
      AddColumn(t, "value", Align::Left, Priority::High);
      for (const auto &row : DecodeRows(response)) {
        auto sem = Semantic::Default;
        if (wire == "show_ntp" && Field(row, 0) == "synced") {
          sem = Field(row, 1) == "yes" ? Semantic::Good
                                       : Semantic::Warn;
        }
        // A fabric that is missing, filtering-off or has a detached
        // member is the reason VLAN config appears to do nothing, so
        // colour those rows rather than leaving them to be read.
        if (wire == "show_fabric") {
          const auto &field = Field(row, 0);
          const auto &value = Field(row, 1);
          if (field == "exists" || field == "vlan filtering") {
            sem = value == "yes" ? Semantic::Good : Semantic::Bad;
          } else if (field == "state") {
            sem = value == "up" ? Semantic::Good : Semantic::Bad;
          } else if (field == "detached" || field == "absent") {
            sem = value == "-" ? Semantic::Dim : Semantic::Warn;
          } else if (field == "conduit") {
            sem = value == "-" ? Semantic::Warn : Semantic::Default;
          }
        }
        // A box whose configuration drifted out of band, or whose boot
        // outcome is unknown, must not read like a healthy row.
        if (wire == "show_system" && Field(row, 0) == "config-divergence") {
          const auto &value = Field(row, 1);
          sem = value == "none"      ? Semantic::Good
                : value.starts_with("unknown") ? Semantic::Warn
                                               : Semantic::Bad;
        }
        if (wire == "show_system" && Field(row, 0) == "alarms") {
          sem = Field(row, 1) == "none" ? Semantic::Good : Semantic::Bad;
        }
        AddRow(t, {Cell{Field(row, 0), Semantic::Emphasis},
                   Cell{Field(row, 1), sem}});
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_ip") {
      Table t;
      AddColumn(t, "interface", Align::Left, Priority::High);
      AddColumn(t, "address", Align::Left, Priority::High);
      AddColumn(t, "state", Align::Left, Priority::Medium);
      AddColumn(t, "mac", Align::Left, Priority::Low);
      for (const auto &row : DecodeRows(response)) {
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1)},
            Cell{Field(row, 2), Field(row, 2) == "up"
                                    ? Semantic::Good
                                    : Semantic::Bad},
            Cell{Field(row, 3), Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_dns") {
      const auto rows = DecodeRows(response);
      if (rows.empty()) {
        renderer.Out() << "  (no DNS configured)\n";
        return;
      }
      Table t;
      AddColumn(t, "nameserver", Align::Left, Priority::High);
      for (const auto &row : rows) {
        AddRow(t, {Cell{Field(row, 0), Semantic::Info}});
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_users") {
      Table t;
      AddColumn(t, "user", Align::Left, Priority::High);
      AddColumn(t, "role", Align::Left, Priority::High);
      AddColumn(t, "uid", Align::Right, Priority::Medium);
      for (const auto &row : DecodeRows(response)) {
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1), Field(row, 1) == "admin"
                                    ? Semantic::Warn
                                    : Semantic::Info},
            Cell{Field(row, 2), Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show_poe") {
      Table t;
      AddColumn(t, "port", Align::Right, Priority::High);
      AddColumn(t, "status", Align::Left, Priority::High);
      AddColumn(t, "voltage", Align::Right, Priority::Medium);
      AddColumn(t, "current", Align::Right, Priority::Medium);
      AddColumn(t, "power", Align::Right, Priority::High);
      AddColumn(t, "class", Align::Left, Priority::Low);
      std::string total;
      for (const auto &row : DecodeRows(response)) {
        if (Field(row, 0) == "total") {
          total = Field(row, 1);
          continue;
        }
        const auto &state = Field(row, 2);
        AddRow(t, {
            Cell{Field(row, 0), Semantic::Emphasis},
            Cell{Field(row, 1),
                 state == "delivering" ? Semantic::Good :
                 state == "enabled" ? Semantic::Warn
                                    : Semantic::Dim},
            Cell{std::format("{}V", Field(row, 3))},
            Cell{std::format("{}mA", Field(row, 4))},
            Cell{std::format("{}W", Field(row, 5))},
            Cell{Field(row, 6), Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
      if (!total.empty()) {
        renderer.Out() << std::format("  total: {}W\n", total);
      }
    } else if (wire == "show_version" || wire == "show_log" ||
               wire == "ping" || wire == "traceroute") {
      // Free-form text: pass through as-is.
      renderer.Out() << std::string(response.data.begin(),
                                    response.data.end());
    } else if (wire == "poe_reset" || wire == "user_add" ||
               wire == "user_remove" || wire == "reboot" ||
               wire == "clear_counters" || wire == "clear_mac_table" ||
               wire == "clear_spanning_tree_statistics" ||
               wire == "clear_bpdu_guard" ||
               wire == "clear_dhcp_lease") {
      renderer.Out() << "  "
                     << std::string(response.data.begin(),
                                    response.data.end());
    } else if (wire == "show_config" || wire == "show_diff" ||
               wire == "show_commit") {
      // Config surfaces fold into the hierarchical (Junos-style)
      // view; diff markers keep their colour in the gutter.
      if (response.data.empty()) {
        renderer.Out()
            << "  (no configuration yet — run `configure` then "
               "`set`)\n";
        return;
      }
      cli::render::RenderConfigTree(
          std::string(response.data.begin(), response.data.end()),
          renderer);
    } else {
      RenderKvLines(response, renderer);
    }
  }

  auto EventTopicsFor(const CommandSpec &) const
      -> std::vector<std::string> override {
    return {};
  }

  auto RenderEvent(const std::string &,
                   const cli::protocol::Event &,
                   Renderer &) const -> void override {}

 private:
  /// Generic renderer for the confd runtime's key=value line
  /// format (show config / show commits / show commit / show
  /// status). `show commit` prefixes lines with a diff marker
  /// (+/-/~/=); colour accordingly.
  static auto RenderKvLines(const Response &response,
                            Renderer &renderer) -> void {
    using namespace cli::render;
    if (response.data.empty()) {
      Table t;
      AddColumn(t, "status", Align::Left, Priority::High);
      AddRow(t, {Cell{"ok", Semantic::Good}});
      RenderFormatted(t, renderer);
      return;
    }
    Table t;
    AddColumn(t, "field", Align::Left, Priority::High);
    AddColumn(t, "value", Align::Left, Priority::High);
    const std::string body(response.data.begin(),
                           response.data.end());
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
      if (line.empty()) continue;
      Semantic key_sem = Semantic::Emphasis;
      Semantic val_sem = Semantic::Default;
      char marker = 0;
      if (line[0] == '+' || line[0] == '-' || line[0] == '~' ||
          line[0] == '=' || line[0] == '!') {
        marker = line[0];
        line.erase(0, 1);
        switch (marker) {
          case '+': key_sem = val_sem = Semantic::Good; break;
          case '-': key_sem = val_sem = Semantic::Bad; break;
          case '~': key_sem = val_sem = Semantic::Warn; break;
          case '=': key_sem = val_sem = Semantic::Dim; break;
          // Anti-lockout and friends: not a diff line at all, but the
          // one line in this output an operator must not skim past.
          case '!': key_sem = val_sem = Semantic::Bad; break;
        }
      }
      const auto eq = line.find('=');
      std::string key = line;
      std::string val;
      if (eq != std::string::npos) {
        key = line.substr(0, eq);
        val = line.substr(eq + 1);
      }
      if (marker == 0) {
        if (key == "commit_id" || key == "status") {
          val_sem = Semantic::Good;
        } else if (val.empty() || val == "<none>") {
          val_sem = Semantic::Dim;
        }
      }
      if (marker) key = std::format("{} {}", marker, key);
      AddRow(t, {Cell{key, key_sem}, Cell{val, val_sem}});
    }
    RenderFormatted(t, renderer);
  }

  cli::schema::SchemaHandle schema_;
};

}  // namespace

auto MakeSwitchAdapter(cli::schema::SchemaHandle schema)
    -> std::unique_ptr<ProductAdapter> {
  return std::make_unique<SwitchAdapter>(std::move(schema));
}

}  // namespace einheit::s5
