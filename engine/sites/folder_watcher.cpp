#include "engine/sites/folder_watcher.hpp"
#include "common/win_handle.hpp"
#include <windows.h>
#include <array>

namespace appytizer {
FolderWatcher::~FolderWatcher() { stop(); }
bool FolderWatcher::start(const std::filesystem::path& root, Callback callback) {
  stop();
  if (root.empty() || !std::filesystem::is_directory(root)) return false;
  callback_ = std::move(callback);
  running_ = true;
  thread_ = std::thread([this, root] { run(root); });
  return true;
}
void FolderWatcher::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) CancelSynchronousIo(thread_.native_handle());
  if (thread_.joinable()) thread_.join();
  directory_ = nullptr;
}
void FolderWatcher::run(std::filesystem::path root) {
  WinHandle directory(CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!directory) { running_ = false; return; }
  directory_ = directory.get();
  std::array<std::byte, 4096> buffer{};
  while (running_) {
    DWORD bytes{};
    const BOOL changed = ReadDirectoryChangesW(directory.get(), buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
        FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_FILE_NAME, &bytes, nullptr, nullptr);
    if (!running_) break;
    if (changed && bytes && callback_) callback_();
  }
  directory_ = nullptr;
}
} // namespace appytizer
