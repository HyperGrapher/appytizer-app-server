#pragma once
#include "common/config.hpp"
#include "common/service_provider.hpp"
#include "engine/dns/dns_server.hpp"
#include "engine/ipc/pipe_server.hpp"
#include "engine/sites/site_registry.hpp"
#include <atomic>
#include <nlohmann/json.hpp>

namespace appytizer {
/// Coordinates persistent engine subsystems and implements IPC commands.
class Engine {
public:
  Engine(); ~Engine();
  bool start(); void stop();
  void wait(HANDLE stop_event) const;
  [[nodiscard]] std::string handle_message(std::string_view line);
  static bool apply_nrpt(std::string_view extension);
  static bool remove_nrpt();
  static bool sync_hosts(const nlohmann::json& sites, std::string_view extension);
private:
  void configure_nginx();
  void rescan_sites_and_refresh_nginx();
  nlohmann::json service_list() const;
  ConfigStore config_store_; AppConfig config_; ServiceRegistry services_; SiteRegistry sites_; DnsServer dns_; PipeServer ipc_; std::atomic_bool running_{};
};
} // namespace appytizer
