// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <libgen.h>
#include <unistd.h>

#include <csignal>
#include <iostream>
#include <memory>
#include <string>

#include <absl/flags/flag.h>
#include <absl/flags/parse.h>
#include <absl/log/initialize.h>
#include <absl/strings/str_split.h>
#include <oatpp-swagger/Controller.hpp>
#include <oatpp-swagger/Resources.hpp>
#include <oatpp/network/Server.hpp>
#include <oatpp/network/tcp/server/ConnectionProvider.hpp>
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/HttpRouter.hpp>

#include <packager/version/version.h>
#include <packager/webapi/controller/event_controller.hpp>
#include <packager/webapi/controller/health_controller.hpp>
#include <packager/webapi/service/event_manager.h>

ABSL_FLAG(int32_t, api_port, 8088, "HTTP port for the web API.");
ABSL_FLAG(std::string, api_bind_address, "0.0.0.0", "Bind address.");
ABSL_FLAG(std::string,
          api_token,
          "",
          "Static bearer token. When set, /api/v1/* requests require "
          "'Authorization: Bearer <token>'. /health and /swagger stay open.");
ABSL_FLAG(std::string,
          packager_bin,
          "",
          "Path to the packager binary spawned per event. Defaults to "
          "'packager' next to this executable.");
ABSL_FLAG(std::string,
          event_metrics_port_range,
          "19100-19199",
          "Inclusive port range allocated to per-event --metrics_port.");
ABSL_FLAG(std::string,
          event_log_dir,
          "/tmp",
          "Directory for per-event stderr log files.");

namespace shaka {
namespace webapi {
namespace {

volatile std::sig_atomic_t g_shutdown_requested = 0;

void HandleSignal(int) {
  g_shutdown_requested = 1;
}

std::string DefaultPackagerBin(const char* argv0) {
  std::string self(argv0);
  char* dir = dirname(self.data());
  return std::string(dir) + "/packager";
}

int RunServer(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  oatpp::base::Environment::init();

  EventManager::Config config;
  config.packager_bin = absl::GetFlag(FLAGS_packager_bin).empty()
                            ? DefaultPackagerBin(argv[0])
                            : absl::GetFlag(FLAGS_packager_bin);
  config.log_dir = absl::GetFlag(FLAGS_event_log_dir);
  const std::vector<std::string> range =
      absl::StrSplit(absl::GetFlag(FLAGS_event_metrics_port_range), '-');
  if (range.size() != 2) {
    std::cerr << "--event_metrics_port_range must be MIN-MAX" << std::endl;
    return 1;
  }
  config.metrics_port_min = std::stoi(range[0]);
  config.metrics_port_max = std::stoi(range[1]);

  auto event_manager = std::make_shared<EventManager>(config);

  auto router = oatpp::web::server::HttpRouter::createShared();
  auto object_mapper =
      oatpp::parser::json::mapping::ObjectMapper::createShared();
  object_mapper->getSerializer()->getConfig()->includeNullFields = false;

  auto health_controller =
      std::make_shared<HealthController>(object_mapper);
  auto event_controller =
      std::make_shared<EventController>(object_mapper, event_manager);
  router->addController(health_controller);
  router->addController(event_controller);

  auto doc_info =
      oatpp::swagger::DocumentInfo::Builder()
          .setTitle("Shaka Packager Web API")
          .setVersion(GetPackagerVersion().c_str())
          .setDescription("Event control API for packaging as a service")
          .setLicenseName("BSD")
          .setLicenseUrl(
              "https://developers.google.com/open-source/licenses/bsd")
          .build();
  auto swagger_resources =
      oatpp::swagger::Resources::loadResources(OATPP_SWAGGER_RES_PATH);
  oatpp::web::server::api::Endpoints doc_endpoints;
  doc_endpoints.append(health_controller->getEndpoints());
  doc_endpoints.append(event_controller->getEndpoints());
  router->addController(oatpp::swagger::Controller::createShared(
      doc_endpoints, doc_info, swagger_resources));

  auto connection_handler =
      oatpp::web::server::HttpConnectionHandler::createShared(router);
  auto connection_provider =
      oatpp::network::tcp::server::ConnectionProvider::createShared(
          oatpp::network::Address(
              absl::GetFlag(FLAGS_api_bind_address).c_str(),
              static_cast<uint16_t>(absl::GetFlag(FLAGS_api_port))));
  oatpp::network::Server server(connection_provider, connection_handler);

  std::signal(SIGTERM, HandleSignal);
  std::signal(SIGINT, HandleSignal);

  std::cout << "packager-api listening on "
            << absl::GetFlag(FLAGS_api_bind_address) << ":"
            << absl::GetFlag(FLAGS_api_port) << std::endl;

  server.run([&] { return g_shutdown_requested == 0; });

  std::cout << "shutting down; draining events" << std::endl;
  event_manager->Shutdown();
  oatpp::base::Environment::destroy();
  return 0;
}

}  // namespace
}  // namespace webapi
}  // namespace shaka

int main(int argc, char** argv) {
  return shaka::webapi::RunServer(argc, argv);
}
