/// @file switch_adapter.cc
/// @brief KSZ9477 switch adapter for Linux DSA + TPS23861 PoE.
// Copyright (c) 2026 Einheit Networks

#include "einheit/s5/dsa.h"
#include "einheit/s5/poe.h"
#include "einheit/s5/sys.h"
#include "einheit/s5/util.h"

#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/render/table.h"
#include "einheit/cli/schema.h"

namespace einheit::s5 {
namespace {

using cli::CommandSpec;
using cli::ProductAdapter;
using cli::ProductMetadata;
using cli::protocol::Response;
using cli::protocol::ResponseStatus;
using cli::render::Renderer;
using cli::schema::Schema;

auto ExtractArgs(const Response &r) -> std::vector<std::string> {
  std::vector<std::string> args;
  std::string current;
  for (auto b : r.data) {
    if (b == 0) {
      args.push_back(std::move(current));
      current.clear();
    } else {
      current += static_cast<char>(b);
    }
  }
  if (!current.empty()) args.push_back(std::move(current));
  return args;
}

auto Arg(const Response &r, std::size_t idx) -> std::string {
  auto args = ExtractArgs(r);
  return idx < args.size() ? args[idx] : "";
}

constexpr const char *kSchemaYaml = R"(
version: 1
product: s5

config:
  hostname:
    type: string
    help: "Switch hostname"
    example: "s5-rack01"
)";

class SwitchAdapter : public ProductAdapter {
 public:
  SwitchAdapter() {
    ports_ = dsa::DiscoverPorts();
  }

  auto Metadata() const -> ProductMetadata override {
    return {
        .id = "s5",
        .display_name = "einheit S5",
        .version = "0.1.0",
        .banner = "einheit S5 — 5-port managed gigabit PoE switch",
        .prompt = "S5",
    };
  }

  auto GetSchema() const -> const Schema & override {
    return *schema_;
  }

  void SetSchema(std::shared_ptr<Schema> s) {
    schema_ = std::move(s);
  }

  auto ControlSocketPath() const -> std::string override {
    return "";
  }

  auto EventSocketPath() const -> std::string override {
    return "";
  }

