#pragma once
#include "common/config.hpp"
#include "common/service_provider.hpp"
#include "engine/dns/dns_server.hpp"
#include "engine/ipc/pipe_server.hpp"
#include "engine/sites/site_registry.hpp"
#include "engine/sites/folder_watcher.hpp"
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
  static bool sync_hosts(const nlohmann::json& sites, std::string_view extension);
private:
  void configure_nginx();
  void start_default_services();
  [[nodiscard]] bool rescan_sites_and_refresh_nginx();
  void publish_status();
  void publish_sites_changed();
  nlohmann::json service_list() const;
  ConfigStore config_store_; AppConfig config_; ServiceRegistry services_; SiteRegistry sites_; FolderWatcher watcher_; DnsServer dns_; PipeServer ipc_; std::atomic_bool running_{};
};
} // namespace appytizer
