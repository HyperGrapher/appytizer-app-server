#include <catch2/catch_test_macros.hpp>

#include "common/config.hpp"
#include "common/ipc_protocol.hpp"
#include "common/win_handle.hpp"
#include "engine/dns/dns_server.hpp"
#include "engine/engine.hpp"
#include "engine/ipc/pipe_server.hpp"
#include "engine/services/detection_utils.hpp"
#include "engine/services/providers.hpp"
#include "engine/sites/folder_watcher.hpp"
#include "engine/sites/site_registry.hpp"
#include "engine/tls/certificate_manager.hpp"

#include <windows.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

namespace {
using appytizer::WinHandle;

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view purpose)
      : path_(std::filesystem::temp_directory_path() /
              ("appytizer-" + std::string(purpose) + "-" +
               std::to_string(GetCurrentProcessId()) + "-" +
               std::to_string(next_++))) {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_, error);
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  inline static std::atomic_uint64_t next_{1};
  std::filesystem::path path_;
};

class ScopedEnvironmentVariable {
public:
  ScopedEnvironmentVariable(std::wstring name, const std::filesystem::path& value)
      : name_(std::move(name)) {
    const DWORD size = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
    if (size > 0) {
      previous_.resize(size, L'\0');
      GetEnvironmentVariableW(name_.c_str(), previous_.data(), size);
      previous_.resize(wcslen(previous_.c_str()));
      had_previous_ = true;
    }
    SetEnvironmentVariableW(name_.c_str(), value.c_str());
  }
  ~ScopedEnvironmentVariable() {
    SetEnvironmentVariableW(name_.c_str(), had_previous_ ? previous_.c_str() : nullptr);
  }

private:
  std::wstring name_;
  std::wstring previous_;
  bool had_previous_{};
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}

X509* load_certificate(const std::filesystem::path& path) {
  FILE* file{};
  _wfopen_s(&file, path.c_str(), L"rb");
  if (!file) {
    return nullptr;
  }
  X509* certificate = PEM_read_X509(file, nullptr, nullptr, nullptr);
  fclose(file);
  return certificate;
}

EVP_PKEY* load_private_key(const std::filesystem::path& path) {
  FILE* file{};
  _wfopen_s(&file, path.c_str(), L"rb");
  if (!file) {
    return nullptr;
  }
  EVP_PKEY* key = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
  fclose(file);
  return key;
}

std::vector<std::uint8_t> dns_query(std::string_view hostname, std::uint16_t type = 1) {
  std::vector<std::uint8_t> query{0x12, 0x34, 0x01, 0, 0, 1, 0, 0, 0, 0, 0, 0};
  std::size_t start{};
  while (start < hostname.size()) {
    const auto end = hostname.find('.', start);
    const auto size = (end == std::string_view::npos ? hostname.size() : end) - start;
    query.push_back(static_cast<std::uint8_t>(size));
    query.insert(query.end(), hostname.begin() + static_cast<std::ptrdiff_t>(start),
                 hostname.begin() + static_cast<std::ptrdiff_t>(start + size));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  query.insert(query.end(), {0, static_cast<std::uint8_t>(type >> 8),
                             static_cast<std::uint8_t>(type & 0xff), 0, 1});
  return query;
}

WinHandle connect_pipe(const std::wstring& name) {
  for (int attempt = 0; attempt != 300; ++attempt) {
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      return WinHandle(pipe);
    }
    const DWORD last_error = GetLastError();
    if (last_error == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(name.c_str(), 100);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return {};
}

bool write_text(HANDLE pipe, std::string_view text) {
  DWORD written{};
  return WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
         written == text.size();
}

std::string read_text(HANDLE pipe) {
  std::array<char, 65536> buffer{};
  DWORD read{};
  if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
    return {};
  }
  return {buffer.data(), read};
}

std::wstring test_pipe_name() {
  return L"\\\\.\\pipe\\AppytizerTests-" + std::to_wstring(GetCurrentProcessId());
}

bool is_elevated_admin() {
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID administrators{};
  if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                &administrators)) {
    return false;
  }
  BOOL is_member{};
  const bool checked = CheckTokenMembership(nullptr, administrators, &is_member) != FALSE;
  FreeSid(administrators);
  return checked && is_member;
}

