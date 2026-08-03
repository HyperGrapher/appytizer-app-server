// FLTK is not thread-safe: all pipe callbacks are marshaled through Fl::awake.
#include "appytizer-ui/ipc/pipe_client.hpp"
#include "common/constants.hpp"
#include "common/win_handle.hpp"
#include <FL/Fl.H>
#include <windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <memory>

namespace appytizer {
namespace {
struct CallbackTask { PipeClient::Callback callback; std::string payload; };
void dispatch(PipeClient::Callback callback, std::string payload) {
  auto* task = new CallbackTask{std::move(callback), std::move(payload)};
  Fl::awake([](void* data) {
    std::unique_ptr<CallbackTask> task(static_cast<CallbackTask*>(data));
    task->callback(std::move(task->payload));
  }, task);
}
}

PipeClient::~PipeClient() {
  listening_ = false;
  if (listener_.joinable()) CancelSynchronousIo(listener_.native_handle());
  if (listener_.joinable()) listener_.join();
}
void PipeClient::request(std::string command, std::string params_json, Callback callback) {
  std::thread([this, command = std::move(command), params_json = std::move(params_json), callback = std::move(callback)]() mutable {
    static std::atomic_uint64_t next{1}; nlohmann::json params = nlohmann::json::object();
    try { params = nlohmann::json::parse(params_json); } catch (...) {}
    const auto message = nlohmann::json{{"id", std::to_string(next++)}, {"cmd", command}, {"params", params}}.dump() + "\n";
    WinHandle pipe(CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
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
  WinHandle pipe(CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
  if (!pipe) { connected_ = false; listening_ = false; return; }
  { std::scoped_lock lock(listener_mutex_); listener_pipe_ = pipe.get(); }
  const std::string subscribe = R"({"id":"events","cmd":"events.subscribe","params":{}})" "\n";
  DWORD written{}, read{}; std::array<char, 65536> buffer{};
  if (!WriteFile(pipe.get(), subscribe.data(), static_cast<DWORD>(subscribe.size()), &written, nullptr) ||
      !ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
    connected_ = false;
  } else {
    connected_ = true;
    while (listening_ && ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
      dispatch(callback, std::string(buffer.data(), read));
  }
  { std::scoped_lock lock(listener_mutex_); listener_pipe_ = nullptr; }
  listening_ = false;
}
} // namespace appytizer
