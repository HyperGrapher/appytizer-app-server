#include "engine/services/providers.hpp"
#include "engine/services/detection_utils.hpp"
#include <psapi.h>
#include <winsvc.h>
#include <algorithm>
#include <regex>

namespace appytizer {
std::string parse_version(std::string_view text) {
  std::match_results<std::string_view::const_iterator> match;
  static const std::regex pattern(R"((\d+(?:\.\d+){1,3}))");
  return std::regex_search(text.begin(), text.end(), match, pattern) ? match[1].str() : std::string(text);
}

namespace {
std::string narrow(std::wstring_view value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}
std::wstring lower(std::wstring value) { std::ranges::transform(value, value.begin(), ::towlower); return value; }
std::size_t memory_for(DWORD pid) {
  WinHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid));
  PROCESS_MEMORY_COUNTERS_EX info{};
  return process && GetProcessMemoryInfo(process.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info)) ? info.WorkingSetSize : 0;
}
std::filesystem::path configured_root(const AppConfig& config, const std::string& id) {
  const auto found = config.service_roots.find(id); return found == config.service_roots.end() ? std::filesystem::path{} : found->second;
}
std::vector<std::filesystem::path> roots(std::initializer_list<std::filesystem::path> values) {
  std::vector<std::filesystem::path> result;
  for (const auto& value : values) if (!value.empty()) result.push_back(value);
  return result;
}

class Provider final : public IServiceProvider {
public:
  Provider(std::string id, std::string name, std::vector<std::filesystem::path> search_roots,
           std::vector<std::wstring> executable_names, std::vector<std::wstring> service_tokens,
           std::vector<std::wstring> registry_tokens, std::wstring arguments = {})
      : id_(std::move(id)), name_(std::move(name)), search_roots_(std::move(search_roots)),
        executable_names_(std::move(executable_names)), service_tokens_(std::move(service_tokens)),
        registry_tokens_(std::move(registry_tokens)), arguments_(std::move(arguments)) {}

  std::string id() const override { return id_; }
  std::string display_name() const override { return name_; }
  std::vector<InstalledVersion> detect() override {
    std::scoped_lock lock(mutex_); versions_.clear(); status_ = {};
    detect_services(); detect_executables();
    if (active_.empty() && !versions_.empty()) active_ = versions_.front().version_label;
    return versions_;
  }
  bool start(const std::string& version) override {
    std::scoped_lock lock(mutex_); const auto* selected = select(version);
    if (!selected) return false; active_ = selected->version_label;
    return selected->is_windows_service ? control_scm(*selected, true) : spawn(*selected);
  }
  bool stop() override {
    std::scoped_lock lock(mutex_);
    if (process_) { TerminateJobObject(job_.get(), 0); process_.reset(); job_.reset(); status_ = {}; return true; }
    const auto* selected = select(active_);
    if (!selected || !selected->is_windows_service) return !status_.running;
    return control_scm(*selected, false);
  }
  bool restart(const std::string& version) override { return stop() && start(version); }
  ServiceStatus status() const override {
    std::scoped_lock lock(mutex_); ServiceStatus result = status_; result.active_version = active_;
    result.working_set_bytes = 0; for (DWORD pid : result.process_ids) result.working_set_bytes += memory_for(pid);
    return result;
  }
  const std::vector<InstalledVersion>& versions() const override { return versions_; }
  void set_launch_arguments(std::wstring arguments) override {
    std::scoped_lock lock(mutex_); arguments_ = std::move(arguments);
  }

private:
  void detect_executables() {
    for (const auto& executable : find_installed_executables(executable_names_, search_roots_, registry_tokens_)) {
      if (std::ranges::none_of(versions_, [&](const auto& item) { return item.executable_path == executable; }))
        versions_.push_back({executable_version(executable), executable, false, {}});
    }
  }
  void detect_services() {
    if (service_tokens_.empty()) return;
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE)); if (!manager) return;
    DWORD bytes{}, count{}, resume{};
    EnumServicesStatusExW(manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &bytes, &count, &resume, nullptr);
    std::vector<BYTE> data(bytes);
    if (!EnumServicesStatusExW(manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, data.data(), bytes, &bytes, &count, &resume, nullptr)) return;
    const auto* services = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(data.data());
    for (DWORD i = 0; i < count; ++i) {
      const std::wstring candidate = services[i].lpServiceName, normalized = lower(candidate); bool matches = false;
      for (const auto& token : service_tokens_) if (normalized.find(lower(token)) != std::wstring::npos) matches = true;
      if (!matches) continue;
      const auto service_name = narrow(candidate); const auto version = parse_version(service_name);
      versions_.push_back({version, {}, true, service_name});
      if (services[i].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING) {
        active_ = version; status_.running = true;
        if (services[i].ServiceStatusProcess.dwProcessId) status_.process_ids = {services[i].ServiceStatusProcess.dwProcessId};
      }
    }
  }
  const InstalledVersion* select(const std::string& requested) const {
    if (versions_.empty()) return nullptr;
    const auto found = std::ranges::find_if(versions_, [&](const auto& value) { return requested.empty() || value.version_label == requested; });
    return found == versions_.end() ? &versions_.front() : &*found;
  }
  bool spawn(const InstalledVersion& version) {
    WinHandle job(CreateJobObjectW(nullptr, nullptr)); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
    std::wstring command = L"\"" + version.executable_path.wstring() + L"\" " + arguments_;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, version.executable_path.parent_path().c_str(), &startup, &info)) return false;
    WinHandle thread(info.hThread), process(info.hProcess);
    if (!AssignProcessToJobObject(job.get(), process.get())) { TerminateProcess(process.get(), 1); return false; }
    ResumeThread(thread.get());
    if (WaitForSingleObject(process.get(), 250) == WAIT_OBJECT_0) return false;
    status_.running = true; status_.process_ids = {info.dwProcessId};
    process_ = std::move(process); job_ = std::move(job); return true;
  }
  bool control_scm(const InstalledVersion& version, bool start_service) {
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    const std::wstring name(version.windows_service_name.begin(), version.windows_service_name.end());
    ServiceHandle service(manager ? OpenServiceW(manager.get(), name.c_str(), SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS) : nullptr);
    if (!service) return false;
    const bool changed = start_service ? StartServiceW(service.get(), 0, nullptr) != FALSE : [&] { SERVICE_STATUS value{}; return ControlService(service.get(), SERVICE_CONTROL_STOP, &value) != FALSE; }();
    const DWORD operation_error = GetLastError(); SERVICE_STATUS_PROCESS state{}; DWORD size{};
    QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&state), sizeof(state), &size);
    status_.running = state.dwCurrentState == SERVICE_RUNNING || state.dwCurrentState == SERVICE_START_PENDING;
    status_.process_ids = state.dwProcessId ? std::vector<DWORD>{state.dwProcessId} : std::vector<DWORD>{};
    return changed || operation_error == static_cast<DWORD>(start_service ? ERROR_SERVICE_ALREADY_RUNNING : ERROR_SERVICE_NOT_ACTIVE);
  }

  std::string id_, name_, active_;
  std::vector<std::filesystem::path> search_roots_;
  std::vector<std::wstring> executable_names_, service_tokens_, registry_tokens_;
  std::wstring arguments_;
  std::vector<InstalledVersion> versions_; mutable std::mutex mutex_;
  WinHandle job_, process_; ServiceStatus status_;
};
} // namespace

