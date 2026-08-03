#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace appytizer {
/// Short-lived named-pipe client. Callbacks are always dispatched on FLTK's UI thread.
class PipeClient {
public:
  using Callback = std::function<void(std::string)>;
  void request(std::string command, std::string params_json, Callback callback);
  [[nodiscard]] bool connected() const { return connected_; }
private: std::atomic_bool connected_{};
};
} // namespace appytizer
