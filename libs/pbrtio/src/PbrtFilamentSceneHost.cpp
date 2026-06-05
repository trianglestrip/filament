#include <pbrtio/PbrtFilamentSceneHost.h>

#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>

#include <filamentapp/MeshAssimp.h>

#include <utils/EntityManager.h>
#include <utils/Path.h>

#include <cmath>
#include <iostream>
#include <map>

namespace filament::pbrtio {
namespace {

using namespace filament::math;
using utils::Entity;
using utils::EntityManager;

mat4f buildRectLightTransform(const PbrtRectAreaLight& rect) {
    const float3& p0 = rect.positions[0];
    const float3& p1 = rect.positions[1];
    const float3& p3 = rect.positions[3];
    const float3 center = (rect.positions[0] + rect.positions[1] + rect.positions[2] +
            rect.positions[3]) * 0.25f;
    const float3 edge1 = (p1 - p0) * 0.5f;
    const float3 edge2 = (p3 - p0) * 0.5f;
    const float3 normal = normalize(cross(p1 - p0, p3 - p0));
    return mat4f(
            float4(edge1, 0.f),
            float4(edge2, 0.f),
            float4(normal, 0.f),
            float4(center, 1.f));
}

} // namespace

PbrtFilamentSceneHost::PbrtFilamentSceneHost() = default;

PbrtFilamentSceneHost::~PbrtFilamentSceneHost() {
    if (mBuilt) {
        std::cerr << "PbrtFilamentSceneHost destroyed without calling destroy()" << std::endl;
    }
}

MaterialInstance* PbrtFilamentSceneHost::getOrCreateMaterial(Engine& engine,
        const PbrtMeshInstance& mesh) {
    const bool textured = !mesh.baseColorTexturePath.empty();
    const std::string key = textured
            ? (mesh.materialName + "|" + mesh.baseColorTexturePath.string())
            : (mesh.materialName.empty()
                    ? (std::to_string(mesh.baseColor.x) + "," +
                       std::to_string(mesh.baseColor.y) + "," +
                       std::to_string(mesh.baseColor.z))
                    : mesh.materialName);

    auto it = mMaterialInstances.find(key);
    if (it != mMaterialInstances.end()) {
        return it->second;
    }

    MaterialInstance* mi = nullptr;
    if (textured && mOptions.materials.texturedData && mOptions.materials.texturedSize > 0) {
        const std::string texKey = mesh.baseColorTexturePath.string();
        auto texIt = mScene.textures.gpu.find(texKey);
        if (texIt != mScene.textures.gpu.end() && texIt->second) {
            Material* material = Material::Builder()
                    .package(mOptions.materials.texturedData, mOptions.materials.texturedSize)
                    .build(engine);
            mOwnedMaterials.push_back(material);
            mi = material->createInstance();
            TextureSampler sampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
                    TextureSampler::MagFilter::LINEAR,
                    TextureSampler::WrapMode::REPEAT);
            mi->setParameter("baseColorMap", texIt->second, sampler);
            mi->setParameter("baseColor", RgbType::LINEAR, float3(1.f));
            mi->setParameter("roughness", mesh.roughness);
            mi->setParameter("metallic", mesh.metallic);
        }
    }

    if (!mi && mOptions.materials.litData && mOptions.materials.litSize > 0) {
        Material* material = Material::Builder()
                .package(mOptions.materials.litData, mOptions.materials.litSize)
                .build(engine);
        mOwnedMaterials.push_back(material);
        mi = material->createInstance();
        mi->setParameter("baseColor", RgbType::LINEAR, mesh.baseColor);
        mi->setParameter("roughness", mesh.roughness);
        mi->setParameter("metallic", mesh.metallic);
    }

    if (mi) {
        mMaterialInstances[key] = mi;
    }
    return mi;
}

PbrtFilamentSceneHost::RectLightAsset PbrtFilamentSceneHost::createRectAreaLight(Engine& engine,
        Scene& scene, const PbrtRectAreaLight& rect) {
    float3 vertices[4];
    for (int i = 0; i < 4; ++i) {
        vertices[i] = rect.positions[i];
    }

    VertexBuffer* vb = VertexBuffer::Builder()
            .vertexCount(4)
            .bufferCount(1)
            .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3)
            .build(engine);
    vb->setBufferAt(engine, 0, VertexBuffer::BufferDescriptor(vertices, sizeof(vertices)));

    IndexBuffer* ib = IndexBuffer::Builder()
            .indexCount(6)
            .bufferType(IndexBuffer::IndexType::USHORT)
            .build(engine);
    ib->setBuffer(engine, IndexBuffer::BufferDescriptor(rect.indices, sizeof(rect.indices)));

    Material* material = nullptr;
    MaterialInstance* mi = nullptr;
    if (mOptions.materials.unlitData && mOptions.materials.unlitSize > 0) {
        material = Material::Builder()
                .package(mOptions.materials.unlitData, mOptions.materials.unlitSize)
                .build(engine);
        mi = material->createInstance();
        const float3 radiance = rect.radiance * rect.scale;
        mi->setParameter("baseColor", RgbType::LINEAR, float3(0.f));
        mi->setParameter("emissive", float4(radiance, 1.f));
    }

    Box aabb = RenderableManager::computeAABB(vertices, rect.indices, 6);
    Entity meshEntity = EntityManager::get().create();
    RenderableManager::Builder(1)
            .boundingBox(aabb)
            .material(0, mi)
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, vb, ib)
            .culling(false)
            .build(engine, meshEntity);
    scene.addEntity(meshEntity);

    const float3 radiance = rect.radiance * rect.scale;
    const float quadArea = std::max(rect.width * rect.height, 1e-4f);
    const float luminance = (radiance.x + radiance.y + radiance.z) / 3.f;
    const float lumens = luminance * quadArea * float(M_PI);

    Entity lightEntity = EntityManager::get().create();
    auto& tcm = engine.getTransformManager();
    tcm.create(lightEntity);
    tcm.setTransform(tcm.getInstance(lightEntity), buildRectLightTransform(rect));
    LightManager::Builder(LightManager::Type::RECT)
            .color(radiance)
            .intensity(lumens)
            .falloff(50.f)
            .castShadows(false)
            .build(engine, lightEntity);
    scene.addEntity(lightEntity);

    return RectLightAsset{ meshEntity, lightEntity, vb, ib, material, mi };
}

