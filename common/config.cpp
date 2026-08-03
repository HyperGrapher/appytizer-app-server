#include "common/config.hpp"
#include "common/constants.hpp"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <nlohmann/json.hpp>

namespace appytizer {
namespace {
std::filesystem::path path_from_utf8(const std::string& value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), size);
  return wide;
}
void to_json(nlohmann::json& j, const AppConfig& c) {
  j = {{"root_folder", c.root_folder.u8string()}, {"extension", c.extension}, {"run_minimized", c.run_minimized},
       {"autostart", c.autostart}, {"active_versions", c.active_versions}};
  nlohmann::json roots = nlohmann::json::object();
  for (const auto& [id, path] : c.service_roots) roots[id] = path.u8string();
  j["service_roots"] = std::move(roots);
}
AppConfig parse(const nlohmann::json& j) {
  AppConfig c;
  c.root_folder = path_from_utf8(j.value("root_folder", std::string{}));
  c.extension = j.value("extension", ".local");
  if (c.extension.empty() || c.extension.front() != '.') c.extension.insert(c.extension.begin(), '.');
  c.run_minimized = j.value("run_minimized", false);
  c.autostart = j.value("autostart", false);
  c.active_versions = j.value("active_versions", decltype(c.active_versions){});
  for (const auto& [id, value] : j.value("service_roots", nlohmann::json::object()).items()) c.service_roots[id] = path_from_utf8(value.get<std::string>());
  return c;
}
}
ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::move(path)) {}
std::filesystem::path ConfigStore::default_path() {
  if (const DWORD size = GetEnvironmentVariableW(L"APPYTIZER_DATA_DIR", nullptr, 0); size > 0) {
    std::wstring override_path(size, L'\0');
    GetEnvironmentVariableW(L"APPYTIZER_DATA_DIR", override_path.data(), size);
    override_path.resize(wcslen(override_path.c_str()));
    return std::filesystem::path(override_path) / L"config.json";
  }
  PWSTR raw{}; std::filesystem::path path = L".";
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) { path = raw; CoTaskMemFree(raw); }
  return path / kApplicationId / L"config.json";
}
AppConfig ConfigStore::load() const {
  std::scoped_lock lock(mutex_); std::ifstream input(path_);
  if (!input) return {};
  try { nlohmann::json j; input >> j; return parse(j); } catch (...) { return {}; }
}
bool ConfigStore::save(const AppConfig& config) const {
  std::scoped_lock lock(mutex_); std::error_code ec; std::filesystem::create_directories(path_.parent_path(), ec);
  const auto temporary = path_.wstring() + L".tmp"; std::ofstream output(temporary, std::ios::trunc);
  if (!output) return false; nlohmann::json j; to_json(j, config); output << j.dump(2) << '\n'; output.close();
  std::filesystem::rename(temporary, path_, ec); if (!ec) return true;
  std::filesystem::remove(path_, ec); ec.clear(); std::filesystem::rename(temporary, path_, ec); return !ec;
}
} // namespace appytizer
