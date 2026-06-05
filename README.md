# Filament — PBRT 集成分支（当前改动说明）

本仓库在 [Google Filament](https://github.com/google/filament) 基础上扩展 **PBRT v4 场景加载与预览**，分支：`feature/pbrt-kitchen`。

> 上游完整介绍、特性列表与 Maven/CocoaPods 说明见 [官方 README](https://github.com/google/filament/blob/main/README.md) 与 [BUILDING.md](BUILDING.md)。

---

## 当前改动摘要

### 1. `libs/pbrtio` — PBRT 解析与 Filament 桥接

| 组件 | 说明 |
|------|------|
| `PbrtSceneLoader.cpp` | PBRT v4 场景解析入口（网格、材质、相机、光源） |
| `FilamentPbrtLoader` | 扩展：PLY 网格实例、纹理路径、**矩形面光源**（`AreaLightSource "diffuse"`）、`infinite` 环境贴图、相机参数 |
| `PbrtFilamentSceneHost` | 封装 Filament 资源生命周期：加载 mesh、创建材质实例、挂载矩形区光与解析光源 |
| `taskflow` | 并行 mesh 校验/加载（`third_party/taskflow`） |

### 2. `samples/pbrt_kitchen` — Kitchen 场景样例

- 默认场景：`scene-v4.pbrt`（可用 `--scene` 指定）
- **严格 PBRT 模式（默认）**：无 IBL/天空盒、关闭 AO/Bloom/SSR/Fog，黑色背景，仅依赖场景内 **矩形面光源** + 阴影
- **`--ibl`**：启用默认 `lightroom_14b` IBL；若 PBRT 含 `Light "infinite"` 则加载环境贴图
- 使用内嵌材质：`aiDefaultMat`、`pbrtTextured`、`sandboxUnlit`

### 3. 引擎与着色器 — 矩形区光（RECT）

- `LightManager::Type::RECT`：定向矩形面光源（单面发射）
- `Scene.cpp`：RECT 光源纳入 froxel/光照更新路径
- `surface_light_punctual.fs` / `surface_lighting.fs`：前向路径支持矩形区光采样

---

## 构建（Windows / VS2022）

```bat
cd D:\gitProject\filament
mkdir out
cd out
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release --target pbrt_kitchen
```

产物：`out\bin/Release/pbrt_kitchen.exe`

## 运行

```bat
out\bin\Release\pbrt_kitchen.exe --scene D:\gitProject\VLR_WF\models\kitchen\scene-v4.pbrt
```

可选：`--ibl` 开启 IBL；`--help` 查看后端 API 参数。

---

## 与 FalcorRendering 的关系

[FalcorRendering](https://github.com/trianglestrip/FalcorRendering) 的 `PBRTOfflineRenderer` 在 Falcor 上实现了 Filament 风格后处理与 IBL 对齐；本分支在 **原生 Filament** 上验证 PBRT 几何与面光源，用于交叉对比 kitchen 场景观感。

---

## 许可

遵循上游 [LICENSE](LICENSE)。非 Google 官方支持产品。