std::filesystem::path find_nginx() {
  const DWORD override_size = GetEnvironmentVariableW(L"APPYTIZER_NGINX_TEST_EXECUTABLE", nullptr, 0);
  if (override_size > 0) {
    std::wstring override_path(override_size, L'\0');
    GetEnvironmentVariableW(L"APPYTIZER_NGINX_TEST_EXECUTABLE", override_path.data(), override_size);
    override_path.resize(wcslen(override_path.c_str()));
    return override_path;
  }
  std::wstring buffer(32768, L'\0');
  const DWORD size = SearchPathW(nullptr, L"nginx.exe", nullptr,
                                 static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
  std::filesystem::path candidate;
  if (size > 0 && size < buffer.size()) {
    buffer.resize(size);
    candidate = buffer;
  } else {
    const DWORD local_size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (local_size == 0) {
      return {};
    }
    std::wstring local(local_size, L'\0');
    GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), local_size);
    local.resize(wcslen(local.c_str()));
    candidate = std::filesystem::path(local) / L"Microsoft" / L"WinGet" / L"Links" / L"nginx.exe";
    std::error_code exists_error;
    if (!std::filesystem::exists(candidate, exists_error) || exists_error) {
      return {};
    }
  }
  std::error_code symlink_error;
  const auto symlink_target = std::filesystem::read_symlink(candidate, symlink_error);
  if (!symlink_error && !symlink_target.empty()) {
    return symlink_target;
  }
  WinHandle handle(CreateFileW(candidate.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr));
  if (handle) {
    std::wstring resolved(32768, L'\0');
    const DWORD resolved_size = GetFinalPathNameByHandleW(
        handle.get(), resolved.data(), static_cast<DWORD>(resolved.size()), FILE_NAME_NORMALIZED);
    if (resolved_size > 0 && resolved_size < resolved.size()) {
      resolved.resize(resolved_size);
      constexpr std::wstring_view prefix = L"\\\\?\\";
      if (resolved.starts_with(prefix)) {
        resolved.erase(0, prefix.size());
      }
      return resolved;
    }
  }
  return candidate;
}

bool validate_nginx(const std::filesystem::path& executable,
                    const std::filesystem::path& prefix,
                    const std::filesystem::path& configuration) {
  std::wstring command = L"\"" + executable.wstring() + L"\" -t -p \"" +
                         prefix.wstring() + L"\" -c \"" + configuration.wstring() + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process_info{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, executable.parent_path().c_str(), &startup, &process_info)) {
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
} // namespace

TEST_CASE("IPC requests are validated and responses are newline delimited") {
  const auto request = appytizer::parse_request(R"({"id":"7","cmd":"sites.list"})");
  REQUIRE(request);
  REQUIRE(request->id == "7");
  REQUIRE(request->command == "sites.list");
  REQUIRE_FALSE(appytizer::parse_request("not-json"));
  REQUIRE_FALSE(appytizer::parse_request(R"({"id":"7"})"));
  REQUIRE(appytizer::response_ok("7").ends_with("\n"));
}

TEST_CASE("HTTPS configuration round-trips and obsolete extension is ignored") {
  TemporaryDirectory directory("config");
  appytizer::ConfigStore store(directory.path() / "config.json");
  appytizer::AppConfig original;
  original.root_folder = directory.path() / "sites";
  original.https_enabled = false;
  original.run_minimized = true;
  original.autostart = true;
  original.active_versions["php"] = "8.4.7";
  original.service_roots["nginx"] = directory.path() / "nginx";
  REQUIRE(store.save(original));
  const auto saved_text = read_file(directory.path() / "config.json");
  REQUIRE(saved_text.find("https_enabled") != std::string::npos);
  REQUIRE(saved_text.find("extension") == std::string::npos);
  const auto loaded = store.load();
  REQUIRE(loaded.root_folder == original.root_folder);
  REQUIRE_FALSE(loaded.https_enabled);
  REQUIRE(loaded.run_minimized);
  REQUIRE(loaded.autostart);
  REQUIRE(loaded.active_versions.at("php") == "8.4.7");
  REQUIRE(loaded.service_roots.at("nginx") == directory.path() / "nginx");
}