  auto Commands() const
      -> std::vector<CommandSpec> override {
    return {
        CommandSpec{
            .path = "show interfaces",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show interfaces",
            .help = "Show port status",
        },
        CommandSpec{
            .path = "show counters",
            .args = {{.name = "port", .help = "Port name",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show counters",
            .help = "Show port counters",
        },
        CommandSpec{
            .path = "show mac-table",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show mac-table",
            .help = "Show learned MAC addresses",
        },
        CommandSpec{
            .path = "show vlans",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show vlans",
            .help = "Show VLAN configuration",
        },
        CommandSpec{
            .path = "show version",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show version",
            .help = "Show switch information",
        },
        CommandSpec{
            .path = "shell",
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "shell",
            .help = "Drop to bash (exit returns here)",
        },
        // System management.
        CommandSpec{
            .path = "show system",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show system",
            .help = "Show system info",
        },
        CommandSpec{
            .path = "show ip",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show ip",
            .help = "Show IP addresses",
        },
        CommandSpec{
            .path = "show dns",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show dns",
            .help = "Show DNS servers",
        },
        CommandSpec{
            .path = "show ntp",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show ntp",
            .help = "Show NTP status",
        },
        CommandSpec{
            .path = "show log",
            .args = {{.name = "lines", .help = "Number of lines",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show log",
            .help = "Show syslog",
        },
        CommandSpec{
            .path = "show users",
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show users",
            .help = "Show user accounts",
        },
        CommandSpec{
            .path = "set hostname",
            .args = {{.name = "name", .help = "Hostname",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "set hostname",
            .help = "Set system hostname",
        },
        CommandSpec{
            .path = "set interface address",
            .args = {{.name = "iface", .help = "e.g. eth0",
                      .required = true},
                     {.name = "addr", .help = "IP/mask e.g. 10.0.0.1/24",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "set interface address",
            .help = "Set static IP address",
        },
        CommandSpec{
            .path = "set interface dhcp",
            .args = {{.name = "iface", .help = "e.g. eth0",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "set interface dhcp",
            .help = "Enable DHCP on interface",
        },
        CommandSpec{
            .path = "set dns",
            .args = {{.name = "server", .help = "DNS server IP",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "set dns",
            .help = "Set DNS nameserver",
        },
        CommandSpec{
            .path = "set ntp",
            .args = {{.name = "server", .help = "NTP server IP",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "set ntp",
            .help = "Set NTP server",
        },
        CommandSpec{
            .path = "set user",
            .args = {{.name = "name", .help = "Username",
                      .required = true},
                     {.name = "role", .help = "admin|operator",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "set user",
            .help = "Add or update a user",
        },
        CommandSpec{
            .path = "delete user",
            .args = {{.name = "name", .help = "Username",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "delete user",
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
        // PoE commands.
        CommandSpec{
            .path = "show poe",
            .args = {{.name = "port", .help = "Port number 1-5",
                      .required = false}},
            .role = cli::RoleGate::AnyAuthenticated,
            .wire_command = "show poe",
            .help = "Show PoE status",
        },
        CommandSpec{
            .path = "set poe enable",
            .args = {{.name = "port", .help = "Port number 1-5",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "set poe enable",
            .help = "Enable PoE on a port",
        },
        CommandSpec{
            .path = "set poe disable",
            .args = {{.name = "port", .help = "Port number 1-5",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "set poe disable",
            .help = "Disable PoE on a port",
        },
        CommandSpec{
            .path = "set poe reset",
            .args = {{.name = "port", .help = "Port number 1-5",
                      .required = true}},
            .role = cli::RoleGate::AdminOnly,
            .wire_command = "set poe reset",
            .help = "Power-cycle a PoE port",
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

    if (wire == "show interfaces") {
      Table t;
      AddColumn(t, "port", Align::Left, Priority::High);
      AddColumn(t, "link", Align::Left, Priority::High);
      AddColumn(t, "speed", Align::Left, Priority::Medium);
      AddColumn(t, "duplex", Align::Left, Priority::Medium);

      for (const auto &name : ports_) {
        auto st = dsa::GetPortStatus(name);
        AddRow(t, {
            Cell{st.name, Semantic::Emphasis},
            Cell{st.link ? "up" : "down",
                 st.link ? Semantic::Good : Semantic::Bad},
            Cell{st.speed},
            Cell{st.duplex},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show counters") {
      Table t;
      AddColumn(t, "port", Align::Left, Priority::High);
      AddColumn(t, "rx_bytes", Align::Right, Priority::High);
      AddColumn(t, "tx_bytes", Align::Right, Priority::High);
      AddColumn(t, "rx_pkts", Align::Right, Priority::Medium);
      AddColumn(t, "tx_pkts", Align::Right, Priority::Medium);
      AddColumn(t, "rx_err", Align::Right, Priority::Low);

      for (const auto &name : ports_) {
        auto c = dsa::GetPortCounters(name);
        AddRow(t, {
            Cell{c.name, Semantic::Emphasis},
            Cell{std::to_string(c.rx_bytes)},
            Cell{std::to_string(c.tx_bytes)},
            Cell{std::to_string(c.rx_packets)},
            Cell{std::to_string(c.tx_packets)},
            Cell{std::to_string(c.rx_errors),
                 c.rx_errors ? Semantic::Warn : Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show mac-table") {
      auto macs = dsa::GetMacTable();
      Table t;
      AddColumn(t, "mac", Align::Left, Priority::High);
      AddColumn(t, "port", Align::Left, Priority::High);
      AddColumn(t, "vlan", Align::Right, Priority::Medium);

      for (const auto &e : macs) {
        AddRow(t, {
            Cell{e.mac},
            Cell{e.port, Semantic::Emphasis},
            Cell{std::to_string(e.vid), Semantic::Dim},
        });
      }
      if (macs.empty()) {
        renderer.Out() << "  (empty)\n";
      } else {
        RenderFormatted(t, renderer);
      }
    } else if (wire == "show vlans") {
      auto vlans = dsa::GetVlans();
      Table t;
      AddColumn(t, "vid", Align::Right, Priority::High);
      AddColumn(t, "port", Align::Left, Priority::High);
      AddColumn(t, "untagged", Align::Left, Priority::Medium);
      AddColumn(t, "pvid", Align::Left, Priority::Medium);

      for (const auto &v : vlans) {
        AddRow(t, {
            Cell{std::to_string(v.vid), Semantic::Emphasis},
            Cell{v.port},
            Cell{v.untagged ? "yes" : "-", Semantic::Dim},
            Cell{v.pvid ? "yes" : "-", Semantic::Dim},
        });
      }
      if (vlans.empty()) {
        renderer.Out() << "  (no VLANs)\n";
      } else {
        RenderFormatted(t, renderer);
      }
    } else if (wire == "show version") {
      renderer.Out()
          << std::format("  product: einheit s5\n")
          << std::format("  ports:   {}\n", ports_.size());
      for (const auto &p : ports_) {
        renderer.Out() << std::format("    {}\n", p);
      }
    } else if (wire == "shell") {
      renderer.Out() << "  dropping to bash (exit to return)\n";
      renderer.Out().flush();
      std::system("/bin/bash");
    } else if (wire == "show system") {
      auto hostname = sys::GetHostname();
      auto uptime = sys::GetUptime();
      auto mem = sys::GetMemInfo();
      auto disk = sys::GetDiskInfo();
      auto temp = sys::GetCpuTemp();
      Table t;
      AddColumn(t, "field", Align::Left, Priority::High);
      AddColumn(t, "value", Align::Left, Priority::High);
      AddRow(t, {Cell{"hostname", Semantic::Emphasis},
                 Cell{hostname}});
      AddRow(t, {Cell{"uptime", Semantic::Emphasis},
                 Cell{uptime}});
      AddRow(t, {Cell{"cpu temp", Semantic::Emphasis},
                 Cell{temp}});
      AddRow(t, {Cell{"memory", Semantic::Emphasis},
                 Cell{std::format("{} MB / {} MB",
                      mem.avail_kb / 1024,
                      mem.total_kb / 1024)}});
      AddRow(t, {Cell{"disk", Semantic::Emphasis},
                 Cell{std::format("{} / {} ({})",
                      disk.used, disk.size, disk.use_pct)}});
      AddRow(t, {Cell{"ports", Semantic::Emphasis},
                 Cell{std::to_string(ports_.size())}});
      RenderFormatted(t, renderer);
    } else if (wire == "show ip") {
      auto ifaces = sys::GetInterfaces();
      Table t;
      AddColumn(t, "interface", Align::Left, Priority::High);
      AddColumn(t, "address", Align::Left, Priority::High);
      AddColumn(t, "state", Align::Left, Priority::Medium);
      AddColumn(t, "mac", Align::Left, Priority::Low);
      for (const auto &i : ifaces) {
        AddRow(t, {
            Cell{i.name, Semantic::Emphasis},
            Cell{i.address},
            Cell{i.state, i.state == "up"
                              ? Semantic::Good
                              : Semantic::Bad},
            Cell{i.mac, Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "show dns") {
      auto servers = sys::GetDnsServers();
      Table t;
      AddColumn(t, "nameserver", Align::Left, Priority::High);
      for (const auto &s : servers) {
        AddRow(t, {Cell{s, Semantic::Info}});
      }
      if (servers.empty()) {
        renderer.Out() << "  (no DNS configured)\n";
      } else {
        RenderFormatted(t, renderer);
      }
    } else if (wire == "show ntp") {
      auto ntp = sys::GetNtpStatus();
      Table t;
      AddColumn(t, "field", Align::Left, Priority::High);
      AddColumn(t, "value", Align::Left, Priority::High);
      AddRow(t, {Cell{"synced", Semantic::Emphasis},
                 Cell{ntp.synced ? "yes" : "no",
                      ntp.synced ? Semantic::Good
                                 : Semantic::Warn}});
      AddRow(t, {Cell{"server", Semantic::Emphasis},
                 Cell{ntp.server}});
      RenderFormatted(t, renderer);
    } else if (wire == "show log") {
      auto arg = Arg(response, 0);
      int lines = arg.empty() ? 20 : std::stoi(arg);
      auto log = sys::GetSyslog(lines);
      renderer.Out() << log;
    } else if (wire == "show users") {
      auto users = sys::GetUsers();
      Table t;
      AddColumn(t, "user", Align::Left, Priority::High);
      AddColumn(t, "role", Align::Left, Priority::High);
      AddColumn(t, "uid", Align::Right, Priority::Medium);
      for (const auto &u : users) {
        AddRow(t, {
            Cell{u.name, Semantic::Emphasis},
            Cell{u.role, u.role == "admin"
                             ? Semantic::Warn
                             : Semantic::Info},
            Cell{std::to_string(u.uid), Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
    } else if (wire == "set hostname") {
      auto name = Arg(response, 0);
      if (sys::SetHostname(name)) {
        renderer.Out() << std::format("  hostname set to '{}'\n",
                                      name);
      } else {
        renderer.Out() << "  failed to set hostname\n";
      }
    } else if (wire == "set interface address") {
      auto iface = Arg(response, 0);
      auto addr = Arg(response, 1);
      if (sys::SetInterfaceAddr(iface, addr)) {
        renderer.Out() << std::format(
            "  {} set to {}\n", iface, addr);
      } else {
        renderer.Out() << "  failed to set address\n";
      }
    } else if (wire == "set interface dhcp") {
      auto iface = Arg(response, 0);
      if (sys::SetInterfaceDhcp(iface)) {
        renderer.Out() << std::format(
            "  DHCP started on {}\n", iface);
      } else {
        renderer.Out() << "  failed to start DHCP\n";
      }
    } else if (wire == "set dns") {
      auto server = Arg(response, 0);
      if (sys::SetDnsServers({server})) {
        renderer.Out() << std::format(
            "  DNS set to {}\n", server);
      } else {
        renderer.Out() << "  failed to set DNS\n";
      }
    } else if (wire == "set ntp") {
      auto server = Arg(response, 0);
      if (sys::SetNtpServer(server)) {
        renderer.Out() << std::format(
            "  NTP set to {}\n", server);
      } else {
        renderer.Out() << "  failed to set NTP\n";
      }
    } else if (wire == "set user") {
      auto name = Arg(response, 0);
      auto role = Arg(response, 1);
      if (sys::AddUser(name, role)) {
        renderer.Out() << std::format(
            "  user '{}' added as {}\n", name, role);
      } else {
        renderer.Out() << "  failed to add user\n";
      }
    } else if (wire == "delete user") {
      auto name = Arg(response, 0);
      if (sys::DelUser(name)) {
        renderer.Out() << std::format(
            "  user '{}' removed\n", name);
      } else {
        renderer.Out() << "  failed to remove user\n";
      }
    } else if (wire == "ping") {
      auto host = Arg(response, 0);
      renderer.Out().flush();
      std::system(
          ("ping -c 4 " + host + " 2>&1").c_str());
    } else if (wire == "traceroute") {
      auto host = Arg(response, 0);
      renderer.Out().flush();
      std::system(
          ("traceroute " + host + " 2>&1").c_str());
    } else if (wire == "reboot") {
      renderer.Out() << "  rebooting...\n";
      renderer.Out().flush();
      sys::Reboot();
    } else if (wire == "show poe") {
      auto statuses = poe::GetAllStatus();
      Table t;
      AddColumn(t, "port", Align::Right, Priority::High);
      AddColumn(t, "status", Align::Left, Priority::High);
      AddColumn(t, "voltage", Align::Right, Priority::Medium);
      AddColumn(t, "current", Align::Right, Priority::Medium);
      AddColumn(t, "power", Align::Right, Priority::High);
      AddColumn(t, "class", Align::Left, Priority::Low);
      for (const auto &s : statuses) {
        AddRow(t, {
            Cell{std::to_string(s.port), Semantic::Emphasis},
            Cell{s.status,
                 s.delivering ? Semantic::Good :
                 s.enabled ? Semantic::Warn : Semantic::Dim},
            Cell{std::format("{:.1f}V", s.voltage_v)},
            Cell{std::format("{:.0f}mA", s.current_ma)},
            Cell{std::format("{:.1f}W", s.power_w)},
            Cell{s.classification, Semantic::Dim},
        });
      }
      RenderFormatted(t, renderer);
      renderer.Out() << std::format(
          "  total: {:.1f}W\n", poe::GetTotalPower());
    } else if (wire == "set poe enable") {
      int port = std::stoi(Arg(response, 0));
      if (poe::SetPortEnabled(port, true)) {
        renderer.Out() << std::format(
            "  PoE enabled on port {}\n", port);
      } else {
        renderer.Out() << "  failed to enable PoE\n";
      }
    } else if (wire == "set poe disable") {
      int port = std::stoi(Arg(response, 0));
      if (poe::SetPortEnabled(port, false)) {
        renderer.Out() << std::format(
            "  PoE disabled on port {}\n", port);
      } else {
        renderer.Out() << "  failed to disable PoE\n";
      }
    } else if (wire == "set poe reset") {
      int port = std::stoi(Arg(response, 0));
      renderer.Out() << std::format(
          "  power-cycling port {}...\n", port);
      renderer.Out().flush();
      if (poe::ResetPort(port)) {
        renderer.Out() << "  done\n";
      } else {
        renderer.Out() << "  failed\n";
      }
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
  std::vector<std::string> ports_;
  std::shared_ptr<Schema> schema_;
};

}  // namespace

auto MakeSwitchAdapter()
    -> std::unique_ptr<ProductAdapter> {
  return std::make_unique<SwitchAdapter>();
}

}  // namespace einheit::s5
