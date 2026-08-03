#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace appytizer {
/// Returns existing executable paths from configured/common roots, uninstall registry entries, and PATH.
[[nodiscard]] std::vector<std::filesystem::path> find_installed_executables(
    const std::vector<std::wstring>& executable_names,
    const std::vector<std::filesystem::path>& roots,
    const std::vector<std::wstring>& registry_name_tokens);
/// Reads the executable's fixed file version, or derives a stable label from its parent directory.
[[nodiscard]] std::string executable_version(const std::filesystem::path& executable);
/// Returns an environment variable as a filesystem path when present.
[[nodiscard]] std::filesystem::path environment_path(const wchar_t* name);
} // namespace appytizer