TEST_CASE("Engine IPC round-trips https_enabled and exposes TLS status") {
  TemporaryDirectory directory("engine-ipc");
  const auto data_path = directory.path() / "data";
  const auto certificate_path = directory.path() / "certificates";
  const auto hosts_path = directory.path() / "hosts";
  const auto projects_path = directory.path() / "projects";
  std::error_code error;
  std::filesystem::create_directories(projects_path / "hello", error);
  REQUIRE_FALSE(error);
  std::ofstream(hosts_path) << "127.0.0.1 localhost\n";
  ScopedEnvironmentVariable data_directory(L"APPYTIZER_DATA_DIR", data_path);
  ScopedEnvironmentVariable certificate_directory(L"APPYTIZER_CERTIFICATES_DIR", certificate_path);
  ScopedEnvironmentVariable hosts_file(L"APPYTIZER_HOSTS_FILE", hosts_path);

  appytizer::Engine engine;
  const auto initial_status = nlohmann::json::parse(
      engine.handle_message(R"({"id":"tls","cmd":"tls.status"})"));
  REQUIRE(initial_status.value("ok", false));
  const auto& tls = initial_status.at("result");
  REQUIRE(tls.contains("ready"));
  REQUIRE(tls.contains("trusted"));
  REQUIRE(tls.contains("site_certificate_count"));
  REQUIRE(tls.contains("earliest_expiry"));
  REQUIRE(tls.contains("error"));

  const nlohmann::json set_request{{"id", "set"},
                                   {"cmd", "config.set"},
                                   {"params", {{"root_folder", projects_path.string()},
                                               {"https_enabled", false},
                                               {"run_minimized", true},
                                               {"autostart", true}}}};
  const auto set_response = nlohmann::json::parse(engine.handle_message(set_request.dump()));
  REQUIRE(set_response.value("ok", false));
  const auto get_response = nlohmann::json::parse(
      engine.handle_message(R"({"id":"get","cmd":"config.get"})"));
  REQUIRE(get_response.value("ok", false));
  REQUIRE_FALSE(get_response.at("result").at("https_enabled").get<bool>());
  REQUIRE(get_response.at("result").at("run_minimized").get<bool>());
  REQUIRE(get_response.at("result").at("autostart").get<bool>());
  REQUIRE_FALSE(get_response.at("result").contains("extension"));

  const auto sites_response = nlohmann::json::parse(
      engine.handle_message(R"({"id":"sites","cmd":"sites.list"})"));
  REQUIRE(sites_response.at("result").size() == 1);
  REQUIRE(sites_response.at("result").front().at("hostname") == "hello.test");
  REQUIRE(read_file(hosts_path).find("hello.test") != std::string::npos);
}

TEST_CASE("Service versions and executable detection are stable") {
  REQUIRE(appytizer::parse_version("PHP 8.4.12 (cgi-fcgi)") == "8.4.12");
  REQUIRE(appytizer::parse_version("nginx/1.27.0") == "1.27.0");
  TemporaryDirectory directory("detection");
  std::error_code error;
  std::filesystem::create_directories(directory.path() / "nested", error);
  REQUIRE_FALSE(error);
  std::ofstream(directory.path() / "nested" / "php.exe") << "test";
  const auto found = appytizer::find_installed_executables(
      {L"php.exe"}, {directory.path()}, {L"__appytizer_test_registry_token__"});
  REQUIRE(std::ranges::any_of(found, [&](const auto& path) {
    return path.parent_path() == directory.path() / "nested";
  }));
}

TEST_CASE("DNS responds only to fixed .test A records") {
  const auto query = dns_query("hello.test");
  const auto response = appytizer::DnsServer::make_response(query);
  REQUIRE(response.size() == query.size() + 16);
  REQUIRE(response[7] == 1);
  REQUIRE(response.back() == 1);

  const auto aaaa_response = appytizer::DnsServer::make_response(dns_query("hello.test", 28));
  REQUIRE(aaaa_response.size() == query.size());
  REQUIRE(aaaa_response[7] == 0);

  const auto external_query = dns_query("hello.example");
  const auto external_response = appytizer::DnsServer::make_response(external_query);
  REQUIRE(external_response.size() == external_query.size());
  REQUIRE(external_response[3] == 0x83);
}

