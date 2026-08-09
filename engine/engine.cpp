#include "engine/engine.hpp"

#include "common/constants.hpp"
#include "common/ipc_protocol.hpp"
#include "common/win_handle.hpp"
#include "engine/services/providers.hpp"

#include <windows.h>

#include <fstream>
#include <set>
#include <spdlog/spdlog.h>

extern "C" BOOL WINAPI DnsFlushResolverCache();

namespace appytizer {
namespace {
constexpr char kHostsTag[] = "# Appytizer";

bool run_nginx_validation(const std::filesystem::path& executable,
                          const std::filesystem::path& prefix,
                          const std::filesystem::path& configuration) {
  std::wstring command = L"\"" + executable.wstring() + L"\" -t -p \"" + prefix.wstring() +
                         L"\" -c \"" + configuration.wstring() + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process_info{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      executable.parent_path().c_str(), &startup, &process_info)) {
    return false;
  }
  WinHandle process(process_info.hProcess);
  WinHandle thread(process_info.hThread);
  if (WaitForSingleObject(process.get(), 30000) != WAIT_OBJECT_0) {
    TerminateProcess(process.get(), 1);
    return false;
  }
  DWORD exit_code{};
  return GetExitCodeProcess(process.get(), &exit_code) && exit_code == 0;
}

bool activate_staging_directory(const std::filesystem::path& staging,
                                const std::filesystem::path& active) {
  const auto backup = active.wstring() + L".previous";
  std::error_code error;
  std::filesystem::remove_all(backup, error);
  error.clear();
  const bool had_active = std::filesystem::exists(active, error);
  if (error) {
    return false;
  }
  if (had_active) {
    std::filesystem::rename(active, backup, error);
    if (error) {
      return false;
    }
  }
  std::filesystem::rename(staging, active, error);
  if (error) {
    if (had_active) {
      std::error_code restore_error;
      std::filesystem::rename(backup, active, restore_error);
    }
    return false;
  }
  std::filesystem::remove_all(backup, error);
  return true;
}
} // namespace

Engine::Engine() : config_(config_store_.load()) {
  register_builtin_providers(services_, config_);
}

Engine::~Engine() {
  stop();
}

bool Engine::configure_nginx() {
  const auto sites = sites_.list();
  std::vector<std::string> active_hostnames;
  for (const auto& site : sites) {
    if (site.value("valid", false)) {
      active_hostnames.push_back(site.at("hostname").get<std::string>());
    }
  }

  auto* nginx = services_.get_by_id("nginx");
  const auto executable = nginx && !nginx->versions().empty()
                              ? nginx->versions().front().executable_path
                              : std::filesystem::path{};
  const auto nginx_data = config_store_.default_path().parent_path() / L"nginx";
  const auto active = nginx_data / L"active";
  const auto active_config = active / L"runtime" / L"conf" / L"nginx.conf";
  if (nginx && !executable.empty() && std::filesystem::exists(active_config)) {
    nginx->set_launch_arguments(L"-p \"" + (active / L"runtime").wstring() + L"\" -c \"" +
                                active_config.wstring() + L"\"");
  }

  if (config_.https_enabled) {
    const auto tls_status = certificates_.status();
    if (!tls_status.ready) {
      spdlog::error("HTTPS configuration was not published: {}", tls_status.error);
      return false;
    }
    for (const auto& hostname : active_hostnames) {
      if (!certificates_.ensure_site_certificate(hostname)) {
        spdlog::error("HTTPS configuration was not published: certificate issuance failed for {}.", hostname);
        return false;
      }
    }
  }

  if (!nginx || executable.empty()) {
    spdlog::info("nginx is not detected; generated site state will be applied when nginx is available.");
    return true;
  }
  const auto staging = nginx_data / L"staging";
  std::error_code error;
  std::filesystem::remove_all(staging, error);
  error.clear();
  const auto root_config = staging / L"runtime" / L"conf" / L"nginx.conf";
  if (!sites_.write_nginx_configs(staging / L"sites", config_.https_enabled,
                                  certificates_.directory(), 9000,
                                  executable.parent_path() / L"conf" / L"fastcgi_params") ||
      !sites_.write_nginx_root_config(root_config, executable.parent_path(), config_.https_enabled)) {
    spdlog::error("Could not generate the staged Appytizer nginx configuration.");
    std::filesystem::remove_all(staging, error);
    return false;
  }
  if (!run_nginx_validation(executable, staging / L"runtime", root_config)) {
    spdlog::error("nginx -t rejected the staged Appytizer configuration; the active configuration was retained.");
    std::filesystem::remove_all(staging, error);
    return false;
  }
  if (!activate_staging_directory(staging, active)) {
    spdlog::error("Could not atomically activate the validated Appytizer nginx configuration.");
    return false;
  }
  nginx->set_launch_arguments(L"-p \"" + (active / L"runtime").wstring() + L"\" -c \"" +
                              active_config.wstring() + L"\"");
  if (!certificates_.remove_unused_site_certificates(active_hostnames)) {
    spdlog::warn("The nginx configuration was activated, but obsolete site certificates could not be removed.");
  }
  return true;
}

