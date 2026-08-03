// FLTK is not thread-safe: all pipe callbacks are marshaled through Fl::awake.
#include "appytizer-ui/ipc/pipe_client.hpp"
#include "common/constants.hpp"
#include "common/win_handle.hpp"
#include <FL/Fl.H>
#include <windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <memory>

namespace appytizer {
namespace {
struct CallbackTask { PipeClient::Callback callback; std::string payload; };
WinHandle connect_pipe() {
  for (int attempt = 0; attempt < 4; ++attempt) {
    WinHandle pipe(CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (pipe) return pipe;
    if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(kPipeName, 1000)) break;
  }
  return {};
}
void dispatch(PipeClient::Callback callback, std::string payload) {
  auto* task = new CallbackTask{std::move(callback), std::move(payload)};
  Fl::awake([](void* data) {
    std::unique_ptr<CallbackTask> task(static_cast<CallbackTask*>(data));
    task->callback(std::move(task->payload));
  }, task);
}
}

PipeClient::PipeClient() : stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
PipeClient::~PipeClient() {
  listening_ = false;
  if (stop_event_) SetEvent(static_cast<HANDLE>(stop_event_));
  if (listener_.joinable()) CancelSynchronousIo(listener_.native_handle());
  if (listener_.joinable()) listener_.join();
  if (stop_event_) CloseHandle(static_cast<HANDLE>(stop_event_));
}
void PipeClient::request(std::string command, std::string params_json, Callback callback) {
  std::thread([this, command = std::move(command), params_json = std::move(params_json), callback = std::move(callback)]() mutable {
    static std::atomic_uint64_t next{1}; nlohmann::json params = nlohmann::json::object();
    try { params = nlohmann::json::parse(params_json); } catch (...) {}
    const auto message = nlohmann::json{{"id", std::to_string(next++)}, {"cmd", command}, {"params", params}}.dump() + "\n";
    WinHandle pipe = connect_pipe();
    std::string answer;
    if (pipe) {
      connected_ = true; DWORD written{}, read{}; std::array<char, 65536> buffer{};
      if (WriteFile(pipe.get(), message.data(), static_cast<DWORD>(message.size()), &written, nullptr) &&
          ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) answer.assign(buffer.data(), read);
    } else { connected_ = false; answer = R"({"ok":false,"error":"Engine is not connected"})"; }
    dispatch(std::move(callback), std::move(answer));
  }).detach();
}
void PipeClient::subscribe(Callback callback) {
  if (listening_.exchange(true)) return;
  listener_ = std::thread([this, callback = std::move(callback)]() mutable { listen(std::move(callback)); });
}
void PipeClient::listen(Callback callback) {
  const std::string subscribe = R"({"id":"events","cmd":"events.subscribe","params":{}})" "\n";
  DWORD reconnect_delay = 200;
  bool offline_announced = false;
  while (listening_) {
    WinHandle pipe = connect_pipe();
    if (!pipe) {
      connected_ = false;
      if (!offline_announced) { dispatch(callback, R"({"event":"engine.disconnected"})"); offline_announced = true; }
      if (WaitForSingleObject(static_cast<HANDLE>(stop_event_), reconnect_delay) == WAIT_OBJECT_0) break;
      reconnect_delay = (std::min)(reconnect_delay * 2, 5000UL);
      continue;
    }
    { std::scoped_lock lock(listener_mutex_); listener_pipe_ = pipe.get(); }
    DWORD written{}, read{}; std::array<char, 65536> buffer{};
    const bool subscribed = WriteFile(pipe.get(), subscribe.data(), static_cast<DWORD>(subscribe.size()), &written, nullptr) &&
        ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
    if (subscribed) {
      connected_ = true; offline_announced = false; reconnect_delay = 200;
      dispatch(callback, R"({"event":"engine.connected"})");
      while (listening_ && ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
        dispatch(callback, std::string(buffer.data(), read));
    }
    { std::scoped_lock lock(listener_mutex_); listener_pipe_ = nullptr; }
    connected_ = false;
    if (listening_ && !offline_announced) { dispatch(callback, R"({"event":"engine.disconnected"})"); offline_announced = true; }
  }
  listening_ = false;
}
} // namespace appytizer
