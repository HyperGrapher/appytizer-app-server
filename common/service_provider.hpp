#pragma once
#include <windows.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace appytizer {
/// A locally installed version of a managed development service.
struct InstalledVersion {
  std::string version_label;
  std::filesystem::path executable_path;
  bool is_windows_service{};
  std::string windows_service_name;
};
/// Current process and memory state exposed to the UI.
struct ServiceStatus {
  bool running{};
  std::string active_version;
  std::vector<DWORD> process_ids;
  std::size_t working_set_bytes{};
};
/// Extensibility boundary for every service managed by the engine.
class IServiceProvider {
public:
  virtual ~IServiceProvider() = default;
  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual std::string display_name() const = 0;
  virtual std::vector<InstalledVersion> detect() = 0;
  virtual bool start(const std::string& version_label) = 0;
  virtual bool stop() = 0;
  virtual bool restart(const std::string& version_label) = 0;
  [[nodiscard]] virtual ServiceStatus status() const = 0;
  [[nodiscard]] virtual const std::vector<InstalledVersion>& versions() const = 0;
  /// Supplies provider-specific command-line arguments before it is next started.
  virtual void set_launch_arguments(std::wstring) {}
};
/// Owns providers and supplies stable id-based lookup.
class ServiceRegistry {
public:
  void add(std::unique_ptr<IServiceProvider> provider) { providers_.push_back(std::move(provider)); }
  [[nodiscard]] IServiceProvider* get_by_id(const std::string& id) const {
    for (const auto& provider : providers_) if (provider->id() == id) return provider.get();
    return nullptr;
  }
  [[nodiscard]] const std::vector<std::unique_ptr<IServiceProvider>>& all() const { return providers_; }
  void detect_all() { for (const auto& provider : providers_) provider->detect(); }
private:
  std::vector<std::unique_ptr<IServiceProvider>> providers_;
};
} // namespace appytizer