void Engine::start_default_services() {
  // PHP must be listening before nginx starts serving PHP requests.
  for (const std::string_view id : {"php", "nginx"}) {
    auto* provider = services_.get_by_id(std::string(id));
    if (!provider || provider->versions().empty()) {
      spdlog::info("Default service {} is not detected; leaving it stopped.", id);
      continue;
    }
    if (provider->status().running) {
      continue;
    }
    if (!provider->start("")) {
      spdlog::warn("Could not start default service {}.", id);
    } else {
      spdlog::info("Started default service {}.", id);
    }
  }
}

bool Engine::rescan_sites_and_refresh_nginx() {
  std::scoped_lock lock(refresh_mutex_);
  if (!sites_.rescan(config_.root_folder)) {
    spdlog::error("Could not scan the configured projects root.");
    return false;
  }
  auto* nginx = services_.get_by_id("nginx");
  const auto was_running = nginx ? nginx->status() : ServiceStatus{};
  if (!configure_nginx()) {
    return false;
  }
  if (!sync_hosts(sites_.list())) {
    spdlog::error("Could not update the Appytizer hosts entries. The Engine must run elevated.");
    return false;
  }
  if (nginx && was_running.running && !nginx->restart(was_running.active_version)) {
    spdlog::error("The validated nginx configuration was activated, but nginx could not be restarted.");
    return false;
  }
  return true;
}

void Engine::publish_status() {
  ipc_.broadcast(nlohmann::json{{"event", "status.update"}, {"services", service_list()}}.dump());
}

void Engine::publish_sites_changed() {
  ipc_.broadcast(nlohmann::json{{"event", "sites.changed"}, {"sites", sites_.list()}}.dump());
}

bool Engine::start() {
  if (running_) {
    return true;
  }
  services_.detect_all();
  if (!rescan_sites_and_refresh_nginx()) {
    spdlog::warn("The Engine started with its last working site configuration. Repair TLS or review nginx diagnostics.");
  }
  start_default_services();
  if (!dns_.start()) {
    spdlog::warn("DNS server could not bind 127.0.0.1:53");
  }
  ipc_.start([this](std::string_view line) { return handle_message(line); });
  running_ = true;
  watcher_.start(config_.root_folder, [this] {
    if (!running_ || !rescan_sites_and_refresh_nginx()) {
      return;
    }
    publish_sites_changed();
  });
  publish_status();
  return true;
}

void Engine::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  watcher_.stop();
  ipc_.stop();
  dns_.stop();
  for (const auto& provider : services_.all()) {
    provider->stop();
  }
}

void Engine::wait(HANDLE event) const {
  WaitForSingleObject(event, INFINITE);
}

nlohmann::json Engine::service_list() const {
  nlohmann::json result = nlohmann::json::array();
  result.push_back({{"id", "dns"},
                    {"name", "Local DNS"},
                    {"running", dns_.running()},
                    {"version", kSiteSuffix},
                    {"ram_mb", 0},
                    {"available_versions", nlohmann::json::array()},
                    {"installations", nlohmann::json::array()}});
  for (const auto& provider : services_.all()) {
    const auto status = provider->status();
    nlohmann::json versions = nlohmann::json::array();
    nlohmann::json installations = nlohmann::json::array();
    for (const auto& version : provider->versions()) {
      versions.push_back(version.version_label);
      installations.push_back({{"version", version.version_label},
                               {"path", version.executable_path.empty() ? "" : version.executable_path.string()},
                               {"windows_service", version.is_windows_service},
                               {"service_name", version.windows_service_name}});
    }
    result.push_back({{"id", provider->id()},
                      {"name", provider->display_name()},
                      {"running", status.running},
                      {"version", status.active_version},
                      {"ram_mb", status.working_set_bytes / (1024 * 1024)},
                      {"available_versions", versions},
                      {"installations", installations}});
  }
  return result;
}

