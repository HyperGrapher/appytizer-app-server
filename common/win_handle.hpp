#pragma once
#include <windows.h>
#include <winsvc.h>
#include <utility>

namespace appytizer {
/// Move-only owner for a Win32 HANDLE.
class WinHandle {
public:
  WinHandle() = default;
  explicit WinHandle(HANDLE value) noexcept : value_(value) {}
  ~WinHandle() { reset(); }
  WinHandle(const WinHandle&) = delete;
  WinHandle& operator=(const WinHandle&) = delete;
  WinHandle(WinHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  WinHandle& operator=(WinHandle&& other) noexcept { if (this != &other) reset(std::exchange(other.value_, nullptr)); return *this; }
  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
  HANDLE release() noexcept { return std::exchange(value_, nullptr); }
  void reset(HANDLE value = nullptr) noexcept { if (*this) CloseHandle(value_); value_ = value; }
private:
  HANDLE value_{};
};
/// Move-only owner for an SCM handle.
class ServiceHandle {
public:
  ServiceHandle() = default;
  explicit ServiceHandle(SC_HANDLE value) noexcept : value_(value) {}
  ~ServiceHandle() { reset(); }
  ServiceHandle(const ServiceHandle&) = delete;
  ServiceHandle& operator=(const ServiceHandle&) = delete;
  ServiceHandle(ServiceHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  ServiceHandle& operator=(ServiceHandle&& other) noexcept { if (this != &other) reset(std::exchange(other.value_, nullptr)); return *this; }
  [[nodiscard]] SC_HANDLE get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
  void reset(SC_HANDLE value = nullptr) noexcept { if (value_) CloseServiceHandle(value_); value_ = value; }
private:
  SC_HANDLE value_{};
};
} // namespace appytizer
