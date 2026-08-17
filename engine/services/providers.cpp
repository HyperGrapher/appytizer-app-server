#include "engine/services/providers.hpp"
#include "engine/services/detection_utils.hpp"
#include <psapi.h>
#include <winsvc.h>
#include <algorithm>
#include <array>
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
std::vector<std::filesystem::path> profile_roots(std::wstring_view relative_path) {
  std::vector<std::filesystem::path> result;
  std::error_code error;
  for (const auto& profile : std::filesystem::directory_iterator(L"C:\\Users", error)) {
    if (error) break;
    if (!profile.is_directory(error) || error) {
      error.clear();
      continue;
    }
    result.push_back(profile.path() / relative_path);
  }
  return result;
}
std::filesystem::path bundled_runtime_directory() {
  std::array<wchar_t, 32768> executable{};
  const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (length == 0 || length == executable.size()) {
    return {};
  }
  return std::filesystem::path(executable.data()).parent_path() / L"runtime";
}
bool is_within(const std::filesystem::path& candidate, const std::filesystem::path& root) {
  if (root.empty()) {
    return false;
  }
  std::error_code error;
  const auto normalized_candidate = std::filesystem::weakly_canonical(candidate, error);
  if (error) {
    return false;
  }
  const auto normalized_root = std::filesystem::weakly_canonical(root, error);
  if (error) {
    return false;
  }
  const auto relative = normalized_candidate.lexically_relative(normalized_root);
  return !error && !relative.empty() && !relative.native().starts_with(L"..");
}

class Provider final : public IServiceProvider {
public:
  Provider(std::string id, std::string name, std::vector<std::filesystem::path> search_roots,
           std::vector<std::wstring> executable_names, std::vector<std::wstring> service_tokens,
           std::vector<std::wstring> registry_tokens, std::wstring arguments = {},
           std::filesystem::path preferred_root = {}, std::wstring preferred_arguments = {})
      : id_(std::move(id)), name_(std::move(name)), search_roots_(std::move(search_roots)),
        executable_names_(std::move(executable_names)), service_tokens_(std::move(service_tokens)),
        registry_tokens_(std::move(registry_tokens)), arguments_(std::move(arguments)),
        preferred_root_(std::move(preferred_root)), preferred_arguments_(std::move(preferred_arguments)) {}

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
    return selected->is_windows_service ? control_scm(*selected, true) : spawn(*selected, launch_arguments_for(*selected));
  }
  bool stop() override {
    std::scoped_lock lock(mutex_);
    if (process_) {
      if (!TerminateJobObject(job_.get(), 0)) {
        return false;
      }
      const DWORD wait_result = WaitForSingleObject(process_.get(), 5000);
      if (wait_result != WAIT_OBJECT_0) {
        return false;
      }
      process_.reset();
      job_.reset();
      status_ = {};
      return true;
    }
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
    auto executables = find_installed_executables(executable_names_, search_roots_, registry_tokens_);
    std::ranges::stable_sort(executables, [this](const auto& left, const auto& right) {
      return is_within(left, preferred_root_) && !is_within(right, preferred_root_);
    });
    for (const auto& executable : executables) {
      const auto version = executable_version(executable);
      if (std::ranges::none_of(versions_, [&](const auto& item) { return item.version_label == version; })) {
        versions_.push_back({version, executable, false, {}});
      }
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
  std::wstring launch_arguments_for(const InstalledVersion& version) const {
    return is_within(version.executable_path, preferred_root_) && !preferred_arguments_.empty()
               ? preferred_arguments_
               : arguments_;
  }
  bool spawn(const InstalledVersion& version, const std::wstring& arguments) {
    WinHandle job(CreateJobObjectW(nullptr, nullptr)); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
    std::wstring command = L"\"" + version.executable_path.wstring() + L"\" " + arguments;
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
  std::filesystem::path preferred_root_;
  std::wstring preferred_arguments_;
  std::vector<InstalledVersion> versions_; mutable std::mutex mutex_;
  WinHandle job_, process_; ServiceStatus status_;
};
} // namespace

void register_builtin_providers(ServiceRegistry& registry, const AppConfig& config) {
  const auto program_files = environment_path(L"ProgramFiles");
  const auto local = environment_path(L"LOCALAPPDATA");
  const auto bundled_runtime = bundled_runtime_directory();
  const auto bundled_nginx_root = bundled_runtime;
  const auto bundled_php_root = bundled_runtime / L"php";
  const auto php_ini = ConfigStore::default_path().parent_path() / L"php" / L"php.ini";
  const auto user_winget_roots = profile_roots(L"AppData\\Local\\Microsoft\\WinGet\\Packages");
  const auto user_program_roots = profile_roots(L"AppData\\Local\\Programs");
  auto nginx_roots = roots({bundled_nginx_root});
  auto php_roots = roots({configured_root(config, "php"), bundled_php_root, L"C:\\php", L"C:\\tools\\php",
                          program_files / L"PHP", local / L"Programs\\PHP", local / L"scoop\\apps\\php"});
  php_roots.insert(php_roots.end(), user_winget_roots.begin(), user_winget_roots.end());
  php_roots.insert(php_roots.end(), user_program_roots.begin(), user_program_roots.end());
  registry.add(std::make_unique<Provider>("nginx", "nginx",
      std::move(nginx_roots),
      std::vector<std::wstring>{L"nginx.exe"}, std::vector<std::wstring>{}, std::vector<std::wstring>{},
      L"", bundled_nginx_root));
  registry.add(std::make_unique<Provider>("php", "PHP",
      std::move(php_roots),
      std::vector<std::wstring>{L"php-cgi.exe"}, std::vector<std::wstring>{}, std::vector<std::wstring>{L"PHP"},
      L"-b 127.0.0.1:9000", bundled_php_root,
      L"-c \"" + php_ini.wstring() + L"\" -b 127.0.0.1:9000"));
  registry.add(std::make_unique<Provider>("mysql", "MySQL",
      roots({configured_root(config, "mysql"), program_files / L"MySQL", program_files / L"MariaDB"}),
      std::vector<std::wstring>{L"mysqld.exe"}, std::vector<std::wstring>{L"mysql", L"mariadb"}, std::vector<std::wstring>{L"MySQL", L"MariaDB"}));
  registry.add(std::make_unique<Provider>("postgres", "PostgreSQL",
      roots({configured_root(config, "postgres"), program_files / L"PostgreSQL"}),
      std::vector<std::wstring>{L"postgres.exe"}, std::vector<std::wstring>{L"postgresql"}, std::vector<std::wstring>{L"PostgreSQL"}));
}
} // namespace appytizer
