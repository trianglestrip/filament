/*
 * Filament PBRT scene loader (uses pbrtio parser from Falcor PBRTImporter).
 */

#include <pbrtio/FilamentPbrtLoader.h>

#include <pbrtio/Builder.h>
#include <pbrtio/Parser.h>
#include <pbrtio/PbrtSpectrum.h>

#include <algorithm>
#include <cmath>

namespace filament::pbrtio {

using namespace filament::pbrt;
using namespace filament::math;

static const mat4f kPbrtToFilament = mat4f(
        1.f, 0.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 0.f, 1.f);

static const mat4f kInvertZ = mat4f(
        1.f, 0.f,  0.f, 0.f,
        0.f, 1.f,  0.f, 0.f,
        0.f, 0.f, -1.f, 0.f,
        0.f, 0.f,  0.f, 1.f);

static float3 spectrumToColor(const Spectrum& spectrum) {
    return spectrumToRGB(spectrum);
}

static std::filesystem::path resolveSpectrumImagePath(const BasicScene& scene,
        const std::string& textureName, bool& outSRGB) {
    const auto& textures = scene.getSpectrumTextures();
    const auto it = textures.find(textureName);
    if (it == textures.end() || it->second.name != "imagemap") {
        return {};
    }
    const auto& entity = it->second;
    const std::string filename = entity.params.getString("filename", "");
    if (filename.empty()) {
        return {};
    }
    const std::string encoding = entity.params.getString("encoding", "");
    if (encoding == "linear") {
        outSRGB = false;
    } else if (encoding == "sRGB") {
        outSRGB = true;
    } else {
        outSRGB = true;
    }
    return scene.resolvePath(filename);
}

static void resolveMaterial(const BasicScene& scene, const MaterialRef& ref,
        PbrtMeshInstance& inst) {
    if (std::holds_alternative<std::monostate>(ref)) {
        return;
    }
    const MaterialSceneEntity& mat = scene.getMaterial(ref);
    const auto& params = mat.params;
    if (mat.type == "diffuse" || mat.type == "coateddiffuse") {
        if (params.hasTexture("reflectance")) {
            inst.baseColorTexturePath = resolveSpectrumImagePath(scene,
                    params.getTexture("reflectance"), inst.baseColorTextureSRGB);
        } else if (params.hasSpectrum("reflectance")) {
            inst.baseColor = spectrumToColor(params.getSpectrum("reflectance", Spectrum(float3(0.8f)),
                    [&](const std::filesystem::path& p) { return scene.resolvePath(p); }));
        }
        if (mat.type == "coateddiffuse") {
            const float ur = params.getFloat("uroughness", inst.roughness);
            const float vr = params.getFloat("vroughness", inst.roughness);
            inst.roughness = (ur + vr) * 0.5f;
        } else if (params.hasFloat("roughness")) {
            inst.roughness = params.getFloat("roughness", inst.roughness);
        }
    } else if (mat.type == "conductor" || mat.type == "coatedconductor") {
        inst.metallic = 1.f;
        const float ur = params.getFloat("uroughness", 0.15f);
        const float vr = params.getFloat("vroughness", inst.roughness);
        inst.roughness = (ur + vr) * 0.5f;
        inst.baseColor = float3(0.9f, 0.9f, 0.92f);
    } else if (mat.type == "dielectric" || mat.type == "thindielectric") {
        inst.roughness = params.getFloat("roughness", 0.05f);
        inst.baseColor = float3(0.95f);
    }
}

static void addShape(const BasicScene& scene, const ShapeSceneEntity& shape,
        std::vector<PbrtMeshInstance>& meshes) {
    if (shape.name != "plymesh") {
        return;
    }
    const auto filename = shape.params.getString("filename", "");
    if (filename.empty()) {
        return;
    }

    PbrtMeshInstance inst;
    inst.plyPath = scene.resolvePath(filename);
    inst.transform = kPbrtToFilament * shape.transform * kInvertZ;

    if (!std::holds_alternative<std::monostate>(shape.materialRef)) {
        if (const auto* name = std::get_if<std::string>(&shape.materialRef)) {
            inst.materialName = *name;
        }
        resolveMaterial(scene, shape.materialRef, inst);
    }

    meshes.push_back(std::move(inst));
}

bool loadPbrtScene(const std::filesystem::path& pbrtPath, PbrtLoadedScene& out) {
    if (!std::filesystem::exists(pbrtPath)) {
        return false;
    }

    BasicScene scene(pbrtPath.parent_path());
    BasicSceneBuilder builder(scene);
    parseFile(builder, pbrtPath);

    out.searchPath = scene.resolvePath(".");
    out.meshes.clear();

    for (const auto& shape : scene.getShapes()) {
        addShape(scene, shape, out.meshes);
    }

    for (const auto& [_, def] : scene.getInstanceDefinitions()) {
        for (const auto& shape : def.shapes) {
            addShape(scene, shape, out.meshes);
        }
    }

    for (const auto& instance : scene.getInstances()) {
        auto it = scene.getInstanceDefinitions().find(instance.name);
        if (it == scene.getInstanceDefinitions().end()) {
            continue;
        }
        for (const auto& shape : it->second.shapes) {
            ShapeSceneEntity instShape = shape;
            instShape.transform = instance.transform * shape.transform;
            addShape(scene, instShape, out.meshes);
        }
    }

    const auto& cam = scene.getCamera();
    out.cameraTransform = kPbrtToFilament * cam.transform * kInvertZ;
    out.cameraFov = cam.params.getFloat("fov", 45.f);

    float3 bmin(1e30f), bmax(-1e30f);
    for (const auto& mesh : out.meshes) {
        const float4 t = mesh.transform[3];
        const float3 p(t.x, t.y, t.z);
        bmin = min(bmin, p);
        bmax = max(bmax, p);
    }
    if (out.meshes.empty()) {
        out.sceneCenter = float3(0.f);
        out.sceneRadius = 1.f;
    } else {
        out.sceneCenter = (bmin + bmax) * 0.5f;
        out.sceneRadius = std::max(length(bmax - bmin) * 0.5f, 0.5f);
    }

    return !out.meshes.empty();
}

} // namespace filament::pbrtio
