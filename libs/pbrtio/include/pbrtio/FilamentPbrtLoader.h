/*
 * Load a parsed PBRT scene into Filament-friendly mesh instances.
 * Parser derived from Falcor PBRTImporter / pbrt-v4.
 */
#pragma once

#include <math/mat4.h>
#include <math/vec3.h>

#include <filesystem>
#include <string>
#include <vector>

namespace filament::pbrtio {

struct PbrtMeshInstance {
    std::filesystem::path plyPath;
    math::mat4f transform;
    math::float3 baseColor{ 0.75f, 0.75f, 0.75f };
    float roughness = 0.45f;
    float metallic = 0.f;
    std::string materialName;
    /** Resolved imagemap path for spectrum reflectance textures (empty = solid color only). */
    std::filesystem::path baseColorTexturePath;
    bool baseColorTextureSRGB = true;
};

struct PbrtLoadedScene {
    std::filesystem::path searchPath;
    std::vector<PbrtMeshInstance> meshes;
    math::mat4f cameraTransform = math::mat4f(1.0f);
    float cameraFov = 45.f;
    math::float3 sceneCenter{ 0.f };
    float sceneRadius = 1.f;
};

bool loadPbrtScene(const std::filesystem::path& pbrtPath, PbrtLoadedScene& out);

} // namespace filament::pbrtio
