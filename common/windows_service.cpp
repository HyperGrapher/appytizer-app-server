#include "common/windows_service.hpp"

#include "common/win_handle.hpp"

#include <aclapi.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace appytizer {
namespace {

struct LocalFreeDeleter {
  void operator()(void *value) const noexcept { LocalFree(value); }
};

} // namespace

ServiceStartResult startWindowsService(std::wstring_view serviceName) {
  const std::wstring terminatedName(serviceName);
  ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager) {
    const DWORD error = GetLastError();
    return error == ERROR_ACCESS_DENIED
               ? ServiceStartResult::accessDenied
               : ServiceStartResult::failed;
  }

  ServiceHandle service(OpenServiceW(manager.get(), terminatedName.c_str(),
                                     SERVICE_START | SERVICE_QUERY_STATUS));
  if (!service) {
    const DWORD error = GetLastError();
    if (error == ERROR_ACCESS_DENIED) {
      return ServiceStartResult::accessDenied;
    }
    return error == ERROR_SERVICE_DOES_NOT_EXIST
               ? ServiceStartResult::notInstalled
               : ServiceStartResult::failed;
  }

  for (int attempt = 0; attempt < 150; ++attempt) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded{};
    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE *>(&status), sizeof(status),
                              &bytesNeeded)) {
      return ServiceStartResult::failed;
    }
    if (status.dwCurrentState == SERVICE_RUNNING ||
        status.dwCurrentState == SERVICE_START_PENDING) {
      return ServiceStartResult::running;
    }
    if (status.dwCurrentState == SERVICE_STOPPED) {
      if (!StartServiceW(service.get(), 0, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_SERVICE_ALREADY_RUNNING) {
          return ServiceStartResult::running;
        }
        return error == ERROR_ACCESS_DENIED
                   ? ServiceStartResult::accessDenied
                   : ServiceStartResult::failed;
      }
      return ServiceStartResult::running;
    }
    if (status.dwCurrentState != SERVICE_STOP_PENDING) {
      return ServiceStartResult::failed;
    }
    Sleep(100);
  }
  return ServiceStartResult::failed;
}

bool grantServiceStartAccess(SC_HANDLE service) {
  DWORD descriptorSize{};
  QueryServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, nullptr, 0,
                             &descriptorSize);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || descriptorSize == 0) {
    return false;
  }

  std::vector<BYTE> descriptor(descriptorSize);
  if (!QueryServiceObjectSecurity(
          service, DACL_SECURITY_INFORMATION,
          reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()),
          descriptorSize, &descriptorSize)) {
    return false;
  }

  PACL existingAcl{};
  BOOL isAclPresent{};
  BOOL isAclDefaulted{};
  if (!GetSecurityDescriptorDacl(
          reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()),
          &isAclPresent, &existingAcl, &isAclDefaulted) ||
      !isAclPresent) {
    return false;
  }

  std::array<BYTE, SECURITY_MAX_SID_SIZE> sidStorage{};
  DWORD sidSize = static_cast<DWORD>(sidStorage.size());
  PSID interactiveUsers = sidStorage.data();
  if (!CreateWellKnownSid(WinInteractiveSid, nullptr, interactiveUsers,
                          &sidSize)) {
    return false;
  }

  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = SERVICE_START | SERVICE_QUERY_STATUS;
  access.grfAccessMode = GRANT_ACCESS;
  access.grfInheritance = NO_INHERITANCE;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
  access.Trustee.ptstrName = static_cast<LPWSTR>(interactiveUsers);

  PACL updatedAcl{};
  if (SetEntriesInAclW(1, &access, existingAcl, &updatedAcl) != ERROR_SUCCESS) {
    return false;
  }
  const std::unique_ptr<ACL, LocalFreeDeleter> ownedAcl(updatedAcl);

  SECURITY_DESCRIPTOR updatedDescriptor{};
  if (!InitializeSecurityDescriptor(&updatedDescriptor,
                                    SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorDacl(&updatedDescriptor, TRUE, updatedAcl, FALSE)) {
    return false;
  }
  return SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION,
                                  &updatedDescriptor) != FALSE;
}

} // namespace appytizer
