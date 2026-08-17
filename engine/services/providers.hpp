#pragma once
#include "common/config.hpp"
#include "common/service_provider.hpp"
#include "common/win_handle.hpp"
#include <mutex>

namespace appytizer {
/// Creates the bundled nginx and PHP, MySQL, and PostgreSQL providers.
void register_builtin_providers(ServiceRegistry& registry, const AppConfig& config);
/// Parses a dotted version from common command output or a filesystem name.
[[nodiscard]] std::string parse_version(std::string_view text);
} // namespace appytizer
