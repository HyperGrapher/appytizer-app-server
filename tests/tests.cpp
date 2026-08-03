#include <catch2/catch_test_macros.hpp>

#include "common/ipc_protocol.hpp"
#include "common/config.hpp"
#include "common/win_handle.hpp"
#include "engine/dns/dns_server.hpp"
#include "engine/ipc/pipe_server.hpp"
#include "engine/services/detection_utils.hpp"
#include "engine/services/providers.hpp"
#include "engine/sites/folder_watcher.hpp"

#include <windows.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using appytizer::WinHandle;

WinHandle connect_pipe(const std::wstring& name) {
  // The server creates its first instance on a worker thread, so allow that
  // short startup window before asking Win32 to wait for the instance.
  for (int attempt = 0; attempt != 300; ++attempt) {
    if (WaitNamedPipeW(name.c_str(), 100)) {
      HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
      if (pipe != INVALID_HANDLE_VALUE) return WinHandle(pipe);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return {};
}

bool write_text(HANDLE pipe, std::string_view text) {
  DWORD written{};
  return WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
}

std::string read_text(HANDLE pipe) {
  std::array<char, 65536> buffer{};
  DWORD read{};
  if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return {};
  return {buffer.data(), read};
}

std::wstring test_pipe_name() {
  return L"\\\\.\\pipe\\AppytizerTests-" + std::to_wstring(GetCurrentProcessId());
}
}

TEST_CASE("IPC requests are validated and responses are newline delimited") {
  const auto request = appytizer::parse_request(R"({"id":"7","cmd":"sites.list"})");
  REQUIRE(request);
  REQUIRE(request->id == "7");
  REQUIRE(request->command == "sites.list");
  REQUIRE_FALSE(appytizer::parse_request("not-json"));
  REQUIRE_FALSE(appytizer::parse_request(R"({"id":"7"})"));
  REQUIRE(appytizer::response_ok("7").ends_with("\n"));
}

TEST_CASE("Configuration round-trips through JSON with normalized extension") {
  const auto root = std::filesystem::temp_directory_path() /
                    ("appytizer-config-" + std::to_string(GetCurrentProcessId()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  appytizer::ConfigStore store(root / "config.json");
  appytizer::AppConfig original;
  original.root_folder = root / "sites";
  original.extension = "dev";
  original.run_minimized = true;
  original.autostart = true;
  original.active_versions["php"] = "8.4.7";
  original.service_roots["nginx"] = root / "nginx";
  REQUIRE(store.save(original));
  const auto loaded = store.load();
  REQUIRE(loaded.root_folder == original.root_folder);
  REQUIRE(loaded.extension == ".dev");
  REQUIRE(loaded.run_minimized);
  REQUIRE(loaded.autostart);
  REQUIRE(loaded.active_versions.at("php") == "8.4.7");
  REQUIRE(loaded.service_roots.at("nginx") == root / "nginx");
  std::filesystem::remove_all(root, error);
}

TEST_CASE("Service versions and executable detection are stable") {
  REQUIRE(appytizer::parse_version("PHP 8.4.12 (cgi-fcgi)") == "8.4.12");
  REQUIRE(appytizer::parse_version("nginx/1.27.0") == "1.27.0");

  const auto root = std::filesystem::temp_directory_path() /
                    ("appytizer-detection-" + std::to_string(GetCurrentProcessId()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root / "nested", error);
  REQUIRE_FALSE(error);
  { std::ofstream(root / "nested" / "php.exe") << "test"; }
  const auto found = appytizer::find_installed_executables({L"php.exe"}, {root}, {});
  REQUIRE(std::ranges::any_of(found, [&](const auto& path) { return path.parent_path() == root / "nested"; }));
  std::filesystem::remove_all(root, error);
}

TEST_CASE("DNS responds only to local A records") {
  const std::array<std::uint8_t, 27> query{0x12,0x34,0x01,0,0,1,0,0,0,0,0,0,3,'a','p','p',5,'l','o','c','a','l',0,0,1,0,1};
  const auto response = appytizer::DnsServer::make_response(query, ".local");
  REQUIRE(response.size() == query.size() + 16);
  REQUIRE(response[7] == 1);
  REQUIRE(response.back() == 1);

  auto aaaa_query = query;
  aaaa_query[24] = 28;
  const auto aaaa_response = appytizer::DnsServer::make_response(aaaa_query, ".local");
  REQUIRE(aaaa_response.size() == query.size());
  REQUIRE(aaaa_response[7] == 0);

  const auto external_response = appytizer::DnsServer::make_response(query, ".example");
  REQUIRE(external_response.size() == query.size());
  REQUIRE(external_response[3] == 0x83);
}

TEST_CASE("Folder watcher emits a notification for a direct child") {
  const auto root = std::filesystem::temp_directory_path() /
                    ("appytizer-watcher-" + std::to_string(GetCurrentProcessId()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  REQUIRE_FALSE(error);

  std::mutex mutex;
  std::condition_variable changed;
  int notifications = 0;
  appytizer::FolderWatcher watcher;
  REQUIRE(watcher.start(root, [&] {
    std::scoped_lock lock(mutex);
    ++notifications;
    changed.notify_all();
  }));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::filesystem::create_directories(root / "site", error);
  REQUIRE_FALSE(error);
  {
    std::unique_lock lock(mutex);
    REQUIRE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return notifications > 0; }));
  }
  watcher.stop();
  std::filesystem::remove_all(root, error);
}

TEST_CASE("Pipe server delivers events and accepts concurrent clients") {
  const auto name = test_pipe_name();
  appytizer::PipeServer server(name);
  REQUIRE(server.start([](std::string_view request) {
    const auto parsed = appytizer::parse_request(request);
    return parsed ? appytizer::response_ok(parsed->id) : appytizer::response_error("", "invalid request");
  }));

  auto subscriber = connect_pipe(name);
  REQUIRE(subscriber);
  REQUIRE(write_text(subscriber.get(), "{\"id\":\"subscribe\",\"cmd\":\"events.subscribe\"}\n"));
  REQUIRE(read_text(subscriber.get()).find("\"ok\":true") != std::string::npos);

  server.broadcast(R"({"event":"status.update","service":"nginx"})");
  REQUIRE(read_text(subscriber.get()).find("status.update") != std::string::npos);

  std::atomic_int successes = 0;
  std::vector<std::thread> clients;
  for (int i = 0; i != 8; ++i) {
    clients.emplace_back([&, i] {
      auto client = connect_pipe(name);
      if (!client) return;
      const auto request = std::string("{\"id\":\"client-") + std::to_string(i) + "\",\"cmd\":\"service.list\"}\n";
      if (write_text(client.get(), request) && read_text(client.get()).find("\"ok\":true") != std::string::npos) ++successes;
    });
  }
  for (auto& client : clients) client.join();
  subscriber.reset();
  server.stop();
  REQUIRE(successes == 8);
}
