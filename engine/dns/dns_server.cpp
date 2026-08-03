#include "engine/dns/dns_server.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <array>

namespace appytizer {
namespace { std::uint16_t read16(std::span<const std::uint8_t> b, std::size_t p) { return p + 1 < b.size() ? static_cast<std::uint16_t>((b[p] << 8) | b[p + 1]) : 0; } bool ends_with_ci(std::string value, std::string suffix) { std::ranges::transform(value, value.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); }); std::ranges::transform(suffix, suffix.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); }); return value.size() >= suffix.size() && value.ends_with(suffix); } }
DnsServer::~DnsServer() { stop(); }
std::string DnsServer::decode_question_name(std::span<const std::uint8_t> packet, std::size_t& offset) {
  std::string name; while (offset < packet.size()) { const auto length = packet[offset++]; if (!length) return name; if ((length & 0xc0) || offset + length > packet.size()) return {}; if (!name.empty()) name += '.'; name.append(reinterpret_cast<const char*>(packet.data() + offset), length); offset += length; } return {};
}
std::vector<std::uint8_t> DnsServer::make_response(std::span<const std::uint8_t> query, std::string_view extension) {
  if (query.size() < 12 || read16(query, 4) != 1) return {};
  std::size_t position = 12; const auto name = decode_question_name(query, position); if (name.empty() || position + 4 > query.size()) return {};
  const auto type = read16(query, position); const bool ours = ends_with_ci(name, std::string(extension)); const std::size_t question_end = position + 4;
  std::vector<std::uint8_t> out(query.begin(), query.begin() + static_cast<std::ptrdiff_t>(question_end));
  out[2] = static_cast<std::uint8_t>(0x80 | (query[2] & 0x01)); out[3] = ours ? 0x80 : 0x83; out[6] = 0; out[7] = static_cast<std::uint8_t>(ours && type == 1 ? 1 : 0); out[8] = out[9] = out[10] = out[11] = 0;
  if (ours && type == 1) { const std::array<std::uint8_t,16> answer{0xc0,0x0c,0,1,0,1,0,0,0,5,0,4,127,0,0,1}; out.insert(out.end(), answer.begin(), answer.end()); }
  return out;
}
bool DnsServer::start(std::string extension) {
  if (running_) return true; WSADATA data{}; if (WSAStartup(MAKEWORD(2,2), &data) != 0) return false;
  const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (socket == INVALID_SOCKET) { WSACleanup(); return false; }
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(53); inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) { closesocket(socket); WSACleanup(); return false; }
  extension_ = std::move(extension); socket_ = socket; running_ = true; thread_ = std::thread([this]{ run(); }); return true;
}
void DnsServer::stop() { if (!running_.exchange(false)) return; closesocket(static_cast<SOCKET>(socket_)); if (thread_.joinable()) thread_.join(); socket_ = INVALID_SOCKET; WSACleanup(); }
void DnsServer::run() { std::array<std::uint8_t,512> buffer{}; while (running_) { sockaddr_in peer{}; int length = sizeof(peer); const int count = recvfrom(static_cast<SOCKET>(socket_), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&peer), &length); if (count <= 0) continue; const auto response = make_response(std::span(buffer.data(), static_cast<std::size_t>(count)), extension_); if (!response.empty()) sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(response.data()), static_cast<int>(response.size()), 0, reinterpret_cast<sockaddr*>(&peer), length); } }
} // namespace appytizer
