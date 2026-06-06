#include "PbrtFilamentSceneHost.h"

#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>

#include <stb_image.h>

#include <utils/EntityManager.h>

#include <cmath>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <math/mat3.h>
#include <math/norm.h>
#include <math/quat.h>

namespace filament::pbrtio {
namespace {

using namespace filament::math;
using utils::Entity;
using utils::EntityManager;

static float2 toFilament(const ::pbrtio::pbrt::float2& v) {
    return float2(v.x, v.y);
}

static float3 toFilament(const ::pbrtio::pbrt::float3& v) {
    return float3(v.x, v.y, v.z);
}

static float4 toFilament(const ::pbrtio::pbrt::float4& v) {
    return float4(v.x, v.y, v.z, v.w);
}

static mat4f toFilament(const ::pbrtio::pbrt::mat4f& m) {
    return mat4f(
            toFilament(m[0]),
            toFilament(m[1]),
            toFilament(m[2]),
            toFilament(m[3]));
}

mat4f buildRectLightTransform(const PbrtRectAreaLight& rect) {
    const float3 p0 = toFilament(rect.positions[0]);
    const float3 p1 = toFilament(rect.positions[1]);
    const float3 p2 = toFilament(rect.positions[2]);
    const float3 p3 = toFilament(rect.positions[3]);
    const float3 center = (p0 + p1 + p2 + p3) * 0.25f;
    const float3 edge1 = (p1 - p0) * 0.5f;
    const float3 edge2 = (p3 - p0) * 0.5f;
    const float3 normal = normalize(cross(p1 - p0, p3 - p0));
    return mat4f(
            float4(edge1, 0.f),
            float4(edge2, 0.f),
            float4(normal, 0.f),
            float4(center, 1.f));
}

template<typename T>
struct BufferState {
    explicit BufferState(std::vector<T>&& in) : data(std::move(in)) {}

    static void free(void*, size_t, void* user) {
        delete static_cast<BufferState<T>*>(user);
    }

    size_t size() const { return data.size() * sizeof(T); }

