#include "engine/engine.hpp"
#include "common/constants.hpp"
#include "common/win_handle.hpp"
#include <windows.h>
#include <winsvc.h>
#include <string_view>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace {
SERVICE_STATUS_HANDLE g_status_handle{};
SERVICE_STATUS g_status{};
appytizer::WinHandle g_stop_event;
appytizer::Engine* g_engine{};

void WINAPI control_handler(DWORD control) {
  if (control != SERVICE_CONTROL_STOP || !g_engine) return;
  g_status.dwCurrentState = SERVICE_STOP_PENDING;
  SetServiceStatus(g_status_handle, &g_status);
  SetEvent(g_stop_event.get());
}

void WINAPI service_main(DWORD, wchar_t**) {
  g_status_handle = RegisterServiceCtrlHandlerW(appytizer::kServiceName, control_handler);
  if (!g_status_handle) return;
  g_status = {};
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
  g_status.dwCurrentState = SERVICE_START_PENDING;
  SetServiceStatus(g_status_handle, &g_status);
  g_stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  appytizer::Engine engine;
  g_engine = &engine;
  if (engine.start()) {
    g_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_status_handle, &g_status);
    engine.wait(g_stop_event.get());
    engine.stop();
  }
  g_engine = nullptr;
  g_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(g_status_handle, &g_status);
}

bool install() {
  wchar_t path[32768]{};
  if (!GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)))) return false;
  appytizer::ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
  if (!manager) return false;
  appytizer::ServiceHandle service(CreateServiceW(manager.get(), appytizer::kServiceName,
      appytizer::kServiceDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
      SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, path, nullptr, nullptr, nullptr, nullptr, nullptr));
  if (!service && GetLastError() == ERROR_SERVICE_EXISTS) return true;
  if (!service) return false;
  SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}};
  SERVICE_FAILURE_ACTIONSW failure{60, nullptr, nullptr, 3, actions};
  ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
  return StartServiceW(service.get(), 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
}

bool uninstall() {
  appytizer::Engine::remove_nrpt();
  appytizer::Engine::sync_hosts(nlohmann::json::array(), "");
  appytizer::ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  appytizer::ServiceHandle service(manager ? OpenServiceW(manager.get(), appytizer::kServiceName,
      SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS) : nullptr);
  if (!service) return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
  SERVICE_STATUS status{};
  ControlService(service.get(), SERVICE_CONTROL_STOP, &status);
  return DeleteService(service.get()) != FALSE;
}

int run_console() {
  appytizer::Engine engine;
  if (!engine.start()) {
    fwprintf(stderr, L"Appytizer Engine failed to start. See the engine log for details.\n");
    return 1;
  }
  g_stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  g_engine = &engine;
  SetConsoleCtrlHandler([](DWORD) { SetEvent(g_stop_event.get()); return TRUE; }, TRUE);
  wprintf(L"Appytizer Engine is running in console mode. Press Ctrl+C to stop.\n");
  engine.wait(g_stop_event.get());
  engine.stop(); g_engine = nullptr;
  return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    const auto log = appytizer::ConfigStore::default_path().parent_path() / L"logs" / L"engine.log";
    std::filesystem::create_directories(log.parent_path());
    spdlog::set_default_logger(spdlog::rotating_logger_mt("engine", log.string(), 5 * 1024 * 1024, 3));
  } catch (...) {}
  if (argc > 1) {
    const std::wstring_view arg = argv[1];
    if (arg == L"--install-service") return install() ? 0 : 1;
    if (arg == L"--uninstall-service") return uninstall() ? 0 : 1;
    if (arg == L"--run-console") return run_console();
  }
  SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(appytizer::kServiceName), service_main}, {nullptr, nullptr}};
  if (StartServiceCtrlDispatcherW(table)) return 0;
  if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) return run_console();
  fwprintf(stderr, L"Could not connect to the Service Control Manager (error %lu).\n", GetLastError());
  return 1;
}
