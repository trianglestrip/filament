/*
 * PBRT kitchen scene viewer sample.
 * Strict-ish PBRT preview: rect area lights only, no IBL, minimal post-processing.
 */

#include "common/arguments.h"

#include "PbrtFilamentSceneHost.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Exposure.h>
#include <filament/IndirectLight.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/View.h>

#include <camutils/Manipulator.h>

#include <filamentapp/Config.h>
#include <filamentapp/FilamentApp.h>

#include <imgui.h>
#include <math/mat3.h>
#include <utils/Path.h>
#include <utils/getopt.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>

#include "generated/resources/resources.h"

using namespace filament;
using namespace utils;

struct App {
    Config config;
    std::filesystem::path scenePath;
    std::filesystem::path environmentPath;
    bool strictPbrt = true;
    bool enableIBL = false;
    bool enableAO = false;
    bool enableBloom = false;
    bool enableSSR = false;
    bool enableFog = false;
    bool autoExposure = false;
    float aperture = 4.0f;
    float shutterSpeed = 30.0f;
    float sensitivity = 400.0f;
    float autoExposureCompensation = 0.0f;
    float autoExposureMinEV = 0.0f;
    float autoExposureMaxEV = 18.0f;
    float autoExposureMiddleGrey = 0.18f;
    float iblLuminanceScale = 0.05f;
    float rectLightLuminanceScale = 1.0f;
    float rectLightAverageLuminance = 1.0f;
    float lastAutoEV = 8.0f;
    float iblIntensity = 300.0f;
    float iblRotation = 0.0f;
    View::AmbientOcclusionOptions aoOptions{};
    View::BloomOptions bloomOptions{};
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    bool hasPbrtCamera = false;
    ::pbrtio::PbrtCameraSettings pbrtCamera;
    std::unique_ptr<filament::pbrtio::PbrtFilamentSceneHost> sceneHost;
};

static const char* DEFAULT_SCENE =
        "D:/gitProject/VLR_WF/models/kitchen/scene-v4.pbrt";

static filament::math::float3 toFilament(const ::pbrtio::pbrt::float3& v) {
    return filament::math::float3(v.x, v.y, v.z);
}

static void setStrictPreset(App& app) {
    app.strictPbrt = true;
    app.enableIBL = false;
    app.enableAO = false;
    app.enableBloom = false;
    app.enableSSR = false;
    app.enableFog = false;
    app.autoExposure = false;
    app.aperture = 4.0f;
    app.shutterSpeed = 30.0f;
    app.sensitivity = 400.0f;
    app.autoExposureCompensation = 0.0f;
    app.autoExposureMinEV = 0.0f;
    app.autoExposureMaxEV = 18.0f;
    app.autoExposureMiddleGrey = 0.18f;
    app.iblLuminanceScale = 0.05f;
    app.rectLightLuminanceScale = 1.0f;
    app.iblIntensity = 0.0f;
    app.iblRotation = 0.0f;
    app.clearColor[0] = 0.0f;
    app.clearColor[1] = 0.0f;
    app.clearColor[2] = 0.0f;
    app.clearColor[3] = 1.0f;
    app.aoOptions = {};
    app.bloomOptions = {};
}

