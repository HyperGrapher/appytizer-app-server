#include "engine/services/detection_utils.hpp"
#include <windows.h>
#include <algorithm>
#include <set>

namespace appytizer {
namespace {
std::wstring lower(std::wstring value) {
  std::ranges::transform(value, value.begin(), ::towlower); return value;
}
void add_if_executable(std::set<std::filesystem::path>& result, const std::filesystem::path& candidate) {
  std::error_code error;
  if (std::filesystem::is_regular_file(candidate, error)) result.insert(std::filesystem::weakly_canonical(candidate, error));
}
void scan_root(std::set<std::filesystem::path>& result, const std::filesystem::path& root,
               const std::vector<std::wstring>& names) {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error)) return;
  for (const auto& name : names) add_if_executable(result, root / name);
  std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
  for (; it != end; it.increment(error)) {
    if (error) { error.clear(); continue; }
    if (it.depth() >= 4) it.disable_recursion_pending();
    if (!it->is_regular_file(error)) continue;
    for (const auto& name : names) if (_wcsicmp(it->path().filename().c_str(), name.c_str()) == 0) add_if_executable(result, it->path());
  }
}
void registry_roots(std::vector<std::filesystem::path>& roots, HKEY hive, REGSAM view,
                    const std::vector<std::wstring>& tokens) {
  HKEY key{};
  if (RegOpenKeyExW(hive, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | view, &key) != ERROR_SUCCESS) return;
  DWORD index{}; wchar_t subkey[256]{}; DWORD length{};
  while (true) {
    length = static_cast<DWORD>(std::size(subkey));
    if (RegEnumKeyExW(key, index++, subkey, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    HKEY item{}; if (RegOpenKeyExW(key, subkey, 0, KEY_READ | view, &item) != ERROR_SUCCESS) continue;
    wchar_t display[512]{}, location[32768]{}; DWORD display_size = sizeof(display), location_size = sizeof(location), type{};
    RegQueryValueExW(item, L"DisplayName", nullptr, &type, reinterpret_cast<BYTE*>(display), &display_size);
    RegQueryValueExW(item, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(location), &location_size);
    const auto name = lower(display); bool matches = tokens.empty();
    for (const auto& token : tokens) if (name.find(lower(token)) != std::wstring::npos) matches = true;
    if (matches && location[0]) roots.emplace_back(location);
    RegCloseKey(item);
  }
  RegCloseKey(key);
}
}

std::filesystem::path environment_path(const wchar_t* name) {
  const DWORD size = GetEnvironmentVariableW(name, nullptr, 0); if (!size) return {};
  std::wstring value(size, L'\0'); GetEnvironmentVariableW(name, value.data(), size); value.resize(wcslen(value.c_str()));
  return value;
}

std::vector<std::filesystem::path> find_installed_executables(
    const std::vector<std::wstring>& executable_names, const std::vector<std::filesystem::path>& requested_roots,
    const std::vector<std::wstring>& registry_name_tokens) {
  std::vector<std::filesystem::path> roots = requested_roots;
  registry_roots(roots, HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, registry_name_tokens);
  registry_roots(roots, HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, registry_name_tokens);
  registry_roots(roots, HKEY_CURRENT_USER, 0, registry_name_tokens);
  std::set<std::filesystem::path> found;
  for (const auto& root : roots) if (!root.empty()) scan_root(found, root, executable_names);
  const DWORD path_size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
  if (path_size) {
    std::wstring path(path_size, L'\0'); GetEnvironmentVariableW(L"PATH", path.data(), path_size);
    std::size_t begin{};
    while (begin < path.size()) { const auto end = path.find(L';', begin); const auto directory = path.substr(begin, end - begin); for (const auto& name : executable_names) add_if_executable(found, std::filesystem::path(directory) / name); if (end == std::wstring::npos) break; begin = end + 1; }
  }
  return {found.begin(), found.end()};
}

std::string executable_version(const std::filesystem::path& executable) {
  DWORD ignored{}; const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
  if (size) {
    std::vector<BYTE> data(size); VS_FIXEDFILEINFO* info{}; UINT info_size{};
    if (GetFileVersionInfoW(executable.c_str(), 0, size, data.data()) && VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) && info) {
      return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." + std::to_string(HIWORD(info->dwFileVersionLS));
    }
  }
  const auto parent = executable.parent_path().filename().string();
  return parent.empty() ? executable.filename().string() : parent;
}
} // namespace appytizer