TEST_CASE("DNS label validation rejects unsafe folders and case collisions") {
  REQUIRE(appytizer::SiteRegistry::validate_dns_label("hello").empty());
  REQUIRE(appytizer::SiteRegistry::validate_dns_label("Hello-42").empty());
  REQUIRE_FALSE(appytizer::SiteRegistry::validate_dns_label("hello world").empty());
  REQUIRE_FALSE(appytizer::SiteRegistry::validate_dns_label("hello_world").empty());
  REQUIRE_FALSE(appytizer::SiteRegistry::validate_dns_label("-hello").empty());
  REQUIRE_FALSE(appytizer::SiteRegistry::validate_dns_label("hello-").empty());
  REQUIRE_FALSE(appytizer::SiteRegistry::validate_dns_label(std::string(64, 'a')).empty());
  REQUIRE_FALSE(appytizer::SiteRegistry::validate_dns_label("h\xC3\xA9llo").empty());

  const auto errors = appytizer::SiteRegistry::validate_dns_labels({"Hello", "hello", "valid"});
  REQUIRE(errors.contains("Hello"));
  REQUIRE(errors.contains("hello"));
  REQUIRE_FALSE(errors.contains("valid"));
}

TEST_CASE("nginx configuration covers HTTPS, HTTP, PHP, and default-host rejection") {
  TemporaryDirectory directory("nginx");
  ScopedEnvironmentVariable data_directory(L"APPYTIZER_DATA_DIR", directory.path() / "data");
  std::error_code error;
  std::filesystem::create_directories(directory.path() / "projects" / "hello", error);
  std::filesystem::create_directories(directory.path() / "projects" / "php", error);
  std::filesystem::create_directories(directory.path() / "projects" / "empty", error);
  std::filesystem::create_directories(directory.path() / "projects" / "bad_name", error);
  std::ofstream(directory.path() / "projects" / "hello" / "index.html") << "<!doctype html>";
  std::ofstream(directory.path() / "projects" / "php" / "index.php") << "<?php";
  REQUIRE_FALSE(error);

  appytizer::SiteRegistry registry;
  REQUIRE(registry.rescan(directory.path() / "projects"));
  const auto sites = registry.list();
  REQUIRE(sites.size() == 4);
  REQUIRE(std::ranges::count_if(sites, [](const auto& site) { return site.value("valid", false); }) == 3);
  const auto hello = std::ranges::find_if(sites, [](const auto& site) {
    return site.value("name", "") == "hello";
  });
  const auto php = std::ranges::find_if(sites, [](const auto& site) {
    return site.value("name", "") == "php";
  });
  const auto empty = std::ranges::find_if(sites, [](const auto& site) {
    return site.value("name", "") == "empty";
  });
  REQUIRE(hello != sites.end());
  REQUIRE(hello->value("type", "") == "html");
  REQUIRE(hello->value("has_index", false));
  REQUIRE(php != sites.end());
  REQUIRE(php->value("type", "") == "php");
  REQUIRE(php->value("has_index", false));
  REQUIRE(empty != sites.end());
  REQUIRE(empty->value("type", "").empty());
  REQUIRE_FALSE(empty->value("has_index", true));

  const auto certificates = directory.path() / "certificates";
  const auto https = directory.path() / "https";
  REQUIRE(registry.write_nginx_configs(https / "sites", true, certificates, 9000,
                                       directory.path() / "nginx" / "conf" / "fastcgi_params"));
  REQUIRE(registry.write_nginx_root_config(https / "runtime" / "conf" / "nginx.conf",
                                           directory.path() / "nginx", true));
  const auto hello_https = read_file(https / "sites" / "hello.test.conf");
  const auto php_https = read_file(https / "sites" / "php.test.conf");
  const auto https_root = read_file(https / "runtime" / "conf" / "nginx.conf");
  REQUIRE(hello_https.find("return 308 https://$host$request_uri") != std::string::npos);
  REQUIRE(hello_https.find("listen 443 ssl") != std::string::npos);
  REQUIRE(hello_https.find("hello.test.crt.pem") != std::string::npos);
  REQUIRE(hello_https.find("hello.test.key.pem") != std::string::npos);
  REQUIRE(hello_https.find("Strict-Transport-Security") == std::string::npos);
  REQUIRE(php_https.find("fastcgi_pass 127.0.0.1:9000") != std::string::npos);
  REQUIRE(php_https.find("SCRIPT_FILENAME") != std::string::npos);
  REQUIRE(https_root.find("listen 80 default_server") != std::string::npos);
  REQUIRE(https_root.find("listen 443 ssl default_server") != std::string::npos);
  REQUIRE(https_root.find("ssl_reject_handshake on") != std::string::npos);
  REQUIRE(https_root.find("include ../../sites/*.conf") != std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(https / "sites" / "bad_name.test.conf"));

  const auto http = directory.path() / "http";
  REQUIRE(registry.write_nginx_configs(http / "sites", false, certificates));
  REQUIRE(registry.write_nginx_root_config(http / "runtime" / "conf" / "nginx.conf",
                                           directory.path() / "nginx", false));
  const auto hello_http = read_file(http / "sites" / "hello.test.conf");
  const auto http_root = read_file(http / "runtime" / "conf" / "nginx.conf");
  REQUIRE(hello_http.find("listen 80") != std::string::npos);
  REQUIRE(hello_http.find("listen 443") == std::string::npos);
  REQUIRE(hello_http.find("ssl_certificate") == std::string::npos);
  REQUIRE(http_root.find("listen 443") == std::string::npos);
}

