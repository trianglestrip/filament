/*
 * Compatibility layer for porting Falcor PBRT importer to Filament.
 * SPDX-License-Identifier: Apache-2.0 (parser derived from pbrt-v4)
 */

#pragma once

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <filesystem>
#include <functional>
#include <algorithm>
#include <span>
#include <optional>
#include <variant>
#include <vector>

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>
#include <math/mat4.h>

namespace filament::pbrt {

using Float = float;
using float2 = math::float2;
using float3 = math::float3;
using float4 = math::float4;
using float4x4 = math::mat4f;

inline float4x4 identity4x4() { return float4x4(1.0f); }

#define FALCOR_ASSERT(x) assert(x)
#define FALCOR_UNREACHABLE() assert(false)
#define FALCOR_UNIMPLEMENTED() assert(false)
#define FALCOR_CHECK(cond, ...) assert(cond)

[[noreturn]] inline void falcorThrowMsg(const std::string& msg) {
    throw std::runtime_error(msg);
}

template<typename... Args>
[[noreturn]] inline void FALCOR_THROW(const char* fmt, Args&&...) {
    falcorThrowMsg(fmt);
}

inline bool hasExtension(const std::filesystem::path& path, const char* ext) {
    const auto e = path.extension().string();
    const std::string dotExt = std::string(".") + ext;
    return e == ext || e == dotExt;
}

inline std::string decompressFile(const std::filesystem::path& path) {
    (void)path;
    throw std::runtime_error("gzip decompression is not supported in pbrtio");
}

inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to read file: " + path.string());
    }
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

template<typename... Args>
inline void logInfo(std::format_string<Args...> fmt, Args&&... args) {
    std::fprintf(stderr, "[pbrtio] %s\n",
            std::format(fmt, std::forward<Args>(args)...).c_str());
}

} // namespace filament::pbrt

#include <pbrtio/PbrtSpectrum.h>
