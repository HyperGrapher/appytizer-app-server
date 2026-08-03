#pragma once
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace appytizer {
/// Minimal loopback-only authoritative DNS responder for the configured suffix.
class DnsServer {
public:
  DnsServer() = default;
  ~DnsServer();
  DnsServer(const DnsServer&) = delete;
  DnsServer& operator=(const DnsServer&) = delete;
  bool start(std::string extension);
  void stop();
  [[nodiscard]] bool running() const { return running_; }
  [[nodiscard]] static std::string decode_question_name(std::span<const std::uint8_t> packet, std::size_t& offset);
  [[nodiscard]] static std::vector<std::uint8_t> make_response(std::span<const std::uint8_t> query, std::string_view extension);
private:
  void run();
  std::atomic_bool running_{};
  std::string extension_{".local"};
  std::thread thread_;
  std::uintptr_t socket_{~std::uintptr_t{0}};
};
} // namespace appytizer
