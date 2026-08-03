#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace appytizer {
/// Named-pipe command server with persistent clients for Engine event broadcasts.
class PipeServer {
public:
  using Handler = std::function<std::string(std::string_view)>;
  ~PipeServer();
  bool start(Handler handler);
  void stop();
  /// Sends a newline-delimited JSON event to every subscribed UI client.
  void broadcast(std::string_view event);
private:
  void run();
  void serve_client(void* pipe);
  std::atomic_bool running_{};
  std::thread accept_thread_;
  std::mutex clients_mutex_, workers_mutex_;
  std::vector<void*> subscribers_;
  std::vector<std::thread> workers_;
  Handler handler_;
};
} // namespace appytizer