std::string Engine::handle_message(std::string_view line) {
  const auto request = parse_request(line);
  if (!request) {
    return response_error("", "invalid request");
  }
  try {
    const auto& command = request->command;
    if (command == "events.subscribe") {
      return response_ok(request->id);
    }
    if (command == "service.list") {
      return response_ok(request->id, service_list());
    }
    if (command == "sites.list") {
      return response_ok(request->id, sites_.list());
    }
    if (command == "tls.status") {
      return response_ok(request->id, certificates_.status());
    }
    if (command == "config.get") {
      return response_ok(request->id,
                         {{"root_folder", config_.root_folder.string()},
                          {"https_enabled", config_.https_enabled},
                          {"run_minimized", config_.run_minimized},
                          {"autostart", config_.autostart}});
    }
    if (command == "sites.rescan") {
      if (!rescan_sites_and_refresh_nginx()) {
        return response_error(request->id,
                              "Could not activate the site configuration; check TLS status and nginx diagnostics");
      }
      publish_sites_changed();
      return response_ok(request->id, sites_.list());
    }
    if (command == "service.rescan") {
      services_.detect_all();
      if (!rescan_sites_and_refresh_nginx()) {
        return response_error(request->id, "Detected services, but the nginx configuration was not activated");
      }
      publish_status();
      return response_ok(request->id, service_list());
    }
    if (command == "dns.start") {
      const bool ok = dns_.start();
      if (ok) {
        publish_status();
      }
      return ok ? response_ok(request->id, service_list()) : response_error(request->id, "DNS start failed");
    }
    if (command == "dns.stop") {
      dns_.stop();
      publish_status();
      return response_ok(request->id, service_list());
    }
    if (command == "dns.restart") {
      dns_.stop();
      const bool ok = dns_.start();
      if (ok) {
        publish_status();
      }
      return ok ? response_ok(request->id, service_list()) : response_error(request->id, "DNS restart failed");
    }
    if (command == "stop_all") {
      dns_.stop();
      for (const auto& provider : services_.all()) {
        provider->stop();
      }
      publish_status();
      return response_ok(request->id, service_list());
    }
    if (command.starts_with("service.")) {
      const auto id = request->params.value("service", "");
      auto* provider = services_.get_by_id(id);
      if (!provider) {
        return response_error(request->id, "unknown service");
      }
      const auto version = request->params.value("version", "");
      bool ok = false;
      if (command == "service.start") {
        ok = provider->start(version);
      } else if (command == "service.stop") {
        ok = provider->stop();
      } else if (command == "service.restart" || command == "service.set_version") {
        ok = provider->restart(version);
      }
      if (ok) {
        publish_status();
      }
      return ok ? response_ok(request->id, service_list())
                : response_error(request->id, "service operation failed");
    }
    if (command == "config.set") {
      const AppConfig previous_config = config_;
      if (request->params.contains("root_folder")) {
        config_.root_folder = request->params.at("root_folder").get<std::string>();
      }
      if (request->params.contains("https_enabled")) {
        config_.https_enabled = request->params.at("https_enabled").get<bool>();
      }
      if (request->params.contains("run_minimized")) {
        config_.run_minimized = request->params.at("run_minimized").get<bool>();
      }
      if (request->params.contains("autostart")) {
        config_.autostart = request->params.at("autostart").get<bool>();
      }
      if (!rescan_sites_and_refresh_nginx()) {
        config_ = previous_config;
        sites_.rescan(config_.root_folder);
        return response_error(request->id,
                              "Could not activate settings; check TLS status and nginx diagnostics");
      }
      if (!config_store_.save(config_)) {
        config_ = previous_config;
        if (!rescan_sites_and_refresh_nginx()) {
          spdlog::error("Could not restore the previous Appytizer configuration after a save failure.");
        }
        return response_error(request->id, "Could not save Appytizer configuration");
      }
      watcher_.start(config_.root_folder, [this] {
        if (!running_ || !rescan_sites_and_refresh_nginx()) {
          return;
        }
        publish_sites_changed();
      });
      publish_status();
      publish_sites_changed();
      return response_ok(request->id);
    }
    return response_error(request->id, "unknown command");
  } catch (const std::exception& error) {
    spdlog::error("IPC command failed: {}", error.what());
    return response_error(request->id, error.what());
  }
}

bool Engine::sync_hosts(const nlohmann::json& sites) {
  std::filesystem::path path;
  bool is_test_override = false;
  if (const DWORD size = GetEnvironmentVariableW(L"APPYTIZER_HOSTS_FILE", nullptr, 0); size > 0) {
    std::wstring value(size, L'\0');
    GetEnvironmentVariableW(L"APPYTIZER_HOSTS_FILE", value.data(), size);
    value.resize(wcslen(value.c_str()));
    path = value;
    is_test_override = true;
  } else {
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows, MAX_PATH)) {
      return false;
    }
    path = std::filesystem::path(windows) / L"System32" / L"drivers" / L"etc" / L"hosts";
  }
  std::ifstream input(path);
  if (!input) {
    return false;
  }
  std::vector<std::string> lines;
  for (std::string entry; std::getline(input, entry);) {
    if (entry.find(kHostsTag) == std::string::npos) {
      lines.push_back(std::move(entry));
    }
  }
  input.close();
  for (const auto& site : sites) {
    if (site.value("valid", false)) {
      lines.push_back("127.0.0.1\t" + site.at("hostname").get<std::string>() + "\t" + kHostsTag);
    }
  }
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    return false;
  }
  for (const auto& entry : lines) {
    output << entry << '\n';
  }
  output.close();
  if (!is_test_override) {
    DnsFlushResolverCache();
  }
  return true;
}

} // namespace appytizer
