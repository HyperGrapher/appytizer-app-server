#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace appytizer {
/// Validated newline-delimited JSON request.
struct IpcRequest { std::string id; std::string command; nlohmann::json params = nlohmann::json::object(); };
[[nodiscard]] std::optional<IpcRequest> parse_request(std::string_view line);
[[nodiscard]] std::string response_ok(std::string_view id, const nlohmann::json& result = nlohmann::json::object());
[[nodiscard]] std::string response_error(std::string_view id, std::string_view message);
} // namespace appytizer
