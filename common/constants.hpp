#pragma once
#include <string_view>

namespace appytizer {
inline constexpr wchar_t kServiceName[] = L"AppytizerEngine";
inline constexpr wchar_t kServiceDisplayName[] = L"Appytizer App Server Engine";
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\AppytizerEngine";
inline constexpr wchar_t kApplicationId[] = L"Appytizer";
inline constexpr std::string_view kDefaultExtension = ".test";
} // namespace appytizer
