#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace appytizer {

struct TlsStatus {
  bool ready{};
  bool trusted{};
  std::size_t site_certificate_count{};
  std::string earliest_expiry;
  std::string error;
};

void to_json(nlohmann::json& json, const TlsStatus& status);

/// Owns Appytizer's machine-wide CA and exact per-site nginx certificates.
class CertificateManager {
public:
  explicit CertificateManager(std::filesystem::path directory = default_directory(),
                              bool restrict_private_keys = true);

  [[nodiscard]] static std::filesystem::path default_directory();
  [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
  [[nodiscard]] std::filesystem::path certificate_path(std::string_view hostname) const;
  [[nodiscard]] std::filesystem::path private_key_path(std::string_view hostname) const;

  /// Creates or validates the CA and installs its exact certificate in LocalMachine\Root.
  bool provision();
  /// Creates or validates CA files without modifying a trust store. Intended for tests.
  bool provision_files_only();
  /// Removes only the recorded Appytizer CA trust entry and Appytizer-owned certificate files.
  bool remove();
  /// Creates or renews an exact-host leaf certificate when fewer than 30 days remain.
  bool ensure_site_certificate(std::string_view hostname);
  [[nodiscard]] bool certificate_needs_renewal(std::string_view hostname,
                                               int threshold_days = 30) const;
  /// Removes leaf material not present in the supplied exact-hostname set.
  bool remove_unused_site_certificates(const std::vector<std::string>& hostnames);
  [[nodiscard]] TlsStatus status() const;

private:
  bool ensure_ca_files();
  bool install_trust_entry();
  bool remove_recorded_trust_entry();
  [[nodiscard]] bool is_trusted() const;
  [[nodiscard]] bool ca_files_valid() const;
  [[nodiscard]] bool site_certificate_valid(std::string_view hostname, int renewal_days) const;
  void set_error(std::string message) const;

  std::filesystem::path directory_;
  bool restrict_private_keys_{};
  mutable std::mutex mutex_;
  mutable std::string last_error_;
};

} // namespace appytizer
