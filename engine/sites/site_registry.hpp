#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>

struct sqlite3;
namespace appytizer {
/// SQLite-backed snapshot of folders served by Appytizer.
class SiteRegistry {
public:
  SiteRegistry(); ~SiteRegistry();
  SiteRegistry(const SiteRegistry&) = delete; SiteRegistry& operator=(const SiteRegistry&) = delete;
  bool rescan(const std::filesystem::path& root);
  [[nodiscard]] nlohmann::json list() const;
  bool write_nginx_configs(const std::filesystem::path& output, std::string_view extension,
      std::uint16_t php_port = 9000, const std::filesystem::path& fastcgi_params = {}) const;
  /// Writes the nginx root configuration that includes Appytizer's generated site configs.
  bool write_nginx_root_config(const std::filesystem::path& output, const std::filesystem::path& nginx_root) const;
private: sqlite3* db_{};
};
} // namespace appytizer