bool PbrtFilamentSceneHost::build(Engine& engine, Scene& scene,
        const std::filesystem::path& pbrtPath, const PbrtFilamentBuildOptions& options) {
    if (mBuilt) {
        destroy(engine, scene);
    }

    mOptions = options;
    mResult = {};
    mScene = {};

    if (!loadPbrtFilamentScene(pbrtPath, &engine, mScene)) {
        return false;
    }

    mMeshLoader = std::make_unique<MeshAssimp>(engine);
    auto& tcm = engine.getTransformManager();
    auto& rcm = engine.getRenderableManager();
    auto& em = EntityManager::get();

    for (const auto& mesh : mScene.scene.meshes) {
        if (!mesh.plyFileExists) {
            std::cerr << "Missing mesh: " << mesh.plyPath << std::endl;
            continue;
        }
        MaterialInstance* mi = getOrCreateMaterial(engine, mesh);
        if (!mi) {
            std::cerr << "Failed to create material for: " << mesh.plyPath << std::endl;
            continue;
        }
        std::map<std::string, MaterialInstance*> matMap;
        matMap["DefaultMaterial"] = mi;
        const size_t before = mMeshLoader->getRenderables().size();
        mMeshLoader->addFromFile(utils::Path(mesh.plyPath.string()), matMap, true);
        const auto& renderables = mMeshLoader->getRenderables();
        if (renderables.size() == before) {
            std::cerr << "Assimp failed to load: " << mesh.plyPath << std::endl;
            continue;
        }
        for (size_t i = before; i < renderables.size(); ++i) {
            Entity e = renderables[i];
            if (!tcm.hasComponent(e)) {
                tcm.create(e);
            }
            auto ti = tcm.getInstance(e);
            tcm.setTransform(ti, mesh.transform);
            if (rcm.hasComponent(e)) {
                auto ri = rcm.getInstance(e);
                rcm.setCastShadows(ri, true);
            }
            scene.addEntity(e);
        }
        ++mResult.meshesLoaded;
    }

    if (mOptions.spawnAnalyticLights) {
        for (const auto& light : mScene.scene.lights) {
            mAnalyticLights.push_back(em.create());
            if (light.type == PbrtLightType::Directional) {
                LightManager::Builder(LightManager::Type::SUN)
                        .color(light.color)
                        .intensity(light.intensity)
                        .direction(light.direction)
                        .castShadows(light.castShadows)
                        .build(engine, mAnalyticLights.back());
            } else {
                LightManager::Builder(LightManager::Type::POINT)
                        .color(light.color)
                        .intensity(light.intensity)
                        .position(light.position)
                        .castShadows(light.castShadows)
                        .build(engine, mAnalyticLights.back());
            }
            scene.addEntity(mAnalyticLights.back());
        }
        mResult.analyticLightCount = mAnalyticLights.size();
    }

    for (const auto& rect : mScene.scene.areaLights) {
        mRectLights.push_back(createRectAreaLight(engine, scene, rect));
    }
    mResult.rectAreaLightCount = mScene.scene.areaLights.size();
    mBuilt = true;
    return true;
}

void PbrtFilamentSceneHost::destroy(Engine& engine, Scene& scene) {
    if (!mBuilt) {
        return;
    }

    for (auto& asset : mRectLights) {
        scene.remove(asset.lightEntity);
        engine.destroy(asset.lightEntity);
        EntityManager::get().destroy(asset.lightEntity);
        scene.remove(asset.meshEntity);
        engine.destroy(asset.meshEntity);
        engine.destroy(asset.mi);
        engine.destroy(asset.material);
        engine.destroy(asset.vb);
        engine.destroy(asset.ib);
        EntityManager::get().destroy(asset.meshEntity);
    }
    mRectLights.clear();

    mMeshLoader.reset();
    mMaterialInstances.clear();

    for (Material* material : mOwnedMaterials) {
        engine.destroy(material);
    }
    mOwnedMaterials.clear();

    destroyPbrtFilamentTextures(engine, mScene.textures);

    for (Entity e : mAnalyticLights) {
        scene.remove(e);
        engine.destroy(e);
        EntityManager::get().destroy(e);
    }
    mAnalyticLights.clear();

    mScene = {};
    mResult = {};
    mBuilt = false;
}

} // namespace filament::pbrtio
