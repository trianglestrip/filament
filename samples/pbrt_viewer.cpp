/*
 * PBRT scene viewer sample.
 * Uses the same ViewerGui sidebar as gltf_viewer, with PBRT-specific panels via setUiCallback.
 */

#include "common/arguments.h"

#include "PbrtFilamentSceneHost.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/View.h>

#include <camutils/Manipulator.h>

#include <filamentapp/Config.h>
#include <filamentapp/FilamentApp.h>

#include <viewer/AutomationEngine.h>
#include <viewer/Settings.h>
#include <viewer/ViewerGui.h>

#include <imgui.h>
#include <math/mat3.h>
#include <utils/Path.h>
#include <utils/getopt.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#undef near
#undef far
#endif

#include "generated/resources/resources.h"

#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

using namespace filament;
using namespace filament::viewer;
using namespace utils;

struct App {
    Config config;
    std::filesystem::path scenePath;
    std::filesystem::path environmentPath;
    bool strictPbrt = true;
    bool enableIBL = false;
    ViewerGui* viewer = nullptr;
    float rectLightLuminanceScale = 1.0f;
    float rectLightAverageLuminance = 1.0f;
    bool hasPbrtCamera = false;
    bool screenshot = false;
    bool headless = false;
    bool captureRequested = false;
    bool capturedWithAO = false;
    bool capturedWithoutAO = false;
    uint32_t captureFrame = 0;
    uint32_t captureCloseCountdown = 0;
    uint32_t captureSkipFrames = 8;
    uint32_t windowWidth = 1280;
    uint32_t windowHeight = 720;
    std::string capturePrefix;
    ::pbrtio::PbrtCameraSettings pbrtCamera;
    std::unique_ptr<filament::pbrtio::PbrtFilamentSceneHost> sceneHost;

    char browsePath[512] = {0};
    bool pendingReload = false;
    std::string statusMessage;
};

static const char* DEFAULT_IBL = "assets/ibl/lightroom_14b";
static const char* DEFAULT_SCENE =
        "D:/models/pbrt-v4-scenes/barcelona-pavilion/pavilion-day.pbrt";

static filament::math::float3 toFilament(const ::pbrtio::pbrt::float3& v) {
    return filament::math::float3(v.x, v.y, v.z);
}

static filament::pbrtio::PbrtFilamentBuildOptions makeBuildOptions(bool strictPbrt);

#ifdef _WIN32
static bool openPbrtFileDialog(char* outPath, size_t outSize) {
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "PBRT Files (*.pbrt)\0*.pbrt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = outPath;
    ofn.nMaxFile = (DWORD)outSize;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) != FALSE;
}
#endif

static bool loadPbrtScene(App& app, Engine* engine, Scene* scene) {
    if (app.sceneHost) {
        app.sceneHost->destroy(*engine, *scene);
        app.sceneHost.reset();
    }

    app.hasPbrtCamera = false;
    app.environmentPath.clear();

    app.sceneHost = std::make_unique<filament::pbrtio::PbrtFilamentSceneHost>();
    if (!app.sceneHost->build(*engine, *scene, app.scenePath,
            makeBuildOptions(app.strictPbrt))) {
        std::cerr << "Failed to load PBRT scene: " << app.scenePath << std::endl;
        app.sceneHost.reset();
        return false;
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
        std::cerr << "No geometry loaded." << std::endl;
        return false;
    }

    std::cout << "Scene analytic lights: " << result.analyticLightCount
              << ", rect area lights: " << result.rectAreaLightCount
              << std::endl;

    const ::pbrtio::PbrtCameraSettings& cam = pbrtScene.scene.camera;
    app.pbrtCamera = cam;
    app.hasPbrtCamera = true;
    if (app.viewer) {
        auto& settings = app.viewer->getSettings();
        (settings.camera).near = cam.nearPlane;
        (settings.camera).far = cam.farPlane;
    }
    std::cout << "PBRT camera eye=(" << cam.eye.x << ", " << cam.eye.y << ", "
              << cam.eye.z << ") target=(" << cam.target.x << ", " << cam.target.y
              << ", " << cam.target.z << ") up=(" << cam.up.x << ", " << cam.up.y
              << ", " << cam.up.z << ") fov=" << cam.verticalFovDegrees
              << " near=" << cam.nearPlane << " far=" << cam.farPlane << std::endl;
    std::cout << "PBRT scene center=(" << pbrtScene.scene.sceneCenter.x << ", "
              << pbrtScene.scene.sceneCenter.y << ", " << pbrtScene.scene.sceneCenter.z
              << ") radius=" << pbrtScene.scene.sceneRadius << std::endl;

    return true;
}