TEST_CASE("installed nginx accepts the generated HTTPS configuration") {
  const auto nginx = find_nginx();
  std::error_code nginx_error;
  if (nginx.empty() ||
      !std::filesystem::exists(nginx.parent_path() / "conf" / "mime.types", nginx_error) ||
      nginx_error) {
    SUCCEED("nginx validation is unavailable; set APPYTIZER_NGINX_TEST_EXECUTABLE to enable it.");
    return;
  }
  TemporaryDirectory directory("nginx-validation");
  ScopedEnvironmentVariable data_directory(L"APPYTIZER_DATA_DIR", directory.path() / "data");
  std::error_code error;
  const auto projects = directory.path() / "projects";
  std::filesystem::create_directories(projects / "hello", error);
  std::filesystem::create_directories(projects / "php", error);
  std::ofstream(projects / "hello" / "index.html") << "<!doctype html>";
  std::ofstream(projects / "php" / "index.php") << "<?php";
  REQUIRE_FALSE(error);

  appytizer::CertificateManager certificates(directory.path() / "certificates", false);
  REQUIRE(certificates.provision_files_only());
  REQUIRE(certificates.ensure_site_certificate("hello.test"));
  REQUIRE(certificates.ensure_site_certificate("php.test"));
  appytizer::SiteRegistry registry;
  REQUIRE(registry.rescan(projects));
  const auto staging = directory.path() / "staging";
  const auto configuration = staging / "runtime" / "conf" / "nginx.conf";
  REQUIRE(registry.write_nginx_configs(staging / "sites", true, certificates.directory(),
                                       9000, nginx.parent_path() / "conf" / "fastcgi_params"));
  REQUIRE(registry.write_nginx_root_config(configuration, nginx.parent_path(), true));
  REQUIRE(validate_nginx(nginx, staging / "runtime", configuration));
}

