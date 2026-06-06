/*
 * Owns Filament resources built from a parsed PBRT scene (meshes, lights, textures).
 */

#pragma once

#include <pbrtio/PbrtSceneLoader.h>

#include <utils/Entity.h>

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace filament {
class Engine;
class IndexBuffer;
class Material;
class MaterialInstance;
class Scene;
class Texture;
class VertexBuffer;
} // namespace filament

namespace filament::pbrtio {

using ::pbrtio::PbrtCameraSettings;
using ::pbrtio::PbrtDecodedImage;
using ::pbrtio::PbrtLightType;
using ::pbrtio::PbrtMeshInstance;
using ::pbrtio::PbrtRectAreaLight;
using ::pbrtio::PbrtSceneResources;
using ::pbrtio::loadPbrtSceneResources;
using ::pbrtio::printPbrtLoadTimings;

struct PbrtFilamentMaterialPackages {
    const void* litData = nullptr;
    size_t litSize = 0;
    const void* texturedData = nullptr;
    size_t texturedSize = 0;
    const void* unlitData = nullptr;
    size_t unlitSize = 0;
};

struct PbrtFilamentBuildOptions {
    PbrtFilamentMaterialPackages materials;
    bool spawnAnalyticLights = true;
    bool loadEnvironmentTexture = true;
};

struct PbrtFilamentBuildResult {
    size_t meshesLoaded = 0;
    size_t analyticLightCount = 0;
    size_t rectAreaLightCount = 0;
    double meshImportMs = 0;
};

class PbrtFilamentSceneHost {
public:
    PbrtFilamentSceneHost();
    ~PbrtFilamentSceneHost();

    PbrtFilamentSceneHost(const PbrtFilamentSceneHost&) = delete;
    PbrtFilamentSceneHost& operator=(const PbrtFilamentSceneHost&) = delete;

    bool build(filament::Engine& engine, filament::Scene& scene,
            const std::filesystem::path& pbrtPath,
            const PbrtFilamentBuildOptions& options = {});

    void destroy(filament::Engine& engine, filament::Scene& scene);

    const PbrtSceneResources& scene() const { return mScene; }
    const PbrtFilamentBuildResult& result() const { return mResult; }
    bool isBuilt() const { return mBuilt; }

private:
    struct RectLightAsset {
        utils::Entity meshEntity{};
        utils::Entity lightEntity{};
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
        filament::Material* material = nullptr;
        filament::MaterialInstance* mi = nullptr;
    };

    struct MeshAsset {
        utils::Entity entity{};
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
    };

    filament::MaterialInstance* getOrCreateMaterial(filament::Engine& engine,
            const PbrtMeshInstance& mesh);

    RectLightAsset createRectAreaLight(filament::Engine& engine, filament::Scene& scene,
            const PbrtRectAreaLight& rect);

    PbrtSceneResources mScene;
    PbrtFilamentBuildOptions mOptions;
    PbrtFilamentBuildResult mResult;
    std::map<std::string, filament::MaterialInstance*> mMaterialInstances;
    std::vector<filament::Material*> mOwnedMaterials;
    filament::Material* mLitMaterial = nullptr;
    filament::Material* mTexturedMaterial = nullptr;
    filament::Material* mUnlitMaterial = nullptr;
    std::unordered_map<std::string, filament::Texture*> mGpuTextures;
    std::vector<filament::Texture*> mOwnedTextures;
    filament::Texture* mWhiteTexture = nullptr;
    std::vector<MeshAsset> mMeshAssets;
    std::vector<utils::Entity> mAnalyticLights;
    std::vector<RectLightAsset> mRectLights;
    bool mBuilt = false;
};

} // namespace filament::pbrtio
