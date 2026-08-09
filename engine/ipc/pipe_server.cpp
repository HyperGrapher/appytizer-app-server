#include "engine/ipc/pipe_server.hpp"
#include "common/constants.hpp"
#include "common/win_handle.hpp"
#include <windows.h>
#include <sddl.h>
#include <array>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace appytizer {
struct PipeServer::Subscriber {
  HANDLE pipe{};
  std::atomic_bool running{true};
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::string> messages;
  std::thread writer;
};

PipeServer::PipeServer() : pipe_name_(kPipeName) {}
PipeServer::PipeServer(std::wstring pipe_name) : pipe_name_(std::move(pipe_name)) {}
PipeServer::~PipeServer() { stop(); }
bool PipeServer::start(Handler handler) {
  if (running_.exchange(true)) return true;
  handler_ = std::move(handler);
  accept_thread_ = std::thread([this] { run(); });
  return true;
}
void PipeServer::stop() {
  if (!running_.exchange(false)) return;
  WinHandle wake(CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
  if (wake) {
    const char shutdown_request = '\n';
    DWORD written{};
    WriteFile(wake.get(), &shutdown_request, 1, &written, nullptr);
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  std::vector<std::shared_ptr<Subscriber>> subscribers;
  { std::scoped_lock lock(clients_mutex_); subscribers = subscribers_; }
  for (const auto& subscriber : subscribers) {
    subscriber->running = false; subscriber->ready.notify_all();
    DisconnectNamedPipe(subscriber->pipe);
  }
  std::scoped_lock lock(workers_mutex_);
  for (auto& worker : workers_) if (worker.joinable()) worker.join();
  workers_.clear();
  { std::scoped_lock client_lock(clients_mutex_); subscribers_.clear(); }
}
void PipeServer::broadcast(std::string_view event) {
  const std::string message = std::string(event) + "\n";
  std::vector<std::shared_ptr<Subscriber>> subscribers;
  { std::scoped_lock lock(clients_mutex_); subscribers = subscribers_; }
  for (const auto& subscriber : subscribers) {
    { std::scoped_lock lock(subscriber->mutex); subscriber->messages.push_back(message); }
    subscriber->ready.notify_all();
  }
}
void PipeServer::run() {
  PSECURITY_DESCRIPTOR descriptor{};
  ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)(A;;GRGW;;;WD)(A;;GRGW;;;RC)(A;;GRGW;;;AC)S:(ML;;NW;;;LW)",
      SDDL_REVISION_1, &descriptor, nullptr);
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  while (running_) {
    HANDLE pipe = CreateNamedPipeW(pipe_name_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 65536, 65536, 1000, descriptor ? &security : nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (!running_) break;
      spdlog::error("CreateNamedPipeW failed for the Appytizer IPC endpoint (error {}).", GetLastError());
      Sleep(10);
      continue;
    }
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
  bool subscribe = false;
  try { subscribe = nlohmann::json::parse(request).value("cmd", "") == "events.subscribe"; } catch (...) {}
  const std::string answer = handler_(request);
  std::shared_ptr<Subscriber> subscriber;
  if (subscribe) {
    subscriber = std::make_shared<Subscriber>(); subscriber->pipe = pipe.get();
    std::scoped_lock lock(clients_mutex_); subscribers_.push_back(subscriber);
  }
  DWORD written{};
  if (!WriteFile(pipe.get(), answer.data(), static_cast<DWORD>(answer.size()), &written, nullptr)) {
    if (subscriber) { std::scoped_lock lock(clients_mutex_); std::erase(subscribers_, subscriber); }
    return;
  }
  if (!subscribe) return;
  subscriber->writer = std::thread([subscriber] {
    while (subscriber->running) {
      std::string message;
      {
        std::unique_lock lock(subscriber->mutex);
        subscriber->ready.wait(lock, [&] { return !subscriber->running || !subscriber->messages.empty(); });
        if (!subscriber->running && subscriber->messages.empty()) break;
        message = std::move(subscriber->messages.front()); subscriber->messages.pop_front();
      }
      DWORD sent{};
      if (!WriteFile(subscriber->pipe, message.data(), static_cast<DWORD>(message.size()), &sent, nullptr)) {
        subscriber->running = false; subscriber->ready.notify_all(); break;
      }
    }
  });
  {
    std::unique_lock lock(subscriber->mutex);
    subscriber->ready.wait(lock, [&] { return !running_ || !subscriber->running; });
  }
  { std::scoped_lock lock(clients_mutex_); std::erase(subscribers_, subscriber); }
  subscriber->running = false; subscriber->ready.notify_all();
  if (subscriber->writer.joinable()) subscriber->writer.join();
}
} // namespace appytizer
