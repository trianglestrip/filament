/*
 * PBRT kitchen scene viewer sample.
 * Parses pbrt-v4 scenes using pbrtio (ported from Falcor PBRTImporter) and renders with Filament.
 */

#include "common/arguments.h"

#include <pbrtio/FilamentPbrtLoader.h>

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/View.h>

#include <stb_image.h>

#include <filamentapp/Config.h>
#include <filamentapp/FilamentApp.h>
#include <filamentapp/MeshAssimp.h>

#include <utils/EntityManager.h>
#include <utils/Path.h>
#include <utils/getopt.h>

#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "generated/resources/resources.h"

using namespace filament;
using namespace filament::math;
using namespace utils;

struct App {
    Config config;
    std::filesystem::path scenePath;
    std::unique_ptr<MeshAssimp> meshLoader;
    std::map<std::string, MaterialInstance*> materials;
    std::map<std::string, Texture*> textures;
    std::vector<Texture*> ownedTextures;
    std::vector<Entity> lights;
};

static const char* IBL_FOLDER = "assets/ibl/lightroom_14b";
static const char* DEFAULT_SCENE =
        "D:/gitProject/VLR_WF/models/kitchen/scene-v4.pbrt";

static void printUsage(char* name) {
    std::string exec_name(Path(name).getName());
    std::string usage(
            "EXEC loads a PBRT scene (default: kitchen) and renders with Filament\n"
            "Usage:\n"
            "    EXEC [options] [scene.pbrt]\n"
            "Options:\n"
            "   --help, -h\n"
            "       Prints this message\n\n"
            "API_USAGE"
            "   --scene=<path>, -s <path>\n"
            "       PBRT scene file (default: kitchen scene-v4.pbrt)\n\n"
    );
    const std::string from("EXEC");
    for (size_t pos = usage.find(from); pos != std::string::npos; pos = usage.find(from, pos)) {
        usage.replace(pos, from.length(), exec_name);
    }
    const std::string apiUsage("API_USAGE");
    for (size_t pos = usage.find(apiUsage); pos != std::string::npos; pos = usage.find(apiUsage, pos)) {
        usage.replace(pos, apiUsage.length(), samples::getBackendAPIArgumentsUsage());
    }
    std::cout << usage;
}

static int handleCommandLineArguments(int argc, char* argv[], App* app) {
    static constexpr const char* OPTSTR = "ha:s:";
    static const utils::getopt::option OPTIONS[] = {
            { "help",  utils::getopt::no_argument,       nullptr, 'h' },
            { "api",   utils::getopt::required_argument, nullptr, 'a' },
            { "scene", utils::getopt::required_argument, nullptr, 's' },
            { nullptr, 0, nullptr, 0 }
    };
    int opt;
    int option_index = 0;
    while ((opt = utils::getopt::getopt_long(argc, argv, OPTSTR, OPTIONS, &option_index)) >= 0) {
        std::string arg(utils::getopt::optarg ? utils::getopt::optarg : "");
        switch (opt) {
            default:
            case 'h':
                printUsage(argv[0]);
                exit(0);
            case 'a':
                app->config.backend = samples::parseArgumentsForBackend(arg);
                break;
            case 's':
                app->scenePath = arg;
                break;
        }
    }
    return utils::getopt::optind;
}

static Texture* loadBaseColorTexture(Engine& engine, App& app,
        const std::filesystem::path& path, bool sRGB) {
    const std::string key = path.string();
    auto cached = app.textures.find(key);
    if (cached != app.textures.end()) {
        return cached->second;
    }

    if (!std::filesystem::exists(path)) {
        std::cerr << "Missing texture: " << path << std::endl;
        return nullptr;
    }

    int w = 0, h = 0, n = 0;
    constexpr int kChannels = 4;
    const utils::Path absPath = utils::Path(path.string()).getAbsolutePath();
    uint8_t* data = stbi_load(absPath.c_str(), &w, &h, &n, kChannels);
    if (!data) {
        std::cerr << "Failed to load texture: " << path << std::endl;
        return nullptr;
    }

    const Texture::InternalFormat format = sRGB
            ? Texture::InternalFormat::SRGB8_A8
            : Texture::InternalFormat::RGBA8;
    Texture* tex = Texture::Builder()
            .width(static_cast<uint32_t>(w))
            .height(static_cast<uint32_t>(h))
            .levels(0xff)
            .format(format)
            .usage(Texture::Usage::DEFAULT | Texture::Usage::GEN_MIPMAPPABLE)
            .build(engine);

    Texture::PixelBufferDescriptor buffer(data,
            static_cast<size_t>(w) * h * kChannels,
            Texture::Format::RGBA,
            Texture::Type::UBYTE,
            (Texture::PixelBufferDescriptor::Callback) &stbi_image_free);
    tex->setImage(engine, 0, std::move(buffer));
    tex->generateMipmaps(engine);

    app.textures[key] = tex;
    app.ownedTextures.push_back(tex);
    return tex;
}

