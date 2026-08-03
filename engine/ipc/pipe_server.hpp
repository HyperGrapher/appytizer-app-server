#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace appytizer {
/// Single-client worker-thread named-pipe server for local JSON commands.
class PipeServer {
public:
  using Handler = std::function<std::string(std::string_view)>;
  ~PipeServer();
  bool start(Handler handler);
  void stop();
private:
  void run();
  std::atomic_bool running_{}; std::thread thread_; Handler handler_;
};
} // namespace appytizer
