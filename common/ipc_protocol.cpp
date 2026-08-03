#include "common/ipc_protocol.hpp"
namespace appytizer {
std::optional<IpcRequest> parse_request(std::string_view line) { try { const auto j = nlohmann::json::parse(line); if (!j.is_object() || !j.contains("cmd")) return std::nullopt; return IpcRequest{j.value("id", ""), j.at("cmd").get<std::string>(), j.value("params", nlohmann::json::object())}; } catch (...) { return std::nullopt; } }
std::string response_ok(std::string_view id, const nlohmann::json& result) { return nlohmann::json{{"id", id}, {"ok", true}, {"result", result}}.dump() + "\n"; }
std::string response_error(std::string_view id, std::string_view message) { return nlohmann::json{{"id", id}, {"ok", false}, {"error", message}}.dump() + "\n"; }
} // namespace appytizer