TEST_CASE("CA and exact leaf generation are valid, idempotent, and atomically replaced") {
  TemporaryDirectory directory("tls");
  appytizer::CertificateManager certificates(directory.path(), false);
  REQUIRE(certificates.provision_files_only());
  const auto root_certificate_path = directory.path() / "appytizer-root-ca.crt.pem";
  const auto root_key_path = directory.path() / "appytizer-root-ca.key.pem";
  const auto original_root = read_file(root_certificate_path);
  REQUIRE(certificates.provision_files_only());
  REQUIRE(read_file(root_certificate_path) == original_root);

  REQUIRE(certificates.ensure_site_certificate("hello.test"));
  REQUIRE_FALSE(certificates.certificate_needs_renewal("hello.test", 30));
  REQUIRE(certificates.certificate_needs_renewal("hello.test", 398));

  X509* root = load_certificate(root_certificate_path);
  EVP_PKEY* root_key = load_private_key(root_key_path);
  X509* leaf = load_certificate(certificates.certificate_path("hello.test"));
  EVP_PKEY* leaf_key = load_private_key(certificates.private_key_path("hello.test"));
  REQUIRE(root != nullptr);
  REQUIRE(root_key != nullptr);
  REQUIRE(leaf != nullptr);
  REQUIRE(leaf_key != nullptr);
  REQUIRE(EVP_PKEY_bits(root_key) >= 3072);
  REQUIRE(EVP_PKEY_bits(leaf_key) >= 2048);
  REQUIRE(X509_check_private_key(root, root_key) == 1);
  REQUIRE(X509_check_private_key(leaf, leaf_key) == 1);
  REQUIRE(X509_check_issued(root, leaf) == X509_V_OK);
  EVP_PKEY* root_public_key = X509_get_pubkey(root);
  REQUIRE(root_public_key != nullptr);
  REQUIRE(X509_verify(root, root_public_key) == 1);
  REQUIRE(X509_verify(leaf, root_public_key) == 1);
  REQUIRE(X509_get_signature_nid(root) == NID_sha256WithRSAEncryption);
  REQUIRE(X509_get_signature_nid(leaf) == NID_sha256WithRSAEncryption);

  std::array<char, 256> common_name{};
  REQUIRE(X509_NAME_get_text_by_NID(X509_get_subject_name(leaf), NID_commonName,
                                    common_name.data(), static_cast<int>(common_name.size())) == 10);
  REQUIRE(std::string(common_name.data()) == "hello.test");
  GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(
      X509_get_ext_d2i(leaf, NID_subject_alt_name, nullptr, nullptr));
  REQUIRE(names != nullptr);
  REQUIRE(sk_GENERAL_NAME_num(names) == 1);
  const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, 0);
  REQUIRE(name->type == GEN_DNS);
  REQUIRE(std::string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(name->d.dNSName)),
                      static_cast<std::size_t>(ASN1_STRING_length(name->d.dNSName))) == "hello.test");
  EXTENDED_KEY_USAGE* usage = static_cast<EXTENDED_KEY_USAGE*>(
      X509_get_ext_d2i(leaf, NID_ext_key_usage, nullptr, nullptr));
  REQUIRE(usage != nullptr);
  REQUIRE(std::ranges::any_of(std::views::iota(0, sk_ASN1_OBJECT_num(usage)), [&](int index) {
    return OBJ_obj2nid(sk_ASN1_OBJECT_value(usage, index)) == NID_server_auth;
  }));
  int root_days{}; int root_seconds{}; int leaf_days{}; int leaf_seconds{};
  REQUIRE(ASN1_TIME_diff(&root_days, &root_seconds, nullptr, X509_get0_notAfter(root)) == 1);
  REQUIRE(ASN1_TIME_diff(&leaf_days, &leaf_seconds, nullptr, X509_get0_notAfter(leaf)) == 1);
  REQUIRE(root_days >= 3651);
  REQUIRE(root_days <= 3652);
  REQUIRE(leaf_days >= 396);
  REQUIRE(leaf_days <= 397);

  GENERAL_NAMES_free(names);
  EXTENDED_KEY_USAGE_free(usage);
  EVP_PKEY_free(root_public_key);
  EVP_PKEY_free(root_key);
  EVP_PKEY_free(leaf_key);
  X509_free(root);
  X509_free(leaf);

  const auto first_leaf = read_file(certificates.certificate_path("hello.test"));
  REQUIRE(certificates.ensure_site_certificate("hello.test"));
  REQUIRE(read_file(certificates.certificate_path("hello.test")) == first_leaf);
  {
    std::ofstream(certificates.certificate_path("hello.test"), std::ios::trunc) << "invalid";
  }
  REQUIRE(certificates.ensure_site_certificate("hello.test"));
  REQUIRE(read_file(certificates.certificate_path("hello.test")) != "invalid");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory.path())) {
    REQUIRE(entry.path().filename().string().find(".tmp.") == std::string::npos);
  }

  const auto status = certificates.status();
  REQUIRE_FALSE(status.trusted);
  REQUIRE(status.site_certificate_count == 1);
  REQUIRE_FALSE(status.earliest_expiry.empty());
  const nlohmann::json status_json = status;
  REQUIRE(status_json.contains("ready"));
  REQUIRE(status_json.contains("trusted"));
  REQUIRE(status_json.contains("site_certificate_count"));
  REQUIRE(status_json.contains("earliest_expiry"));
  REQUIRE(status_json.contains("error"));
}