    std::vector<T> data;
};

struct PbrtPlyMeshData {
    std::vector<float3> positions;
    std::vector<short4> tangents;
    std::vector<float2> uvs;
    std::vector<uint32_t> indices;
    Box aabb;
};

static bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

static bool readBinary(std::istream& in, void* dst, size_t size) {
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(size));
    return bool(in);
}

static short4 packTangentFrameFromNormal(float3 normal) {
    if (!std::isfinite(length(normal)) || length(normal) < 1e-6f) {
        normal = float3{ 0.f, 1.f, 0.f };
    } else {
        normal = normalize(normal);
    }
    const float3 helper = std::abs(normal.y) < 0.999f ? float3{ 0.f, 1.f, 0.f } :
            float3{ 1.f, 0.f, 0.f };
    const float3 tangent = normalize(cross(helper, normal));
    const float3 bitangent = normalize(cross(normal, tangent));
    const quatf q = filament::math::details::TMat33<float>::packTangentFrame({
            tangent, bitangent, normal });
    return packSnorm16(q.xyzw);
}

static bool loadPbrtBinaryPly(const std::filesystem::path& path, PbrtPlyMeshData& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    std::string line;
    if (!std::getline(in, line) || line != "ply") {
        return false;
    }

    bool binaryLittleEndian = false;
    size_t vertexCount = 0;
    size_t faceCount = 0;
    std::vector<std::string> vertexProperties;
    enum class HeaderElement { None, Vertex, Face };
    HeaderElement element = HeaderElement::None;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "end_header") {
            break;
        }
        if (startsWith(line, "format ")) {
            binaryLittleEndian = line.find("binary_little_endian") != std::string::npos;
        } else if (startsWith(line, "element vertex ")) {
            element = HeaderElement::Vertex;
            std::istringstream iss(line.substr(15));
            iss >> vertexCount;
        } else if (startsWith(line, "element face ")) {
            element = HeaderElement::Face;
            std::istringstream iss(line.substr(13));
            iss >> faceCount;
        } else if (startsWith(line, "property ") && element == HeaderElement::Vertex) {
            std::istringstream iss(line);
            std::string property;
            std::string type;
            std::string name;
            iss >> property >> type >> name;
            if (type != "float" && type != "float32") {
                return false;
            }
            vertexProperties.push_back(name);
        }
    }

    if (!binaryLittleEndian || vertexCount == 0 || vertexProperties.empty()) {
        return false;
    }

    out.positions.reserve(vertexCount);
    out.tangents.reserve(vertexCount);
    out.uvs.reserve(vertexCount);
    float3 bmin(std::numeric_limits<float>::max());
    float3 bmax(std::numeric_limits<float>::lowest());

    std::vector<float> values(vertexProperties.size());
    for (size_t i = 0; i < vertexCount; ++i) {
        for (float& value : values) {
            if (!readBinary(in, &value, sizeof(float))) {
                return false;
            }
        }

        float3 position(0.f);
        float3 normal(0.f, 1.f, 0.f);
        float2 uv(0.f);
        for (size_t p = 0; p < vertexProperties.size(); ++p) {
            const std::string& name = vertexProperties[p];
            const float value = values[p];
            if (name == "x") position.x = value;
            else if (name == "y") position.y = value;
            else if (name == "z") position.z = value;
            else if (name == "nx") normal.x = value;
            else if (name == "ny") normal.y = value;
            else if (name == "nz") normal.z = value;
            else if (name == "u" || name == "s" || name == "texture_u" ||
                    name == "texture_s") uv.x = value;
            else if (name == "v" || name == "t" || name == "texture_v" ||
                    name == "texture_t") uv.y = value;
        }

        out.positions.push_back(position);
        out.tangents.push_back(packTangentFrameFromNormal(normal));
        out.uvs.push_back(uv);
        bmin = min(bmin, position);
        bmax = max(bmax, position);
    }

    out.indices.reserve(faceCount * 3);
    for (size_t i = 0; i < faceCount; ++i) {
        uint8_t faceVertexCount = 0;
        if (!readBinary(in, &faceVertexCount, sizeof(uint8_t))) {
            return false;
        }
        std::vector<uint32_t> face(faceVertexCount);
        for (uint32_t& index : face) {
            int32_t signedIndex = 0;
            if (!readBinary(in, &signedIndex, sizeof(int32_t)) || signedIndex < 0 ||
                    static_cast<size_t>(signedIndex) >= vertexCount) {
                return false;
            }
            index = static_cast<uint32_t>(signedIndex);
        }
        for (uint8_t v = 1; v + 1 < faceVertexCount; ++v) {
            out.indices.push_back(face[0]);
            out.indices.push_back(face[v]);
            out.indices.push_back(face[v + 1]);
        }
    }

    out.aabb = Box().set(bmin, bmax);
    return !out.positions.empty() && !out.indices.empty();
}

static Texture* createWhiteTexture(Engine& engine) {
    constexpr uint32_t kWhitePixel = 0xFFFFFFFFu;
    Texture* texture = Texture::Builder()
            .width(1)
            .height(1)
            .levels(1)
            .format(Texture::InternalFormat::RGBA8)
            .build(engine);
    Texture::PixelBufferDescriptor buffer(&kWhitePixel, 4, Texture::Format::RGBA,
            Texture::Type::UBYTE);
    texture->setImage(engine, 0, std::move(buffer));
    return texture;
}

