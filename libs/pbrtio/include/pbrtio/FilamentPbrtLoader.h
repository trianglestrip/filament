/*
 * Load a parsed PBRT scene into Filament-friendly mesh instances.
 * Parser derived from Falcor PBRTImporter / pbrt-v4.
 */
#pragma once

#include <math/mat4.h>
#include <math/vec3.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace filament {
class Engine;
class Texture;
} // namespace filament

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
    /** Set by parallel mesh verification during loadPbrtFilamentScene. */
    bool plyFileExists = true;
};

enum class PbrtLightType {
    Directional,
    Point,
};

struct PbrtLightInstance {
    PbrtLightType type = PbrtLightType::Directional;
    math::float3 color{ 1.f };
    float intensity = 1.f;
    math::float3 position{ 0.f };
    math::float3 direction{ 0.f, -1.f, 0.f };
    bool castShadows = true;
};

/**
 * PBRT rect area light (AreaLightSource "diffuse" on trianglemesh/bilinearmesh).
 * emissiveColor is spectrum radiance L (W/(m^2*sr)); geometry is a world-space quad.
 * Use LightManager::Type::RECT (oriented quad emitter) plus optional emissive mesh.
 */
struct PbrtRectAreaLight {
    math::float3 positions[4]{};
    uint16_t indices[6]{ 0, 1, 2, 0, 2, 3 };
    math::float3 normal{ 0.f, 1.f, 0.f };
    math::float3 center{ 0.f };
    float width = 1.f;
    float height = 1.f;
    math::float3 radiance{ 1.f };
    float scale = 1.f;
};

using PbrtAreaLightMesh = PbrtRectAreaLight;

/** PBRT Light "infinite" environment map (equirectangular HDR/EXR/etc.). */
struct PbrtEnvironmentLight {
    std::filesystem::path mapPath;
    float scale = 1.f;
    bool valid = false;
};

struct PbrtCameraSettings {
    math::float3 eye{ 0.f, 0.f, 5.f };
    math::float3 target{ 0.f, 0.f, 0.f };
    math::float3 up{ 0.f, 1.f, 0.f };
    float verticalFovDegrees = 45.f;
    float aspectRatio = 16.f / 9.f;
    float nearPlane = 0.01f;
    float farPlane = 100.f;
};

struct PbrtLoadedScene {
    std::filesystem::path searchPath;
    std::vector<PbrtMeshInstance> meshes;
    std::vector<PbrtLightInstance> lights;
    std::vector<PbrtRectAreaLight> areaLights;
    PbrtEnvironmentLight environment;
    PbrtCameraSettings camera;
    math::float3 sceneCenter{ 0.f };
    float sceneRadius = 1.f;
};

/** Decoded RGBA pixels; buffer owned until GPU upload or destroyPbrtFilamentTextures. */
struct PbrtDecodedImage {
    uint8_t* pixels = nullptr;
    size_t byteSize = 0;
    int width = 0;
    int height = 0;
    bool sRGB = true;
    bool valid = false;

    PbrtDecodedImage() = default;
    PbrtDecodedImage(PbrtDecodedImage&& other) noexcept;
    PbrtDecodedImage& operator=(PbrtDecodedImage&& other) noexcept;
    ~PbrtDecodedImage();

    PbrtDecodedImage(const PbrtDecodedImage&) = delete;
    PbrtDecodedImage& operator=(const PbrtDecodedImage&) = delete;
};

struct PbrtFilamentTextures {
    std::unordered_map<std::string, PbrtDecodedImage> decoded;
    std::unordered_map<std::string, filament::Texture*> gpu;
    std::vector<filament::Texture*> owned;
};

struct PbrtLoadTimings {
    double parseMs = 0;
    double collectMs = 0;
    double decodeTexturesMs = 0;
    double verifyMeshesMs = 0;
    double uploadTexturesMs = 0;
    double totalMs = 0;
};

struct PbrtFilamentScene {
    PbrtLoadedScene scene;
    PbrtFilamentTextures textures;
    PbrtLoadTimings timings;
};

/** Parse PBRT file into scene description (meshes, lights, camera). */
bool loadPbrtScene(const std::filesystem::path& pbrtPath, PbrtLoadedScene& out);

/**
 * Full load pipeline: parse, taskflow-parallel texture decode + mesh verify, optional GPU upload.
 * When engine is null, only CPU decode is performed.
 */
bool loadPbrtFilamentScene(const std::filesystem::path& pbrtPath, filament::Engine* engine,
        PbrtFilamentScene& out, bool loadEnvironmentTexture = true);

void destroyPbrtFilamentTextures(filament::Engine& engine, PbrtFilamentTextures& textures);

void printPbrtLoadTimings(const PbrtLoadTimings& timings);

} // namespace filament::pbrtio
