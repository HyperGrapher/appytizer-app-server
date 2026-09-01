#pragma once

#include <windows.h>
#include <winsvc.h>

#include <string_view>

namespace appytizer {

enum class ServiceStartResult {
  running,
  accessDenied,
  notInstalled,
  failed,
};

[[nodiscard]] ServiceStartResult
startWindowsService(std::wstring_view serviceName);

// Grants interactive users only enough access to start the service and observe
// startup.
[[nodiscard]] bool grantServiceStartAccess(SC_HANDLE service);

} // namespace appytizer
