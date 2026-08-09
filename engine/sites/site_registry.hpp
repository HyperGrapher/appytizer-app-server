#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace appytizer {

/// SQLite-backed snapshot of folders served by Appytizer.
class SiteRegistry {
public:
  SiteRegistry();
  ~SiteRegistry();
  SiteRegistry(const SiteRegistry&) = delete;
  SiteRegistry& operator=(const SiteRegistry&) = delete;

  bool rescan(const std::filesystem::path& root);
  [[nodiscard]] nlohmann::json list() const;
  [[nodiscard]] static std::string validate_dns_label(std::string_view label);
  [[nodiscard]] static std::map<std::string, std::string, std::less<>>
  validate_dns_labels(const std::vector<std::string>& labels);

  bool write_nginx_configs(const std::filesystem::path& output, bool https_enabled,
                           const std::filesystem::path& certificates_directory,
                           std::uint16_t php_port = 9000,
                           const std::filesystem::path& fastcgi_params = {}) const;
  bool write_nginx_root_config(const std::filesystem::path& output,
                               const std::filesystem::path& nginx_root,
                               bool https_enabled) const;

private:
  sqlite3* db_{};
};

} // namespace appytizer