static std::string buildMaterialCacheKey(const PbrtMeshInstance& mesh) {
    std::ostringstream key;
    key << mesh.pbrtMaterialType << '|' << mesh.materialName << '|'
        << mesh.baseColor.x << ',' << mesh.baseColor.y << ',' << mesh.baseColor.z << '|'
        << mesh.roughness << '|' << mesh.metallic << '|'
        << mesh.clearCoat << '|' << mesh.clearCoatRoughness << '|'
        << mesh.roughnessScale << '|'
        << mesh.baseColorTexturePath.string() << '|'
        << mesh.roughnessTexturePath.string() << '|'
        << mesh.baseColorUvTransform.x << ',' << mesh.baseColorUvTransform.y << ','
        << mesh.baseColorUvTransform.z << ',' << mesh.baseColorUvTransform.w << '|'
        << mesh.roughnessUvTransform.x << ',' << mesh.roughnessUvTransform.y << ','
        << mesh.roughnessUvTransform.z << ',' << mesh.roughnessUvTransform.w << '|'
        << mesh.roughnessMapsClearCoat;
    return key.str();
}

static Texture* uploadDecodedTexture(Engine& engine, PbrtDecodedImage& image) {
    if (!image.valid || !image.pixels || image.width <= 0 || image.height <= 0) {
        return nullptr;
    }

    const Texture::InternalFormat format = image.sRGB
            ? Texture::InternalFormat::SRGB8_A8
            : Texture::InternalFormat::RGBA8;

    Texture* texture = Texture::Builder()
            .width(static_cast<uint32_t>(image.width))
            .height(static_cast<uint32_t>(image.height))
            .levels(0xff)
            .format(format)
            .usage(Texture::Usage::DEFAULT | Texture::Usage::GEN_MIPMAPPABLE)
            .build(engine);

    uint8_t* pixels = image.pixels;
    const size_t byteSize = image.byteSize;
    image.pixels = nullptr;
    image.byteSize = 0;
    image.valid = false;

    Texture::PixelBufferDescriptor buffer(pixels, byteSize, Texture::Format::RGBA,
            Texture::Type::UBYTE,
            (Texture::PixelBufferDescriptor::Callback) &stbi_image_free);
    texture->setImage(engine, 0, std::move(buffer));
    texture->generateMipmaps(engine);
    return texture;
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
    const std::string key = buildMaterialCacheKey(mesh);

    auto it = mMaterialInstances.find(key);
    if (it != mMaterialInstances.end()) {
        return it->second;
    }

    const bool hasBaseColorMap = !mesh.baseColorTexturePath.empty();
    const bool hasRoughnessMap = !mesh.roughnessTexturePath.empty();
    const bool useTexturedMaterial = hasBaseColorMap || hasRoughnessMap || mesh.clearCoat > 0.f;

    MaterialInstance* mi = nullptr;
    if (useTexturedMaterial && mOptions.materials.texturedData && mOptions.materials.texturedSize > 0) {
        if (!mTexturedMaterial) {
            mTexturedMaterial = Material::Builder()
                    .package(mOptions.materials.texturedData, mOptions.materials.texturedSize)
                    .build(engine);
            mOwnedMaterials.push_back(mTexturedMaterial);
        }
        if (!mWhiteTexture) {
            mWhiteTexture = createWhiteTexture(engine);
            mOwnedTextures.push_back(mWhiteTexture);
        }

        mi = mTexturedMaterial->createInstance();
        TextureSampler sampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
                TextureSampler::MagFilter::LINEAR,
                TextureSampler::WrapMode::REPEAT);

        Texture* baseColorMap = mWhiteTexture;
        if (hasBaseColorMap) {
            auto texIt = mGpuTextures.find(mesh.baseColorTexturePath.string());
            if (texIt != mGpuTextures.end() && texIt->second) {
                baseColorMap = texIt->second;
            }
        }

        Texture* roughnessMap = mWhiteTexture;
        if (hasRoughnessMap) {
            auto texIt = mGpuTextures.find(mesh.roughnessTexturePath.string());
            if (texIt != mGpuTextures.end() && texIt->second) {
                roughnessMap = texIt->second;
            }
        }

        mi->setParameter("baseColorMap", baseColorMap, sampler);
        mi->setParameter("roughnessMap", roughnessMap, sampler);
        mi->setParameter("baseColor", RgbType::LINEAR,
                hasBaseColorMap ? float3(1.f) : toFilament(mesh.baseColor));
        mi->setParameter("baseColorUvTransform", toFilament(mesh.baseColorUvTransform));
        mi->setParameter("roughnessUvTransform", toFilament(mesh.roughnessUvTransform));
        mi->setParameter("roughness", mesh.roughness);
        mi->setParameter("roughnessScale", mesh.roughnessScale);
        mi->setParameter("metallic", mesh.metallic);
        mi->setParameter("clearCoat", mesh.clearCoat);
        mi->setParameter("clearCoatRoughness", mesh.clearCoatRoughness);
        mi->setParameter("hasBaseColorMap", hasBaseColorMap ? 1.f : 0.f);
        mi->setParameter("hasRoughnessMap", hasRoughnessMap ? 1.f : 0.f);
        mi->setParameter("roughnessMapsClearCoat", mesh.roughnessMapsClearCoat ? 1.f : 0.f);
    }

    if (!mi && mOptions.materials.litData && mOptions.materials.litSize > 0) {
        if (!mLitMaterial) {
            mLitMaterial = Material::Builder()
                    .package(mOptions.materials.litData, mOptions.materials.litSize)
                    .build(engine);
            mOwnedMaterials.push_back(mLitMaterial);
        }
        mi = mLitMaterial->createInstance();
        mi->setParameter("baseColor", RgbType::LINEAR, toFilament(mesh.baseColor));
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
        vertices[i] = toFilament(rect.positions[i]);
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
        if (!mUnlitMaterial) {
            mUnlitMaterial = Material::Builder()
                    .package(mOptions.materials.unlitData, mOptions.materials.unlitSize)
                    .build(engine);
            mOwnedMaterials.push_back(mUnlitMaterial);
        }
        mi = mUnlitMaterial->createInstance();
        const float3 radiance = toFilament(rect.radiance) * rect.scale;
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

    const float3 radiance = toFilament(rect.radiance) * rect.scale;
    const float quadArea = std::max(rect.width * rect.height, 1e-4f);
    const float maxChannel = std::max(radiance.x, std::max(radiance.y, radiance.z));
    const float3 lightColor = maxChannel > 0.f ? radiance / maxChannel : float3(1.f);
    const float luminance = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
    const float lumens = std::max(luminance * quadArea * float(M_PI), 1e-4f);

    Entity lightEntity = EntityManager::get().create();
    auto& tcm = engine.getTransformManager();
    tcm.create(lightEntity);
    tcm.setTransform(tcm.getInstance(lightEntity), buildRectLightTransform(rect));
    LightManager::Builder(LightManager::Type::RECT)
            .color(lightColor)
            .intensity(lumens)
            .falloff(100.f)
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
    mGpuTextures.clear();
    mOwnedTextures.clear();
    mWhiteTexture = nullptr;

    if (!loadPbrtSceneResources(pbrtPath, mScene, mOptions.loadEnvironmentTexture)) {
        return false;
    }

    const auto uploadStart = std::chrono::steady_clock::now();
    for (auto& [key, image] : mScene.textures.decoded) {
        if (Texture* texture = uploadDecodedTexture(engine, image)) {
            mGpuTextures[key] = texture;
            mOwnedTextures.push_back(texture);
        }
    }
    mScene.timings.uploadTexturesMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - uploadStart).count();
    mScene.timings.totalMs += mScene.timings.uploadTexturesMs;

    auto& tcm = engine.getTransformManager();
    auto& rcm = engine.getRenderableManager();
    auto& em = EntityManager::get();

    const auto meshImportStart = std::chrono::steady_clock::now();
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

        PbrtPlyMeshData ply;
        if (!loadPbrtBinaryPly(mesh.plyPath, ply)) {
            std::cerr << "PBRT PLY load failed: " << mesh.plyPath << std::endl;
            continue;
        }

        VertexBuffer* vb = VertexBuffer::Builder()
                .vertexCount(static_cast<uint32_t>(ply.positions.size()))
                .bufferCount(3)
                .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3)
                .attribute(VertexAttribute::TANGENTS, 1, VertexBuffer::AttributeType::SHORT4)
                .normalized(VertexAttribute::TANGENTS)
                .attribute(VertexAttribute::UV0, 2, VertexBuffer::AttributeType::FLOAT2)
                .build(engine);

        auto* positions = new BufferState<float3>(std::move(ply.positions));
        vb->setBufferAt(engine, 0, VertexBuffer::BufferDescriptor(
                positions->data.data(), positions->size(), BufferState<float3>::free, positions));

        auto* tangents = new BufferState<short4>(std::move(ply.tangents));
        vb->setBufferAt(engine, 1, VertexBuffer::BufferDescriptor(
                tangents->data.data(), tangents->size(), BufferState<short4>::free, tangents));

        auto* uvs = new BufferState<float2>(std::move(ply.uvs));
        vb->setBufferAt(engine, 2, VertexBuffer::BufferDescriptor(
                uvs->data.data(), uvs->size(), BufferState<float2>::free, uvs));

        IndexBuffer* ib = IndexBuffer::Builder()
                .indexCount(static_cast<uint32_t>(ply.indices.size()))
                .bufferType(IndexBuffer::IndexType::UINT)
                .build(engine);

        auto* indices = new BufferState<uint32_t>(std::move(ply.indices));
        ib->setBuffer(engine, IndexBuffer::BufferDescriptor(
                indices->data.data(), indices->size(), BufferState<uint32_t>::free, indices));

        Entity e = em.create();
        RenderableManager::Builder(1)
                .boundingBox(ply.aabb)
                .material(0, mi)
                .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, vb, ib)
                .culling(false)
                .castShadows(true)
                .receiveShadows(true)
                .build(engine, e);
        tcm.create(e);
        tcm.setTransform(tcm.getInstance(e), toFilament(mesh.transform));
        if (rcm.hasComponent(e)) {
            auto ri = rcm.getInstance(e);
            rcm.setCastShadows(ri, true);
        }
        scene.addEntity(e);
        mMeshAssets.push_back({ e, vb, ib });
        ++mResult.meshesLoaded;
    }
    mResult.meshImportMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - meshImportStart).count();

    if (mOptions.spawnAnalyticLights) {
        for (const auto& light : mScene.scene.lights) {
            mAnalyticLights.push_back(em.create());
            if (light.type == PbrtLightType::Directional) {
                LightManager::Builder(LightManager::Type::SUN)
                        .color(toFilament(light.color))
                        .intensity(light.intensity)
                        .direction(toFilament(light.direction))
                        .castShadows(light.castShadows)
                        .build(engine, mAnalyticLights.back());
            } else {
                LightManager::Builder(LightManager::Type::POINT)
                        .color(toFilament(light.color))
                        .intensity(light.intensity)
                        .position(toFilament(light.position))
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
        if (asset.material) {
            engine.destroy(asset.material);
        }
        engine.destroy(asset.vb);
        engine.destroy(asset.ib);
        EntityManager::get().destroy(asset.meshEntity);
    }
    mRectLights.clear();

    for (auto& asset : mMeshAssets) {
        scene.remove(asset.entity);
        engine.destroy(asset.entity);
        engine.destroy(asset.vb);
        engine.destroy(asset.ib);
        EntityManager::get().destroy(asset.entity);
    }
    mMeshAssets.clear();
    mMaterialInstances.clear();

    for (Material* material : mOwnedMaterials) {
        engine.destroy(material);
    }
    mOwnedMaterials.clear();
    mLitMaterial = nullptr;
    mTexturedMaterial = nullptr;
    mUnlitMaterial = nullptr;

    for (Texture* texture : mOwnedTextures) {
        engine.destroy(texture);
    }
    mOwnedTextures.clear();
    mGpuTextures.clear();
    mWhiteTexture = nullptr;
    mScene.textures.decoded.clear();

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