static MaterialInstance* getOrCreateMaterial(Engine& engine, App& app,
        const filament::pbrtio::PbrtMeshInstance& mesh) {
    const bool textured = !mesh.baseColorTexturePath.empty();
    const std::string key = textured
            ? (mesh.materialName + "|" + mesh.baseColorTexturePath.string())
            : (mesh.materialName.empty()
                    ? (std::to_string(mesh.baseColor.x) + "," +
                       std::to_string(mesh.baseColor.y) + "," +
                       std::to_string(mesh.baseColor.z))
                    : mesh.materialName);

    auto it = app.materials.find(key);
    if (it != app.materials.end()) {
        return it->second;
    }

    MaterialInstance* mi = nullptr;
    if (textured) {
        if (Texture* map = loadBaseColorTexture(engine, app, mesh.baseColorTexturePath,
                mesh.baseColorTextureSRGB)) {
            Material* material = Material::Builder()
                    .package(RESOURCES_PBRTTEXTURED_DATA, RESOURCES_PBRTTEXTURED_SIZE)
                    .build(engine);
            mi = material->createInstance();
            TextureSampler sampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
                    TextureSampler::MagFilter::LINEAR,
                    TextureSampler::WrapMode::REPEAT);
            mi->setParameter("baseColorMap", map, sampler);
            mi->setParameter("baseColor", RgbType::LINEAR, float3(1.f));
            mi->setParameter("roughness", mesh.roughness);
            mi->setParameter("metallic", mesh.metallic);
        }
    }

    if (!mi) {
        Material* material = Material::Builder()
                .package(RESOURCES_AIDEFAULTMAT_DATA, RESOURCES_AIDEFAULTMAT_SIZE)
                .build(engine);
        mi = material->createInstance();
        mi->setParameter("baseColor", RgbType::LINEAR, mesh.baseColor);
        mi->setParameter("roughness", mesh.roughness);
        mi->setParameter("metallic", mesh.metallic);
    }

    app.materials[key] = mi;
    return mi;
}

int main(int argc, char** argv) {
    App app;
    app.config.title = "PBRT Kitchen";
    app.config.iblDirectory = FilamentApp::getRootAssetsPath() + IBL_FOLDER;
    app.scenePath = DEFAULT_SCENE;

    handleCommandLineArguments(argc, argv, &app);
    if (utils::getopt::optind < argc) {
        app.scenePath = argv[utils::getopt::optind];
    }

    filament::pbrtio::PbrtLoadedScene pbrtScene;
    if (!filament::pbrtio::loadPbrtScene(app.scenePath, pbrtScene)) {
        std::cerr << "Failed to load PBRT scene: " << app.scenePath << std::endl;
        return 1;
    }
    std::cout << "Loaded " << pbrtScene.meshes.size() << " meshes from "
              << app.scenePath << std::endl;

    auto setup = [&app, pbrtScene](Engine* engine, View* view, Scene* scene) {
        app.meshLoader = std::make_unique<MeshAssimp>(*engine);
        auto& tcm = engine->getTransformManager();
        auto& rcm = engine->getRenderableManager();
        auto& em = EntityManager::get();

        size_t loaded = 0;
        for (const auto& mesh : pbrtScene.meshes) {
            if (!std::filesystem::exists(mesh.plyPath)) {
                std::cerr << "Missing mesh: " << mesh.plyPath << std::endl;
                continue;
            }
            MaterialInstance* mi = getOrCreateMaterial(*engine, app, mesh);
            std::map<std::string, MaterialInstance*> matMap;
            matMap["DefaultMaterial"] = mi;
            const size_t before = app.meshLoader->getRenderables().size();
            app.meshLoader->addFromFile(utils::Path(mesh.plyPath.string()), matMap, true);
            const auto& renderables = app.meshLoader->getRenderables();
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
                scene->addEntity(e);
            }
            ++loaded;
        }
        std::cout << "Rendered " << loaded << " / " << pbrtScene.meshes.size()
                  << " meshes, " << app.textures.size() << " textures." << std::endl;

        if (loaded == 0) {
            std::cerr << "No geometry loaded. Rebuild assimp (PLY support) and pbrt_kitchen."
                      << std::endl;
            return;
        }

        const float3 bmin = app.meshLoader->minBound;
        const float3 bmax = app.meshLoader->maxBound;
        const float3 center = (bmin + bmax) * 0.5f;
        const float radius = std::max(length(bmax - bmin) * 0.5f, 0.1f);

        app.lights.push_back(em.create());
        LightManager::Builder(LightManager::Type::SUN)
                .color(Color::toLinear<ACCURATE>(sRGBColor(0.98f, 0.95f, 0.88f)))
                .intensity(110000)
                .direction(normalize(float3{ 0.5f, -1.f, -0.3f }))
                .castShadows(true)
                .build(*engine, app.lights.back());
        scene->addEntity(app.lights.back());

        view->setAmbientOcclusionOptions({ .enabled = true });
        view->setBloomOptions({ .enabled = true });

        const float dist = radius * 2.5f;
        const float3 eye = center + float3(0.f, dist * 0.35f, dist);
        Camera& camera = view->getCamera();
        camera.setExposure(16.f, 1.f / 125.f, 100.f);
        camera.lookAt(eye, center);
        FilamentApp::get().setCameraNearFar(radius * 0.01f, radius * 20.f);
    };

    auto preRender = [](Engine*, View*, Scene*, Renderer* renderer) {
        if (!FilamentApp::get().getIBL()) {
            renderer->setClearOptions({
                    .clearColor = { 0.1f, 0.1f, 0.12f, 1.0f },
                    .clear = true });
        }
    };

    auto cleanup = [&app](Engine* engine, View*, Scene*) {
        for (auto& [_, mi] : app.materials) {
            engine->destroy(mi);
        }
        app.materials.clear();
        for (Texture* tex : app.ownedTextures) {
            engine->destroy(tex);
        }
        app.ownedTextures.clear();
        app.textures.clear();
        app.meshLoader.reset();
        for (Entity e : app.lights) {
            engine->destroy(e);
            EntityManager::get().destroy(e);
        }
        app.lights.clear();
    };

    FilamentApp::get().run(app.config, setup, cleanup, {}, preRender);
    return 0;
}