static void setPreviewPreset(App& app) {
    app.strictPbrt = false;
    app.enableIBL = true;
    app.enableAO = true;
    app.enableBloom = false;
    app.enableSSR = false;
    app.enableFog = false;
    app.autoExposure = false;
    app.aperture = 8.0f;
    app.shutterSpeed = 125.0f;
    app.sensitivity = 100.0f;
    app.autoExposureCompensation = 0.0f;
    app.autoExposureMinEV = 0.0f;
    app.autoExposureMaxEV = 18.0f;
    app.autoExposureMiddleGrey = 0.18f;
    app.iblLuminanceScale = 0.05f;
    app.rectLightLuminanceScale = 1.0f;
    app.iblIntensity = 300.0f;
    app.iblRotation = 0.0f;
    app.clearColor[0] = 0.03f;
    app.clearColor[1] = 0.03f;
    app.clearColor[2] = 0.035f;
    app.clearColor[3] = 1.0f;
    app.aoOptions = {};
    app.aoOptions.enabled = true;
    app.aoOptions.radius = 0.45f;
    app.aoOptions.intensity = 0.55f;
    app.aoOptions.power = 1.0f;
    app.bloomOptions = {};
    app.bloomOptions.enabled = false;
    app.bloomOptions.strength = 0.03f;
    app.bloomOptions.threshold = true;
    app.bloomOptions.highlight = 1000.0f;
}

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
            "       Enable preview lighting with default IBL\n\n"
            "   --preview, -p\n"
            "       Enable IBL, AO, bloom, and analytic PBRT lights for nicer realtime viewing\n\n"
            "   --strict\n"
            "       Disable IBL and post effects for a strict PBRT geometry/light check\n\n"
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
    static constexpr const char* OPTSTR = "hai:ps:";
    static constexpr int OPT_STRICT = 1000;
    static const utils::getopt::option OPTIONS[] = {
            { "help",  utils::getopt::no_argument,       nullptr, 'h' },
            { "api",   utils::getopt::required_argument, nullptr, 'a' },
            { "scene", utils::getopt::required_argument, nullptr, 's' },
            { "ibl",   utils::getopt::no_argument,       nullptr, 'i' },
            { "preview", utils::getopt::no_argument,     nullptr, 'p' },
            { "strict", utils::getopt::no_argument,      nullptr, OPT_STRICT },
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
            case 'p':
                app->strictPbrt = false;
                break;
            case OPT_STRICT:
                app->strictPbrt = true;
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
    options.loadEnvironmentTexture = false;
    return options;
}

static void applyPbrtView(App& app, View& view, Scene& scene) {
    if (app.enableIBL && !FilamentApp::get().getIBL() && !app.environmentPath.empty()) {
        FilamentApp::get().loadIBL(app.environmentPath.string());
    }

    if (app.enableIBL && FilamentApp::get().getIBL()) {
        auto* ibl = FilamentApp::get().getIBL();
        ibl->getIndirectLight()->setIntensity(app.iblIntensity);
        ibl->getIndirectLight()->setRotation(filament::math::mat3f::rotation(
                app.iblRotation, filament::math::float3{ 0, 1, 0 }));
        scene.setIndirectLight(ibl->getIndirectLight());
        scene.setSkybox(ibl->getSkybox());
    } else {
        scene.setIndirectLight(nullptr);
        scene.setSkybox(nullptr);
    }

    app.aoOptions.enabled = app.enableAO;
    app.bloomOptions.enabled = app.enableBloom;
    view.setAmbientOcclusionOptions(app.aoOptions);
    view.setBloomOptions(app.bloomOptions);
    view.setScreenSpaceReflectionsOptions({ .enabled = app.enableSSR });
    view.setFogOptions({ .enabled = app.enableFog });
    view.setShadowingEnabled(true);
    float estimatedLuminance = std::max(app.rectLightAverageLuminance *
            app.rectLightLuminanceScale, 0.001f);
    if (app.enableIBL) {
        estimatedLuminance += app.iblIntensity * app.iblLuminanceScale;
    }
    const float middleGrey = std::clamp(app.autoExposureMiddleGrey, 0.01f, 1.0f);
    app.lastAutoEV = std::clamp(Exposure::ev100FromLuminance(estimatedLuminance / middleGrey) +
            app.autoExposureCompensation, app.autoExposureMinEV, app.autoExposureMaxEV);
    if (app.autoExposure) {
        view.getCamera().setExposure(Exposure::exposure(app.lastAutoEV));
    } else {
        view.getCamera().setExposure(app.aperture, 1.0f / app.shutterSpeed, app.sensitivity);
    }
}

static void applyPbrtCameraProjection(App& app, View& view) {
    if (!app.hasPbrtCamera) {
        return;
    }

    const ::pbrtio::PbrtCameraSettings& cam = app.pbrtCamera;
    Camera& camera = view.getCamera();
    camera.setProjection(cam.verticalFovDegrees, cam.aspectRatio, cam.nearPlane, cam.farPlane,
            Camera::Fov::VERTICAL);
    FilamentApp::get().setCameraNearFar(cam.nearPlane, cam.farPlane);
}

static void resetPbrtCameraManipulator(App& app, View& view) {
    if (!app.hasPbrtCamera) {
        return;
    }

    const ::pbrtio::PbrtCameraSettings& cam = app.pbrtCamera;
    applyPbrtCameraProjection(app, view);
    FilamentApp::get().resetCameraManipulator(toFilament(cam.eye), toFilament(cam.target),
            toFilament(cam.up), cam.verticalFovDegrees, cam.farPlane);
}