void register_builtin_providers(ServiceRegistry& registry, const AppConfig& config) {
  const auto program_files = environment_path(L"ProgramFiles");
  const auto local = environment_path(L"LOCALAPPDATA");
  const auto chocolatey = environment_path(L"ChocolateyInstall");
  registry.add(std::make_unique<Provider>("nginx", "nginx",
      roots({configured_root(config, "nginx"), L"C:\\nginx", L"C:\\tools\\nginx", program_files / L"nginx", chocolatey / L"bin"}),
      std::vector<std::wstring>{L"nginx.exe"}, std::vector<std::wstring>{}, std::vector<std::wstring>{L"nginx"}));
  registry.add(std::make_unique<Provider>("php", "PHP",
      roots({configured_root(config, "php"), L"C:\\php", L"C:\\tools\\php", program_files / L"PHP", local / L"Programs\\PHP", local / L"scoop\\apps\\php"}),
      std::vector<std::wstring>{L"php-cgi.exe"}, std::vector<std::wstring>{}, std::vector<std::wstring>{L"PHP"}, L"-b 127.0.0.1:9000"));
  registry.add(std::make_unique<Provider>("mysql", "MySQL",
      roots({configured_root(config, "mysql"), program_files / L"MySQL", program_files / L"MariaDB"}),
      std::vector<std::wstring>{L"mysqld.exe"}, std::vector<std::wstring>{L"mysql", L"mariadb"}, std::vector<std::wstring>{L"MySQL", L"MariaDB"}));
  registry.add(std::make_unique<Provider>("postgres", "PostgreSQL",
      roots({configured_root(config, "postgres"), program_files / L"PostgreSQL"}),
      std::vector<std::wstring>{L"postgres.exe"}, std::vector<std::wstring>{L"postgresql"}, std::vector<std::wstring>{L"PostgreSQL"}));
  registry.add(std::make_unique<Provider>("mongodb", "MongoDB",
      roots({configured_root(config, "mongodb"), program_files / L"MongoDB"}),
      std::vector<std::wstring>{L"mongod.exe"}, std::vector<std::wstring>{L"mongodb"}, std::vector<std::wstring>{L"MongoDB"}));
}
} // namespace appytizer
