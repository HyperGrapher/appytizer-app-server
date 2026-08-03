#include "common/ipc_protocol.hpp"
#include "engine/dns/dns_server.hpp"
#include "engine/services/providers.hpp"
#include <array>
#include <iostream>

int main() {
  int failures = 0;
  const auto request = appytizer::parse_request(R"({"id":"7","cmd":"sites.list"})");
  if (!request || request->id != "7" || request->command != "sites.list") ++failures;
  if (appytizer::parse_version("PHP 8.4.12 (cgi-fcgi)") != "8.4.12") ++failures;
  const std::array<std::uint8_t,27> query{0x12,0x34,0x01,0,0,1,0,0,0,0,0,0,3,'a','p','p',5,'l','o','c','a','l',0,0,1,0,1};
  const auto response = appytizer::DnsServer::make_response(query, ".local");
  if (response.size() != query.size() + 16 || response[7] != 1 || response.back() != 1) ++failures;
  if (failures) std::cerr << failures << " test(s) failed\n";
  return failures ? 1 : 0;
}