static void renderPbrtKitchenGui(App& app, Engine*, View*) {
    ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("PBRT Kitchen");

    if (ImGui::Button("Strict preset")) {
        setStrictPreset(app);
    }
    ImGui::SameLine();
    if (ImGui::Button("Preview preset")) {
        setPreviewPreset(app);
    }

    if (app.sceneHost && app.sceneHost->isBuilt()) {
        const auto& scene = app.sceneHost->scene().scene;
        const auto& result = app.sceneHost->result();
        ImGui::Separator();
        ImGui::Text("Meshes: %zu / %zu", result.meshesLoaded, scene.meshes.size());
        ImGui::Text("Rect lights: %zu", result.rectAreaLightCount);
        ImGui::Text("Mesh import: %.1f ms", result.meshImportMs);
    }

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Auto exposure", &app.autoExposure);
        ImGui::Text("Auto EV100: %.2f", app.lastAutoEV);
        ImGui::BeginDisabled(!app.autoExposure);
        ImGui::SliderFloat("EV bias (higher = darker)", &app.autoExposureCompensation, -5.0f, 8.0f);
        ImGui::SliderFloat("Min EV", &app.autoExposureMinEV, 0.0f, app.autoExposureMaxEV);
        ImGui::SliderFloat("Max EV", &app.autoExposureMaxEV, app.autoExposureMinEV, 18.0f);
        ImGui::SliderFloat("Middle grey", &app.autoExposureMiddleGrey, 0.01f, 1.0f, "%.2f");
        ImGui::SliderFloat("IBL meter weight", &app.iblLuminanceScale, 0.0f, 1.0f, "%.4f");
        ImGui::SliderFloat("Rect meter weight", &app.rectLightLuminanceScale, 0.0f, 20.0f);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(app.autoExposure);
        ImGui::SliderFloat("Aperture", &app.aperture, 1.0f, 32.0f, "f/%.1f");
        ImGui::SliderFloat("Shutter 1/x", &app.shutterSpeed, 1.0f, 1000.0f, "1/%.0f");
        ImGui::SliderFloat("ISO", &app.sensitivity, 25.0f, 6400.0f, "%.0f");
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("IBL", &app.enableIBL);
        ImGui::BeginDisabled(!app.enableIBL);
        ImGui::SliderFloat("IBL intensity", &app.iblIntensity, 0.0f, 50000.0f, "%.0f");
        ImGui::SliderAngle("IBL rotation", &app.iblRotation);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Ambient Occlusion")) {
        ImGui::Checkbox("AO enabled", &app.enableAO);
        ImGui::BeginDisabled(!app.enableAO);
        ImGui::SliderFloat("AO radius", &app.aoOptions.radius, 0.05f, 5.0f);
        ImGui::SliderFloat("AO intensity", &app.aoOptions.intensity, 0.0f, 4.0f);
        ImGui::SliderFloat("AO power", &app.aoOptions.power, 0.1f, 4.0f);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Post Processing")) {
        ImGui::Checkbox("Bloom", &app.enableBloom);
        ImGui::BeginDisabled(!app.enableBloom);
        ImGui::SliderFloat("Bloom strength", &app.bloomOptions.strength, 0.0f, 1.0f);
        ImGui::Checkbox("Bloom threshold", &app.bloomOptions.threshold);
        ImGui::SliderFloat("Bloom highlight", &app.bloomOptions.highlight, 10.0f, 5000.0f, "%.0f");
        ImGui::EndDisabled();
        ImGui::Checkbox("SSR", &app.enableSSR);
        ImGui::Checkbox("Fog", &app.enableFog);
    }

    if (ImGui::CollapsingHeader("Background")) {
        ImGui::ColorEdit3("Clear color", app.clearColor);
    }

    ImGui::End();
}

