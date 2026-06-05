/*
 * Math helpers for PBRT scene builder (Filament port).
 */
#pragma once

#include <pbrtio/PbrtCompat.h>

#include <math/mat3.h>
#include <math/mat4.h>
#include <math/quat.h>
#include <math/scalar.h>
#include <sstream>

namespace filament::pbrt {

inline float radians(float degrees) {
    return degrees * float(M_PI) / 180.f;
}

inline float4x4 matrixFromRotation(float angleRad, float3 axis) {
    axis = normalize(axis);
    return float4x4(filament::math::quatf::fromAxisAngle(axis, angleRad));
}

inline float4x4 matrixFromScaling(float3 s) {
    return float4x4::scaling(s);
}

inline float4x4 matrixFromLookAt(float3 eye, float3 at, float3 up) {
    return float4x4::lookAt(eye, at, up);
}

inline float4x4 float4x4FromPbrtRowMajor(const Float* tr) {
    return float4x4(tr[0], tr[1], tr[2], tr[3], tr[4], tr[5], tr[6], tr[7], tr[8], tr[9], tr[10], tr[11],
            tr[12], tr[13], tr[14], tr[15]);
}

inline float4x4 pbrtRowMajorTransform(const Float* tr) {
    return transpose(float4x4FromPbrtRowMajor(tr));
}

inline std::string to_string(const float4x4& m) {
    (void)m;
    return "mat4";
}

} // namespace filament::pbrt
