#include "common/config.hpp"
#include "common/constants.hpp"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <nlohmann/json.hpp>

namespace appytizer {
namespace {
bool has_data_directory_override() {
  return GetEnvironmentVariableW(L"APPYTIZER_DATA_DIR", nullptr, 0) > 0;
}

std::filesystem::path path_from_utf8(const std::string& value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), size);
  return wide;
}
void to_json(nlohmann::json& j, const AppConfig& c) {
  j = {{"root_folder", c.root_folder.u8string()}, {"https_enabled", c.https_enabled}, {"run_minimized", c.run_minimized},
       {"autostart", c.autostart}, {"active_versions", c.active_versions}};
  nlohmann::json roots = nlohmann::json::object();
  for (const auto& [id, path] : c.service_roots) roots[id] = path.u8string();
  j["service_roots"] = std::move(roots);
}
AppConfig parse(const nlohmann::json& j) {
  AppConfig c;
  if (const auto it = j.find("root_folder"); it != j.end() && it->is_string()) c.root_folder = path_from_utf8(it->get<std::string>());
  if (const auto it = j.find("https_enabled"); it != j.end() && it->is_boolean()) c.https_enabled = it->get<bool>();
  if (const auto it = j.find("run_minimized"); it != j.end() && it->is_boolean()) c.run_minimized = it->get<bool>();
  if (const auto it = j.find("autostart"); it != j.end() && it->is_boolean()) c.autostart = it->get<bool>();
  if (const auto it = j.find("active_versions"); it != j.end() && it->is_object()) {
    for (const auto& [id, value] : it->items()) if (value.is_string()) c.active_versions[id] = value.get<std::string>();
  }
  if (const auto it = j.find("service_roots"); it != j.end() && it->is_object()) {
    for (const auto& [id, value] : it->items()) if (value.is_string()) c.service_roots[id] = path_from_utf8(value.get<std::string>());
  }
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
  PWSTR raw{};
  std::filesystem::path path = L"C:\\ProgramData";
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_CREATE, nullptr, &raw))) {
    path = raw;
    CoTaskMemFree(raw);
  }
  return path / kApplicationId / L"config.json";
}
std::filesystem::path ConfigStore::legacy_user_path() {
  PWSTR raw{};
  std::filesystem::path path = L".";
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
    path = raw;
    CoTaskMemFree(raw);
  }
  return path / kApplicationId / L"config.json";
}
AppConfig ConfigStore::load() const {
  std::scoped_lock lock(mutex_); std::ifstream input(path_);
  if (!input && !has_data_directory_override() && path_ == default_path()) {
    input.clear();
    input.open(legacy_user_path());
  }
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
