#include "engine/ipc/pipe_server.hpp"
#include "common/constants.hpp"
#include "common/win_handle.hpp"
#include <windows.h>
#include <sddl.h>
#include <array>
#include <algorithm>

namespace appytizer {
PipeServer::~PipeServer() { stop(); }
bool PipeServer::start(Handler handler) {
  if (running_.exchange(true)) return true;
  handler_ = std::move(handler);
  accept_thread_ = std::thread([this] { run(); });
  return true;
}
void PipeServer::stop() {
  if (!running_.exchange(false)) return;
  WinHandle wake(CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
  if (accept_thread_.joinable()) accept_thread_.join();
  std::vector<void*> subscribers;
  { std::scoped_lock lock(clients_mutex_); subscribers = subscribers_; }
  for (void* value : subscribers) DisconnectNamedPipe(static_cast<HANDLE>(value));
  std::scoped_lock lock(workers_mutex_);
  for (auto& worker : workers_) if (worker.joinable()) worker.join();
  workers_.clear();
  { std::scoped_lock client_lock(clients_mutex_); subscribers_.clear(); }
}
void PipeServer::broadcast(std::string_view event) {
  const std::string message = std::string(event) + "\n";
  std::vector<void*> subscribers;
  { std::scoped_lock lock(clients_mutex_); subscribers = subscribers_; }
  for (void* value : subscribers) {
    DWORD written{};
    WriteFile(static_cast<HANDLE>(value), message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
  }
}
void PipeServer::run() {
  PSECURITY_DESCRIPTOR descriptor{};
  ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1, &descriptor, nullptr);
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  while (running_) {
    HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        4, 65536, 65536, 1000, descriptor ? &security : nullptr);
    if (pipe == INVALID_HANDLE_VALUE) break;
    if (!(ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)) { CloseHandle(pipe); continue; }
    std::scoped_lock lock(workers_mutex_);
    workers_.emplace_back([this, pipe] { serve_client(pipe); });
  }
  if (descriptor) LocalFree(descriptor);
}
void PipeServer::serve_client(void* raw_pipe) {
  WinHandle pipe(static_cast<HANDLE>(raw_pipe));
  std::array<char, 4096> buffer{};
  DWORD read{};
  if (!ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || !read) return;
  const std::string request(buffer.data(), read);
  const bool subscribe = request.find("\"cmd\":\"events.subscribe\"") != std::string::npos;
  const std::string answer = handler_(request);
  DWORD written{};
  if (!WriteFile(pipe.get(), answer.data(), static_cast<DWORD>(answer.size()), &written, nullptr)) return;
  if (!subscribe) return;
  { std::scoped_lock lock(clients_mutex_); subscribers_.push_back(pipe.get()); }
  while (running_ && ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read) {}
  std::scoped_lock lock(clients_mutex_);
  std::erase(subscribers_, static_cast<void*>(pipe.get()));
}
} // namespace appytizer
