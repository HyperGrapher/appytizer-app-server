#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <thread>

namespace appytizer {
/// Watches a project root and invokes its callback when a direct child changes.
class FolderWatcher {
public:
  using Callback = std::function<void()>;
  ~FolderWatcher();
  bool start(const std::filesystem::path& root, Callback callback);
  void stop();
private:
  void run(std::filesystem::path root);
  std::atomic_bool running_{};
  void* directory_{};
  std::thread thread_;
  Callback callback_;
};
} // namespace appytizer
