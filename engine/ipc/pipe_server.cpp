#include "engine/ipc/pipe_server.hpp"
#include "common/constants.hpp"
#include "common/win_handle.hpp"
#include <windows.h>
#include <sddl.h>
#include <array>

namespace appytizer {
PipeServer::~PipeServer() { stop(); }
bool PipeServer::start(Handler handler) {
  if (running_) return true;
  handler_ = std::move(handler); running_ = true;
  thread_ = std::thread([this] { run(); }); return true;
}
void PipeServer::stop() {
  if (!running_.exchange(false)) return;
  WinHandle wake(CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
  if (thread_.joinable()) thread_.join();
}
void PipeServer::run() {
  PSECURITY_DESCRIPTOR descriptor{};
  ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1, &descriptor, nullptr);
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  while (running_) {
    WinHandle pipe(CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        4, 65536, 65536, 1000, descriptor ? &security : nullptr));
    if (!pipe) break;
    if (!(ConnectNamedPipe(pipe.get(), nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)) continue;
    std::string pending; std::array<char, 4096> buffer{};
    while (running_) {
      DWORD read{};
      if (!ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || !read) break;
      pending.append(buffer.data(), read);
      for (std::size_t newline; (newline = pending.find('\n')) != std::string::npos;) {
        const auto answer = handler_(std::string_view(pending).substr(0, newline)); DWORD written{};
        WriteFile(pipe.get(), answer.data(), static_cast<DWORD>(answer.size()), &written, nullptr);
        pending.erase(0, newline + 1);
      }
    }
    DisconnectNamedPipe(pipe.get());
  }
  if (descriptor) LocalFree(descriptor);
}
} // namespace appytizer
