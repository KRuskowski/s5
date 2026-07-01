/// @file main.cc
/// @brief einheit s5 entry point — direct-to-hardware CLI.
// Copyright (c) 2026 Einheit Networks

#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <string>

#include "einheit/s5/poe.h"
#include "einheit/cli/adapter.h"
#include "einheit/cli/auth.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/render/terminal_caps.h"
#include "einheit/cli/shell.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::s5 {
auto MakeSwitchAdapter()
    -> std::unique_ptr<cli::ProductAdapter>;
}

namespace {

using namespace einheit::cli;

/// Local transport — returns an empty OK for every request.
/// The adapter reads hardware state in RenderResponse, so the
/// response data payload is unused.
class LocalTransport : public transport::Transport {
 public:
  auto Connect()
      -> std::expected<void,
                       Error<transport::TransportError>>
      override {
    return {};
  }

  auto Disconnect() -> void override {}

  auto SendRequest(const protocol::Request &req,
                   std::chrono::milliseconds)
      -> std::expected<protocol::Response,
                       Error<transport::TransportError>>
      override {
    protocol::Response r;
    r.status = protocol::ResponseStatus::Ok;
    // Pack the request args into response data so the
    // adapter can read them. Simple format: each arg is
    // NUL-terminated in the data blob.
    for (const auto &arg : req.args) {
      for (char c : arg) r.data.push_back(
          static_cast<std::uint8_t>(c));
      r.data.push_back(0);
    }
    return r;
  }

  auto Subscribe(const std::string &,
                 transport::EventCallback)
      -> std::expected<void,
                       Error<transport::TransportError>>
      override {
    return {};
  }

  auto Unsubscribe(const std::string &)
      -> std::expected<void,
                       Error<transport::TransportError>>
      override {
    return {};
  }
};

}  // namespace

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  // Initialize PoE I2C bus.
  if (!einheit::s5::poe::Init("/dev/i2c-0")) {
    std::cerr << "warning: PoE I2C bus not available\n";
  }

  auto adapter = einheit::s5::MakeSwitchAdapter();

  CommandTree tree;
  for (const auto &spec : adapter->Commands()) {
    Register(tree, spec);
  }

  auto caller_result = auth::ResolveLocal();
  auth::CallerIdentity caller;
  if (caller_result) {
    caller = *caller_result;
  } else {
    caller.user = "admin";
    caller.transport = "console";
  }
  // Root gets admin. Everyone else keeps their resolved role.
  if (caller.user == "root") {
    caller.role = RoleGate::AdminOnly;
  }

  auto caps = render::DetectTerminal();

  shell::Shell sh;
  sh.tx = std::make_unique<LocalTransport>();
  sh.adapter = std::move(adapter);
  sh.tree = std::move(tree);
  sh.caps = caps;
  sh.caller = caller;
  sh.target_name = "local";

  auto result = shell::RunShell(sh);
  if (!result) {
    std::cerr << std::format("shell error: {}\n",
                             result.error().message);
    return 1;
  }
  return 0;
}
