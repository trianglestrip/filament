/*
 * PBRT kitchen scene viewer sample.
 * Strict-ish PBRT preview: rect area lights only, no IBL, minimal post-processing.
 */

#include "common/arguments.h"

#include <pbrtio/PbrtFilamentSceneHost.h>

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/View.h>

#include <filamentapp/Config.h>
#include <filamentapp/FilamentApp.h>

#include <utils/Path.h>
#include <utils/getopt.h>

#include <iostream>
#include <memory>
#include <string>

#include "generated/resources/resources.h"

using namespace filament;
using namespace utils;

struct App {
    Config config;
    std::filesystem::path scenePath;
    bool strictPbrt = true;
    std::unique_ptr<filament::pbrtio::PbrtFilamentSceneHost> sceneHost;
};

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
            "   --ibl\n"
            "       Enable default IBL (off by default for strict PBRT preview)\n\n"
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
    static constexpr const char* OPTSTR = "hai:s:";
    static const utils::getopt::option OPTIONS[] = {
            { "help",  utils::getopt::no_argument,       nullptr, 'h' },
            { "api",   utils::getopt::required_argument, nullptr, 'a' },
            { "scene", utils::getopt::required_argument, nullptr, 's' },
            { "ibl",   utils::getopt::no_argument,       nullptr, 'i' },
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
            case 'i':
                app->strictPbrt = false;
                break;
        }
    }
    return utils::getopt::optind;
}

static filament::pbrtio::PbrtFilamentBuildOptions makeBuildOptions(bool strictPbrt) {
    filament::pbrtio::PbrtFilamentBuildOptions options;
    options.materials = {
            .litData = RESOURCES_AIDEFAULTMAT_DATA,
            .litSize = RESOURCES_AIDEFAULTMAT_SIZE,
            .texturedData = RESOURCES_PBRTTEXTURED_DATA,
            .texturedSize = RESOURCES_PBRTTEXTURED_SIZE,
            .unlitData = RESOURCES_SANDBOXUNLIT_DATA,
            .unlitSize = RESOURCES_SANDBOXUNLIT_SIZE,
    };
    options.spawnAnalyticLights = !strictPbrt;
    return options;
}

static void applyStrictPbrtView(Engine& engine, View& view, Scene& scene, bool strictPbrt) {
    if (strictPbrt) {
        scene.setIndirectLight(nullptr);
        scene.setSkybox(nullptr);
        view.setAmbientOcclusionOptions({ .enabled = false });
        view.setBloomOptions({ .enabled = false });
        view.setScreenSpaceReflectionsOptions({ .enabled = false });
        view.setFogOptions({ .enabled = false });
        view.setShadowingEnabled(true);
    }
}

int main(int argc, char** argv) {
    App app;
    app.config.title = "PBRT Kitchen";
    app.config.iblDirectory = "";
    app.scenePath = DEFAULT_SCENE;

    handleCommandLineArguments(argc, argv, &app);
    if (utils::getopt::optind < argc) {
        app.scenePath = argv[utils::getopt::optind];
    }

    if (!app.strictPbrt) {
        app.config.iblDirectory = FilamentApp::getRootAssetsPath() + "assets/ibl/lightroom_14b";
    }

    auto setup = [&app](Engine* engine, View* view, Scene* scene) {
        app.sceneHost = std::make_unique<filament::pbrtio::PbrtFilamentSceneHost>();
        if (!app.sceneHost->build(*engine, *scene, app.scenePath, makeBuildOptions(app.strictPbrt))) {
            std::cerr << "Failed to load PBRT scene: " << app.scenePath << std::endl;
            app.sceneHost.reset();
            return;
        }

        const auto& pbrtScene = app.sceneHost->scene();
        const auto& result = app.sceneHost->result();
        filament::pbrtio::printPbrtLoadTimings(pbrtScene.timings);
        std::cout << "Loaded " << pbrtScene.scene.meshes.size() << " meshes, "
                  << pbrtScene.scene.areaLights.size() << " rect area lights, "
                  << pbrtScene.textures.gpu.size() << " textures from "
                  << app.scenePath << std::endl;

        applyStrictPbrtView(*engine, *view, *scene, app.strictPbrt);

        if (!app.strictPbrt && pbrtScene.scene.environment.valid) {
            FilamentApp::get().loadIBL(pbrtScene.scene.environment.mapPath.string());
            std::cout << "Environment map: " << pbrtScene.scene.environment.mapPath << std::endl;
        } else if (app.strictPbrt) {
            std::cout << "Strict PBRT mode: IBL disabled, rect area lights + black background."
                      << std::endl;
        }

        std::cout << "Rendered " << result.meshesLoaded << " / "
                  << pbrtScene.scene.meshes.size() << " meshes." << std::endl;

        if (result.meshesLoaded == 0 && pbrtScene.scene.areaLights.empty()) {
            std::cerr << "No geometry loaded. Rebuild assimp (PLY support) and pbrt_kitchen."
                      << std::endl;
            return;
        }

        std::cout << "Scene analytic lights: " << result.analyticLightCount
                  << ", rect area lights: " << result.rectAreaLightCount
                  << std::endl;

        const filament::pbrtio::PbrtCameraSettings& cam = pbrtScene.scene.camera;
        Camera& camera = view->getCamera();
        camera.setExposure(16.f, 1.f / 125.f, 100.f);
        camera.setProjection(cam.verticalFovDegrees, cam.aspectRatio, cam.nearPlane, cam.farPlane,
                Camera::Fov::VERTICAL);
        camera.lookAt(cam.eye, cam.target, cam.up);
        FilamentApp::get().setCameraNearFar(cam.nearPlane, cam.farPlane);
    };

    auto preRender = [&app](Engine*, View*, Scene*, Renderer* renderer) {
        renderer->setClearOptions({
                .clearColor = { 0.f, 0.f, 0.f, 1.f },
                .clear = true });
        if (!app.strictPbrt && !FilamentApp::get().getIBL()) {
            renderer->setClearOptions({
                    .clearColor = { 0.1f, 0.1f, 0.12f, 1.f },
                    .clear = true });
        }
    };

    auto cleanup = [&app](Engine* engine, View*, Scene* scene) {
        if (app.sceneHost) {
            app.sceneHost->destroy(*engine, *scene);
            app.sceneHost.reset();
        }
    };

    FilamentApp::get().run(app.config, setup, cleanup, {}, preRender);
    return 0;
}
