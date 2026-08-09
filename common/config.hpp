#pragma once
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace appytizer {
/// Persisted application settings shared by engine and UI.
struct AppConfig {
  std::filesystem::path root_folder;
  std::string extension{".test"};
  bool run_minimized{};
  bool autostart{};
  std::map<std::string, std::string> active_versions;
  std::map<std::string, std::filesystem::path> service_roots;
};
/// Thread-safe JSON-backed configuration store.
class ConfigStore {
public:
  explicit ConfigStore(std::filesystem::path path = default_path());
  [[nodiscard]] AppConfig load() const;
  [[nodiscard]] bool save(const AppConfig& config) const;
  [[nodiscard]] static std::filesystem::path default_path();
private:
  std::filesystem::path path_;
  mutable std::mutex mutex_;
};
} // namespace appytizer