static void setStrictPreset(App& app) {
    app.strictPbrt = true;
    app.enableIBL = false;
    app.rectLightLuminanceScale = 1.0f;
    if (app.viewer) {
        auto& settings = app.viewer->getSettings();
        settings.view.ssao.enabled = false;
        settings.view.bloom.enabled = false;
        settings.view.screenSpaceReflections.enabled = false;
        settings.view.fog.enabled = false;
        settings.view.dithering = Dithering::TEMPORAL;
        settings.lighting.iblIntensity = 0.0f;
        settings.viewer.autoScaleEnabled = true;
        settings.viewer.skyboxEnabled = false;
        settings.viewer.backgroundColor = sRGBColor(0.0f, 0.0f, 0.0f);
    }
}

static void setPreviewPreset(App& app) {
    app.strictPbrt = false;
    app.enableIBL = true;
    app.rectLightLuminanceScale = 1.0f;
    if (app.viewer) {
        auto& settings = app.viewer->getSettings();
        settings.view.ssao.enabled = true;
        settings.view.ssao.radius = 0.45f;
        settings.view.ssao.intensity = 0.55f;
        settings.view.ssao.power = 1.0f;
        settings.view.bloom.enabled = false;
        settings.view.bloom.strength = 0.03f;
        settings.view.screenSpaceReflections.enabled = false;
        settings.view.fog.enabled = false;
        settings.view.dithering = Dithering::TEMPORAL;
        settings.lighting.iblIntensity = 30000.0f;
        settings.viewer.autoScaleEnabled = true;
        settings.viewer.skyboxEnabled = true;
        settings.viewer.backgroundColor = sRGBColor(0.03f, 0.03f, 0.035f);
    }
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
            "   --headless\n"
            "       Render without a visible window\n\n"
            "   --width=<pixels>, --height=<pixels>\n"
            "       Capture/window size\n\n"
            "   --capture-prefix=<path>\n"
            "       Export <path>_final_ao.ppm and <path>_final_no_ao.ppm, then exit\n\n"
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
    static constexpr int OPT_HEADLESS = 1001;
    static constexpr int OPT_WIDTH = 1002;
    static constexpr int OPT_HEIGHT = 1003;
    static constexpr int OPT_CAPTURE_PREFIX = 1004;
    static constexpr int OPT_CAPTURE_SKIP = 1005;
    static const utils::getopt::option OPTIONS[] = {
            { "help",  utils::getopt::no_argument,       nullptr, 'h' },
            { "api",   utils::getopt::required_argument, nullptr, 'a' },
            { "scene", utils::getopt::required_argument, nullptr, 's' },
            { "ibl",   utils::getopt::no_argument,       nullptr, 'i' },
            { "preview", utils::getopt::no_argument,     nullptr, 'p' },
            { "strict", utils::getopt::no_argument,      nullptr, OPT_STRICT },
            { "headless", utils::getopt::no_argument,    nullptr, OPT_HEADLESS },
            { "width", utils::getopt::required_argument, nullptr, OPT_WIDTH },
            { "height", utils::getopt::required_argument, nullptr, OPT_HEIGHT },
            { "capture-prefix", utils::getopt::required_argument, nullptr, OPT_CAPTURE_PREFIX },
            { "capture-skip", utils::getopt::required_argument, nullptr, OPT_CAPTURE_SKIP },
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
            case OPT_HEADLESS:
                app->headless = true;
                app->config.headless = true;
                break;
            case OPT_WIDTH:
                app->windowWidth = std::max(1, std::atoi(arg.c_str()));
                break;
            case OPT_HEIGHT:
                app->windowHeight = std::max(1, std::atoi(arg.c_str()));
                break;
            case OPT_CAPTURE_PREFIX:
                app->capturePrefix = arg;
                app->captureRequested = !app->capturePrefix.empty();
                break;
            case OPT_CAPTURE_SKIP:
                app->captureSkipFrames = std::max(0, std::atoi(arg.c_str()));
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

static void setupIBL(App& app) {
    auto ibl = FilamentApp::get().getIBL();
    if (ibl && app.viewer) {
        app.viewer->setIndirectLight(ibl->getIndirectLight(), ibl->getSphericalHarmonics());
        app.viewer->getSettings().view.fogSettings.fogColorTexture = ibl->getFogTexture();
    }
}

static void syncIbl(App& app, Scene& scene) {
    if (!app.viewer) {
        return;
    }

    if (app.enableIBL) {
        if (!FilamentApp::get().getIBL() && !app.environmentPath.empty()) {
            FilamentApp::get().loadIBL(app.environmentPath.string());
        }
        if (auto* ibl = FilamentApp::get().getIBL()) {
            scene.setIndirectLight(ibl->getIndirectLight());
            scene.setSkybox(ibl->getSkybox());
            app.viewer->setIndirectLight(ibl->getIndirectLight(), ibl->getSphericalHarmonics());
            app.viewer->getSettings().view.fogSettings.fogColorTexture = ibl->getFogTexture();
        }
    } else {
        scene.setIndirectLight(nullptr);
        scene.setSkybox(nullptr);
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

static void setupPbrtUiCallback(App& app) {
    app.viewer->setUiCallback([&app]() {
        ImGui::Text("PBRT scene");
        ImGui::PushItemWidth(-1);
        ImGui::InputText("##scenepath", app.browsePath, sizeof(app.browsePath));
        ImGui::PopItemWidth();
        if (ImGui::Button("Browse...")) {
#ifdef _WIN32
            if (openPbrtFileDialog(app.browsePath, sizeof(app.browsePath))) {
                app.statusMessage.clear();
            }
#else
            app.statusMessage = "File browser not available on this platform";
#endif
        }
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            std::string newPath(app.browsePath);
            if (!newPath.empty() && std::filesystem::exists(newPath)) {
                app.scenePath = newPath;
                app.pendingReload = true;
                app.statusMessage.clear();
            } else {
                app.statusMessage = "File not found: " + newPath;
            }
        }

        if (!app.statusMessage.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", app.statusMessage.c_str());
        }

        if (ImGui::Button("Strict preset")) {
            setStrictPreset(app);
        }
        ImGui::SameLine();
        if (ImGui::Button("Preview preset")) {
            setPreviewPreset(app);
        }

        if (ImGui::CollapsingHeader("Stats")) {
            ImGui::Indent();
            if (app.sceneHost && app.sceneHost->isBuilt()) {
                const auto& pbrtScene = app.sceneHost->scene().scene;
                const auto& result = app.sceneHost->result();
                ImGui::Text("File: %s", app.scenePath.filename().string().c_str());
                ImGui::Text("Meshes: %zu / %zu", result.meshesLoaded, pbrtScene.meshes.size());
                ImGui::Text("Rect lights: %zu", result.rectAreaLightCount);
                ImGui::Text("Analytic lights: %zu", result.analyticLightCount);
                ImGui::Text("Mesh import: %.1f ms", result.meshImportMs);
            } else {
                ImGui::Text("No scene loaded");
            }
            ImGui::Text("%zu skipped frames", FilamentApp::get().getSkippedFrameCount());
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader("PBRT Lighting")) {
            ImGui::Indent();
            ImGui::SliderFloat("Rect light scale", &app.rectLightLuminanceScale, 0.0f, 20.0f);
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader("Debug")) {
            ImGui::Indent();
            if (ImGui::Button("Screenshot")) {
                app.screenshot = true;
            }
            bool cameraFrustum = FilamentApp::get().isCameraFrustumEnabled();
            ImGui::Checkbox("Show Camera Frustum", &cameraFrustum);
            FilamentApp::get().setCameraFrustumEnabled(cameraFrustum);
            ImGui::Unindent();
        }
    });
}

int main(int argc, char** argv) {
    App app;
    app.config.title = "Filament";
    app.config.iblDirectory = FilamentApp::getRootAssetsPath() + DEFAULT_IBL;
    app.config.cameraMode = filament::camutils::Mode::FREE_FLIGHT;
    app.scenePath = DEFAULT_SCENE;
    std::strncpy(app.browsePath, app.scenePath.string().c_str(), sizeof(app.browsePath) - 1);
    app.browsePath[sizeof(app.browsePath) - 1] = '\0';

    handleCommandLineArguments(argc, argv, &app);
    if (utils::getopt::optind < argc) {
        app.scenePath = argv[utils::getopt::optind];
        std::strncpy(app.browsePath, app.scenePath.string().c_str(), sizeof(app.browsePath) - 1);
        app.browsePath[sizeof(app.browsePath) - 1] = '\0';
    }

    if (app.strictPbrt) {
        setStrictPreset(app);
        app.config.iblDirectory = "";
    } else {
        setPreviewPreset(app);
    }

    auto setup = [&app](Engine* engine, View* view, Scene* scene) {
        app.viewer = new ViewerGui(engine, scene, view, 410);
        app.viewer->getSettings().viewer.autoScaleEnabled = true;

        if (app.strictPbrt) {
            setStrictPreset(app);
        } else {
            setPreviewPreset(app);
        }

        setupPbrtUiCallback(app);
        setupIBL(app);

        if (!loadPbrtScene(app, engine, scene)) {
            return;
        }

        syncIbl(app, *scene);
        resetPbrtCameraManipulator(app, *view);
    };

    auto gui = [&app](Engine*, View*) {
        app.viewer->updateUserInterface();
        FilamentApp::get().setSidebarWidth(app.viewer->getSidebarWidth());
    };

    auto preRender = [&app](Engine* engine, View* view, Scene* scene, Renderer* renderer) {
        if (app.pendingReload) {
            app.pendingReload = false;
            if (loadPbrtScene(app, engine, scene)) {
                syncIbl(app, *scene);
                resetPbrtCameraManipulator(app, *view);
                app.statusMessage = "Loaded: " + app.scenePath.filename().string();
            } else {
                app.statusMessage = "Failed to load: " + app.scenePath.filename().string();
            }
        }

        applyPbrtCameraProjection(app, *view);
        syncIbl(app, *scene);

        FilamentApp::get().setCameraNearFar(app.viewer->getSettings().camera.near,
                app.viewer->getSettings().camera.far);
        FilamentApp::get().setCameraFocalLength(app.viewer->getSettings().camera.focalLength);

        Camera& camera = view->getCamera();
        Skybox* skybox = scene->getSkybox();
        applySettings(engine, app.viewer->getSettings().viewer, &camera, skybox, renderer);
        const double aspect =
                (double) view->getViewport().width / (double) view->getViewport().height;
        applySettings(engine, app.viewer->getSettings().camera, &camera, aspect);
        applySettings(engine, app.viewer->getSettings().debug, renderer);
        applySettings(engine, app.viewer->getSettings().view, view);
        view->setShadowingEnabled(true);
    };

    auto postRender = [&app](Engine*, View* view, Scene*, Renderer* renderer) {
        if (app.captureRequested) {
            app.captureFrame++;
            if (app.captureFrame >= app.captureSkipFrames && !app.capturedWithAO) {
                AutomationEngine::exportScreenshot(view, renderer,
                        app.capturePrefix + "_final_ao.ppm", false, nullptr);
                app.capturedWithAO = true;
                app.viewer->getSettings().view.ssao.enabled = false;
                app.captureFrame = 0;
            } else if (app.capturedWithAO && app.captureFrame >= 2 && !app.capturedWithoutAO) {
                AutomationEngine::exportScreenshot(view, renderer,
                        app.capturePrefix + "_final_no_ao.ppm", false, nullptr);
                app.capturedWithoutAO = true;
                app.captureCloseCountdown = 12;
            } else if (app.capturedWithoutAO && app.captureCloseCountdown > 0) {
                if (--app.captureCloseCountdown == 0) {
                    FilamentApp::get().close();
                }
            }
        }
        if (app.screenshot) {
            AutomationEngine::exportScreenshot(view, renderer, "screenshot.png", false, nullptr);
            app.screenshot = false;
        }
    };

    auto cleanup = [&app](Engine* engine, View*, Scene* scene) {
        if (app.sceneHost) {
            app.sceneHost->destroy(*engine, *scene);
            app.sceneHost.reset();
        }
        delete app.viewer;
        app.viewer = nullptr;
    };

    FilamentApp& filamentApp = FilamentApp::get();
    filamentApp.setDropHandler([&](std::string_view path) {
        if (path.size() < 5 || path.substr(path.size() - 5) != ".pbrt") {
            return;
        }
        app.scenePath = std::string(path);
        std::strncpy(app.browsePath, app.scenePath.string().c_str(), sizeof(app.browsePath) - 1);
        app.browsePath[sizeof(app.browsePath) - 1] = '\0';
        app.pendingReload = true;
        app.statusMessage.clear();
    });

    filamentApp.run(app.config, setup, cleanup, gui, preRender, postRender,
            app.windowWidth, app.windowHeight);
    return 0;
}
