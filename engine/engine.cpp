#include "engine/engine.hpp"
#include "common/ipc_protocol.hpp"
#include "engine/services/providers.hpp"
#include <windows.h>
#include <fstream>
#include <spdlog/spdlog.h>

extern "C" BOOL WINAPI DnsFlushResolverCache();

namespace appytizer {
namespace {
constexpr wchar_t kNrptKey[] = L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\DNSClient\\DnsPolicyConfig\\Appytizer";
constexpr char kHostsTag[] = "# Appytizer";
}

Engine::Engine() : config_(config_store_.load()) { register_builtin_providers(services_, config_); }
Engine::~Engine() { stop(); }

void Engine::configure_nginx() {
  auto* nginx = services_.get_by_id("nginx");
  if (!nginx || nginx->versions().empty()) return;
  const auto executable = nginx->versions().front().executable_path;
  if (executable.empty()) return;
  const auto nginx_data = config_store_.default_path().parent_path() / L"nginx";
  const auto runtime = nginx_data / L"runtime";
  const auto root_config = runtime / L"conf" / L"nginx.conf";
  if (!sites_.write_nginx_configs(nginx_data / L"sites", config_.extension, 9000,
      executable.parent_path() / L"conf" / L"fastcgi_params") ||
      !sites_.write_nginx_root_config(root_config, executable.parent_path())) {
    spdlog::error("Could not write Appytizer nginx configuration.");
    return;
  }
  nginx->set_launch_arguments(L"-p \"" + runtime.wstring() + L"\" -c \"" + root_config.wstring() + L"\"");
}

void Engine::rescan_sites_and_refresh_nginx() {
  sites_.rescan(config_.root_folder);
  sync_hosts(sites_.list(), config_.extension);
  auto* nginx = services_.get_by_id("nginx");
  const auto was_running = nginx ? nginx->status() : ServiceStatus{};
  configure_nginx();
  if (nginx && was_running.running) nginx->restart(was_running.active_version);
}

void Engine::publish_status() {
  ipc_.broadcast(nlohmann::json{{"event", "status.update"}, {"services", service_list()}}.dump());
}

void Engine::publish_sites_changed() {
  ipc_.broadcast(nlohmann::json{{"event", "sites.changed"}, {"sites", sites_.list()}}.dump());
}

bool Engine::start() {
  if (running_) return true;
  services_.detect_all();
  rescan_sites_and_refresh_nginx();
  apply_nrpt(config_.extension);
  if (!dns_.start(config_.extension)) spdlog::warn("DNS server could not bind 127.0.0.1:53");
  ipc_.start([this](std::string_view line) { return handle_message(line); });
  running_ = true;
  watcher_.start(config_.root_folder, [this] {
    if (!running_) return;
    rescan_sites_and_refresh_nginx();
    publish_sites_changed();
  });
  publish_status();
  return true;
}

void Engine::stop() {
  if (!running_.exchange(false)) return;
  watcher_.stop();
  ipc_.stop(); dns_.stop();
  for (const auto& provider : services_.all()) provider->stop();
}
void Engine::wait(HANDLE event) const { WaitForSingleObject(event, INFINITE); }

nlohmann::json Engine::service_list() const {
  nlohmann::json result = nlohmann::json::array();
  result.push_back({{"id", "dns"}, {"name", "Local DNS"}, {"running", dns_.running()}, {"version", config_.extension}, {"ram_mb", 0}, {"available_versions", nlohmann::json::array()}, {"installations", nlohmann::json::array()}});
  for (const auto& provider : services_.all()) {
    const auto status = provider->status(); nlohmann::json versions = nlohmann::json::array(), installations = nlohmann::json::array();
    for (const auto& version : provider->versions()) {
      versions.push_back(version.version_label);
      installations.push_back({{"version", version.version_label}, {"path", version.executable_path.empty() ? "" : version.executable_path.string()}, {"windows_service", version.is_windows_service}, {"service_name", version.windows_service_name}});
    }
    result.push_back({{"id", provider->id()}, {"name", provider->display_name()}, {"running", status.running}, {"version", status.active_version}, {"ram_mb", status.working_set_bytes / (1024 * 1024)}, {"available_versions", versions}, {"installations", installations}});
  }
  return result;
}

std::string Engine::handle_message(std::string_view line) {
  const auto request = parse_request(line);
  if (!request) return response_error("", "invalid request");
  try {
    const auto& command = request->command;
    if (command == "events.subscribe") return response_ok(request->id);
    if (command == "service.list") return response_ok(request->id, service_list());
    if (command == "sites.list") return response_ok(request->id, sites_.list());
    if (command == "config.get") return response_ok(request->id, {{"root_folder", config_.root_folder.string()}, {"extension", config_.extension}, {"run_minimized", config_.run_minimized}, {"autostart", config_.autostart}});
    if (command == "sites.rescan") { rescan_sites_and_refresh_nginx(); publish_sites_changed(); return response_ok(request->id, sites_.list()); }
    if (command == "service.rescan") { services_.detect_all(); configure_nginx(); publish_status(); return response_ok(request->id, service_list()); }
    if (command == "dns.start") { const bool ok = dns_.start(config_.extension); if (ok) publish_status(); return ok ? response_ok(request->id, service_list()) : response_error(request->id, "DNS start failed"); }
    if (command == "dns.stop") { dns_.stop(); publish_status(); return response_ok(request->id, service_list()); }
    if (command == "dns.restart") { dns_.stop(); const bool ok = dns_.start(config_.extension); if (ok) publish_status(); return ok ? response_ok(request->id, service_list()) : response_error(request->id, "DNS restart failed"); }
    if (command == "stop_all") { dns_.stop(); for (const auto& provider : services_.all()) provider->stop(); publish_status(); return response_ok(request->id, service_list()); }
    if (command.starts_with("service.")) {
      const auto id = request->params.value("service", ""); auto* provider = services_.get_by_id(id);
      if (!provider) return response_error(request->id, "unknown service");
      const auto version = request->params.value("version", ""); bool ok = false;
      if (command == "service.start") ok = provider->start(version);
      else if (command == "service.stop") ok = provider->stop();
      else if (command == "service.restart" || command == "service.set_version") ok = provider->restart(version);
      if (ok) publish_status();
      return ok ? response_ok(request->id, service_list()) : response_error(request->id, "service operation failed");
    }
    if (command == "config.set") {
      if (request->params.contains("root_folder")) config_.root_folder = request->params["root_folder"].get<std::string>();
      if (request->params.contains("extension")) {
        remove_nrpt(); dns_.stop(); config_.extension = request->params["extension"].get<std::string>();
        if (config_.extension.empty() || config_.extension.front() != '.') config_.extension.insert(config_.extension.begin(), '.');
        apply_nrpt(config_.extension); dns_.start(config_.extension);
      }
      if (request->params.contains("run_minimized")) config_.run_minimized = request->params["run_minimized"].get<bool>();
      if (request->params.contains("autostart")) config_.autostart = request->params["autostart"].get<bool>();
      if (!config_store_.save(config_)) spdlog::warn("Could not save Appytizer configuration.");
      rescan_sites_and_refresh_nginx();
      watcher_.start(config_.root_folder, [this] {
        if (!running_) return;
        rescan_sites_and_refresh_nginx();
        publish_sites_changed();
      });
      publish_status(); publish_sites_changed(); return response_ok(request->id);
    }
    return response_error(request->id, "unknown command");
  } catch (const std::exception& error) {
    spdlog::error("IPC command failed: {}", error.what()); return response_error(request->id, error.what());
  }
}

bool Engine::apply_nrpt(std::string_view extension) {
  HKEY key{};
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kNrptKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
  const std::wstring name(extension.begin(), extension.end()), servers = L"127.0.0.1"; DWORD options = 0;
  RegSetValueExW(key, L"Name", 0, REG_SZ, reinterpret_cast<const BYTE*>(name.c_str()), static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"GenericDNSServers", 0, REG_SZ, reinterpret_cast<const BYTE*>(servers.c_str()), static_cast<DWORD>((servers.size() + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"ConfigOptions", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&options), sizeof(options));
  RegCloseKey(key); DnsFlushResolverCache(); return true;
}
bool Engine::remove_nrpt() { const LONG result = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kNrptKey); DnsFlushResolverCache(); return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND; }
bool Engine::sync_hosts(const nlohmann::json& sites, std::string_view extension) {
  wchar_t windows[MAX_PATH]{}; if (!GetWindowsDirectoryW(windows, MAX_PATH)) return false;
  const auto path = std::filesystem::path(windows) / L"System32" / L"drivers" / L"etc" / L"hosts"; std::ifstream input(path); if (!input) return false;
  std::vector<std::string> lines; for (std::string entry; std::getline(input, entry);) if (entry.find(kHostsTag) == std::string::npos) lines.push_back(std::move(entry)); input.close();
  for (const auto& site : sites) lines.push_back("127.0.0.1\t" + site["name"].get<std::string>() + std::string(extension) + "\t" + kHostsTag);
  std::ofstream output(path, std::ios::trunc); if (!output) return false; for (const auto& entry : lines) output << entry << '\n'; output.close(); DnsFlushResolverCache(); return true;
}
} // namespace appytizer