int main(int argc, char** argv) {
    App app;
    app.config.title = "PBRT Kitchen";
    app.config.iblDirectory = "";
    app.config.cameraMode = filament::camutils::Mode::FREE_FLIGHT;
    app.scenePath = DEFAULT_SCENE;

    handleCommandLineArguments(argc, argv, &app);
    if (utils::getopt::optind < argc) {
        app.scenePath = argv[utils::getopt::optind];
    }

    if (app.strictPbrt) {
        setStrictPreset(app);
    } else {
        setPreviewPreset(app);
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
        if (pbrtScene.scene.environment.valid) {
            app.environmentPath = pbrtScene.scene.environment.mapPath;
        }
        if (!pbrtScene.scene.areaLights.empty()) {
            double luminanceSum = 0.0;
            for (const auto& rect : pbrtScene.scene.areaLights) {
                const auto radiance = rect.radiance * rect.scale;
                luminanceSum += radiance.x * 0.2126 + radiance.y * 0.7152 +
                        radiance.z * 0.0722;
            }
            app.rectLightAverageLuminance = static_cast<float>(
                    luminanceSum / double(pbrtScene.scene.areaLights.size()));
        }
        ::pbrtio::printPbrtLoadTimings(pbrtScene.timings);
        std::cout << "Loaded " << pbrtScene.scene.meshes.size() << " meshes, "
                  << pbrtScene.scene.areaLights.size() << " rect area lights, "
                  << pbrtScene.textures.decoded.size() << " textures from "
                  << app.scenePath << std::endl;

        if (!app.strictPbrt && pbrtScene.scene.environment.valid) {
            FilamentApp::get().loadIBL(pbrtScene.scene.environment.mapPath.string());
            std::cout << "Environment map: " << pbrtScene.scene.environment.mapPath << std::endl;
        } else if (app.strictPbrt) {
            std::cout << "Strict PBRT mode: IBL disabled, rect area lights + black background."
                      << std::endl;
        }

        std::cout << "Rendered " << result.meshesLoaded << " / "
                  << pbrtScene.scene.meshes.size() << " meshes." << std::endl;
        std::cout << "Mesh import time: " << result.meshImportMs << " ms." << std::endl;

        if (result.meshesLoaded == 0 && pbrtScene.scene.areaLights.empty()) {
            std::cerr << "No geometry loaded. Rebuild assimp (PLY support) and pbrt_kitchen."
                      << std::endl;
            return;
        }

        std::cout << "Scene analytic lights: " << result.analyticLightCount
                  << ", rect area lights: " << result.rectAreaLightCount
                  << std::endl;

        const ::pbrtio::PbrtCameraSettings& cam = pbrtScene.scene.camera;
        app.pbrtCamera = cam;
        app.hasPbrtCamera = true;
        std::cout << "PBRT camera eye=(" << cam.eye.x << ", " << cam.eye.y << ", "
                  << cam.eye.z << ") target=(" << cam.target.x << ", " << cam.target.y
                  << ", " << cam.target.z << ") up=(" << cam.up.x << ", " << cam.up.y
                  << ", " << cam.up.z << ") fov=" << cam.verticalFovDegrees
                  << " near=" << cam.nearPlane << " far=" << cam.farPlane << std::endl;
        std::cout << "PBRT scene center=(" << pbrtScene.scene.sceneCenter.x << ", "
                  << pbrtScene.scene.sceneCenter.y << ", " << pbrtScene.scene.sceneCenter.z
                  << ") radius=" << pbrtScene.scene.sceneRadius << std::endl;
        view->getCamera().setExposure(app.aperture, 1.f / app.shutterSpeed, app.sensitivity);
        resetPbrtCameraManipulator(app, *view);
        applyPbrtView(app, *view, *scene);
    };

    auto gui = [&app](Engine* engine, View* view) {
        renderPbrtKitchenGui(app, engine, view);
    };

    auto preRender = [&app](Engine*, View* view, Scene* scene, Renderer* renderer) {
        applyPbrtCameraProjection(app, *view);
        applyPbrtView(app, *view, *scene);
        renderer->setClearOptions({
                .clearColor = { app.clearColor[0], app.clearColor[1],
                        app.clearColor[2], app.clearColor[3] },
                .clear = true });
        if (app.enableIBL && !FilamentApp::get().getIBL()) {
            renderer->setClearOptions({
                    .clearColor = { app.clearColor[0], app.clearColor[1],
                            app.clearColor[2], app.clearColor[3] },
                    .clear = true });
        }
    };

    auto cleanup = [&app](Engine* engine, View*, Scene* scene) {
        if (app.sceneHost) {
            app.sceneHost->destroy(*engine, *scene);
            app.sceneHost.reset();
        }
    };

    FilamentApp::get().run(app.config, setup, cleanup, gui, preRender);
    return 0;
}
