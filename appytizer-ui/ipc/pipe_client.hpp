#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace appytizer {
/// Named-pipe client. Every callback is marshaled onto FLTK's UI thread.
class PipeClient {
public:
  using Callback = std::function<void(std::string)>;
  PipeClient() = default;
  ~PipeClient();
  PipeClient(const PipeClient&) = delete;
  PipeClient& operator=(const PipeClient&) = delete;
  void request(std::string command, std::string params_json, Callback callback);
  /// Opens one persistent Engine subscription for unsolicited state-change events.
  void subscribe(Callback callback);
  [[nodiscard]] bool connected() const { return connected_; }
private:
  void listen(Callback callback);
  std::atomic_bool connected_{}, listening_{};
  std::mutex listener_mutex_;
  void* listener_pipe_{};
  std::thread listener_;
};
} // namespace appytizer