TEST_CASE("obsolete leaf certificates are removed only by exact retained hostname") {
  TemporaryDirectory directory("tls-cleanup");
  appytizer::CertificateManager certificates(directory.path(), false);
  REQUIRE(certificates.provision_files_only());
  REQUIRE(certificates.ensure_site_certificate("hello.test"));
  REQUIRE(certificates.ensure_site_certificate("php.test"));
  REQUIRE(certificates.remove_unused_site_certificates({"hello.test"}));
  REQUIRE(std::filesystem::exists(certificates.certificate_path("hello.test")));
  REQUIRE(std::filesystem::exists(certificates.private_key_path("hello.test")));
  REQUIRE_FALSE(std::filesystem::exists(certificates.certificate_path("php.test")));
  REQUIRE_FALSE(std::filesystem::exists(certificates.private_key_path("php.test")));
}

TEST_CASE("elevated trust provisioning and exact-certificate cleanup", "[.elevated]") {
  if (GetEnvironmentVariableW(L"APPYTIZER_RUN_ELEVATED_TLS_TESTS", nullptr, 0) == 0) {
    SKIP("Set APPYTIZER_RUN_ELEVATED_TLS_TESTS=1 to run the trust-store integration test.");
  }
  if (!is_elevated_admin()) {
    SKIP("This integration test must run from an elevated terminal.");
  }
  TemporaryDirectory first_directory("trusted-ca-one");
  TemporaryDirectory second_directory("trusted-ca-two");
  appytizer::CertificateManager first(first_directory.path(), false);
  appytizer::CertificateManager second(second_directory.path(), false);
  struct Cleanup {
    appytizer::CertificateManager* first;
    appytizer::CertificateManager* second;
    ~Cleanup() {
      const bool first_removed = first->remove();
      const bool second_removed = second->remove();
      (void)first_removed;
      (void)second_removed;
    }
  } cleanup{&first, &second};

  REQUIRE(first.provision());
  REQUIRE(first.provision());
  REQUIRE(second.provision());
  REQUIRE(first.status().trusted);
  REQUIRE(second.status().trusted);
  REQUIRE(first.remove());
  REQUIRE(second.status().trusted);
  REQUIRE(second.remove());
}

TEST_CASE("Folder watcher emits a notification for a direct child") {
  TemporaryDirectory directory("watcher");
  std::mutex mutex;
  std::condition_variable changed;
  int notifications = 0;
  appytizer::FolderWatcher watcher;
  REQUIRE(watcher.start(directory.path(), [&] {
    std::scoped_lock lock(mutex);
    ++notifications;
    changed.notify_all();
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::error_code error;
  std::filesystem::create_directories(directory.path() / "site", error);
  REQUIRE_FALSE(error);
  {
    std::unique_lock lock(mutex);
    REQUIRE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return notifications > 0; }));
  }
  watcher.stop();
}

TEST_CASE("Pipe server delivers events and accepts concurrent clients") {
  const auto name = test_pipe_name();
  appytizer::PipeServer server(name);
  REQUIRE(server.start([](std::string_view request) {
    const auto parsed = appytizer::parse_request(request);
    return parsed ? appytizer::response_ok(parsed->id)
                  : appytizer::response_error("", "invalid request");
  }));
  auto subscriber = connect_pipe(name);
  REQUIRE(subscriber);
  REQUIRE(write_text(subscriber.get(), "{\"id\":\"subscribe\",\"cmd\":\"events.subscribe\"}\n"));
  REQUIRE(read_text(subscriber.get()).find("\"ok\":true") != std::string::npos);
  server.broadcast(R"({"event":"status.update","service":"nginx"})");
  REQUIRE(read_text(subscriber.get()).find("status.update") != std::string::npos);

  std::atomic_int successes = 0;
  std::vector<std::thread> clients;
  for (int index = 0; index != 8; ++index) {
    clients.emplace_back([&, index] {
      auto client = connect_pipe(name);
      if (!client) {
        return;
      }
      const auto request = std::string("{\"id\":\"client-") + std::to_string(index) +
                           "\",\"cmd\":\"service.list\"}\n";
      if (write_text(client.get(), request) &&
          read_text(client.get()).find("\"ok\":true") != std::string::npos) {
        ++successes;
      }
    });
  }
  for (auto& client : clients) {
    client.join();
  }
  subscriber.reset();
  server.stop();
  REQUIRE(successes == 8);
}
