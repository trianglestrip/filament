#pragma once
#include <format>
#include <string>

// fmt::format shim for ported Falcor PBRT code (C++20).
namespace fmt {
template<typename... Args>
std::string format(std::format_string<Args...> fmt, Args&&... args) {
    return std::format(fmt, std::forward<Args>(args)...);
}
} // namespace fmt
