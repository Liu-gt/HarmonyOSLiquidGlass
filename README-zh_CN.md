# 鸿蒙液态玻璃

[English](./README.md) | 简体中文

面向 HarmonyOS 的液态玻璃（backdrop）实现。玻璃材质与玻璃页签条由 C++ 通过 `XComponent` + EGL/GLES 自绘，文字用 `native_drawing` 栅格化。仓库根目录是库工程（`backdrop/` HAR 模块），可运行的示例应用是 [`demo/`](./demo) 下的独立 DevEco 工程。

## 功能

- 液态玻璃材质：模糊、折射、色散、边缘高光、投影、内阴影。
- **Backdrop Catalog** 示例：提供组件全景目录，涵盖独立的原生 GLES 渲染场景。
- 玻璃页签条：底板、可拖动指示器、按压形变、速度形变、弹簧回弹、跟手高光。
- 丰富玻璃组件：按钮、开关、滑块、对话框、控制中心、锁屏、放大镜、自适应亮度玻璃、渐进模糊、滚动容器、玻璃游乐场。
- 页签场景内置三个在 GL 表面绘制的原生交互按钮。
- **ArkTS 动态内容实时透射**：通过 `BuilderNode(RENDER_TYPE_TEXTURE)` 将 ArkUI 组件树渲染至 GPU 纹理（接收端为 `OH_NativeImage`），GL 渲染线程逐帧采样。纹理在 GPU 内部流转，无 CPU 回读开销，约一帧延迟；背景组件保持动态动画与触控交互。
- 页签场景支持在应用内实时调节 83 个数值参数与 8 个布尔开关；其余场景采用对齐参考实现（Kyant Compose 版本）的预设参数。
- 静态照片背景：采用 cover 填充适配，支持纵向拖拽平移浏览。
- 功耗优化：静止状态自动暂停重绘以降低功耗；交互时动态提升至 120fps，常规动画维持 60fps；页面不可见时可通过 `GlassGLSetPaused` 暂停渲染循环。

## 目录（Demo catalog）

首页条目涵盖主要效果展示，点击条目进入全屏 `XComponent(SURFACE)` 场景，画面均由原生 `libbackdrop.so` 渲染：

| 分组 | 条目 |
| --- | --- |
| Liquid glass components | Buttons、Toggle、Slider、Bottom tabs、Dialog |
| System UIs | Lock screen（钟表）、Control center、Magnifier |
| Experiments | Glass playground、Adaptive luminance glass、Progressive blur、Scroll container、Lazy scroll container |
| ArkUI (ArkTS) | ArkTS content glass |

「ArkTS content glass」展示混合渲染方案：背景为标准 ArkUI 组件树，通过
`BuilderNode(RENDER_TYPE_TEXTURE)` 离屏渲染至库创建的 `OH_NativeImage` 纹理，GL 渲染器直接将其
作为背景采样源。玻璃材质能够实时折射动态 ArkUI 界面（如加载动画、`TextClock` 时钟、滑块与
开关等）；触摸事件经由上层透明交互层通过 `postInputEvent` 转发，背景组件在玻璃覆盖下
正常响应交互。

「Glass playground」支持通过滑块实时调节材质面板参数；「Bottom tabs」提供完整的实时参数调节面板。

## 目录结构

```
.
├── backdrop/                          # 库：HAR 模块
│   ├── Index.ets                      # ArkTS 出口（转出 libbackdrop.so 的接口）
│   ├── oh-package.json5
│   ├── build-profile.json5
│   └── src/main/
│       ├── module.json5
│       └── cpp/
│           ├── CMakeLists.txt         # 产出 libbackdrop.so
│           ├── napi_init.cpp          # NAPI 模块注册
│           ├── glass/                 # GLES 渲染器
│           │   ├── glass_napi.{h,cpp}
│           │   ├── gles_renderer.{h,cpp}
│           │   ├── glass_params.{h,cpp}
│           │   ├── glass_scene.h      # 目录场景枚举
│           │   └── text_layer.{h,cpp}
│           └── types/libbackdrop/     # NAPI 类型声明
├── demo/                              # 示例应用：独立 DevEco 工程
│   ├── AppScope/
│   ├── build-profile.json5            # 工程级构建与签名配置文件
│   ├── entry/
│   │   ├── oh-package.json5           # "backdrop": "file:../../backdrop"
│   │   └── src/main/
│   │       ├── ets/
│   │       │   ├── entryability/EntryAbility.ets
│   │       │   ├── pages/Index.ets    # 入口：目录首页 <-> 场景页
│   │       │   └── catalog/
│   │       │       ├── Scenes.ets     # 场景表（id 与 glass_scene.h 一致）
│   │       │       ├── CatalogHome.ets
│   │       │       ├── ScenePage.ets  # XComponent 宿主 + 参数菜单
│   │       │       ├── ArkTsGlassContent.ets # 导出为 GL 纹理的 ArkUI 内容
│   │       │       └── Specs.ets      # 页签参数表
│   │       ├── module.json5
│   │       └── resources/rawfile/photo.rgba
│   └── tools/make-photo-raw.py
├── build-and-run.sh                   # 构建 / 安装 / 启动 demo
└── LICENSE
```

## 环境要求

- DevEco Studio（含 HarmonyOS SDK，compatible/target SDK `26.0.0`）。
- `hdc`，用于部署到设备。
- 已连接的 HarmonyOS 设备或模拟器。

## 构建与运行

### 示例应用（需要签名）

DevEco Studio：

1. 打开 `demo/` 目录。
2. `File → Project Structure… → Signing Configs`：勾选自动签名并登录华为开发者账号。
3. 选择 `entry` 模块，运行。

命令行（在仓库根目录执行）：

```bash
./build-and-run.sh              # 打包、安装、启动
./build-and-run.sh --no-deploy  # 只打包
```

注：安装至设备需要签名 HAP，请先在 DevEco Studio 中完成签名配置。
示例工程通过 `file:../../backdrop` 依赖库源码，构建示例工程时会自动编译库模块，无需预先单独打包 HAR。

### 库（无需签名）

用 DevEco Studio 打开仓库根目录，或执行 `assembleHar`：

```bash
<DevEco>/tools/node/bin/node <DevEco>/tools/hvigor/bin/hvigorw.js \
  --no-daemon --mode module -p module=backdrop@default -p product=default assembleHar
```

产物：`backdrop/build/default/outputs/default/backdrop.har`。

## 参数

三个应用内菜单覆盖 83 个数值参数与 8 个开关。

| 菜单 | 内容 |
| --- | --- |
| 页签底座 | 材质（模糊、折射、底色）、按钮、布局、动画 |
| 指示器 · 静止 | 静止折射、高光、阴影、布局、模糊 |
| 指示器 · 拖动 | 按压折射、高光、阴影、速度形变、弹簧 |

参数编号（`number 0..82`、`bool 0..7`）必须在
`backdrop/src/main/cpp/glass/glass_params.{h,cpp}` 与
`demo/entry/src/main/ets/catalog/Specs.ets` 之间保持一致。

场景编号必须在 `backdrop/src/main/cpp/glass/glass_scene.h` 与
`demo/entry/src/main/ets/catalog/Scenes.ets` 之间保持一致。

## 库接口

| 接口 | 用途 |
| --- | --- |
| `GlassGLSetScene(scene)` | 切换展示场景（0 为页签条） |
| `GlassGLSetDensity(vp2px)` | 设置屏幕像素密度，供原生层计算布局 |
| `GlassGLSetNumber/SetBool(id, value)` | 设置页签条数值/布尔调节参数 |
| `GlassGLSetPhoto(buffer)` | 设置自定义背景 RGBA 数据（前 8 字节为宽高元数据） |
| `GlassGLGetContentSurfaceId()` | 获取 ArkTS 内容纹理（`OH_NativeImage`）的 Surface ID，供 `BuilderNode(RENDER_TYPE_TEXTURE)` 绑定；渲染线程就绪前返回空字符串 |
| `GlassGLSetPaused(paused)` | 暂停/恢复渲染：页面不可见时传 true 以降低功耗，恢复可见时传 false |
| `GlassGLRegisterMenuCallback(cb)` | 注册原生层玻璃按钮触发参数菜单的回调 |

## 实现说明

- 渲染器运行在独立线程，通过 EGL 上下文绘制到 XComponent 的 Surface。
- 帧率与空闲调度：`eglSwapBuffers` 在 XComponent Surface 上不一定受 VSync 阻塞（部分机型或窗口模式下可能突破垂直同步限制导致过高帧率），因此渲染循环内置了帧率调度机制及空闲检测机制。帧率调度对齐 `OH_NativeVSync`（`WaitForRenderSlot`）：每个 VSync 周期最多出一帧，出帧点在 VSync 之后 0.4 个周期（夹在 1.5~4ms，让本周期的触摸与内容帧先到齐），交互期间每个 VSync 都出帧（120fps），非交互期间帧间隔取最接近 16ms 的 VSync 整数倍（120Hz 屏隔一个 VSync 出一帧，约 60fps）；交换前设置期望上屏时间（`SET_DESIRED_PRESENT_TIMESTAMP`），RenderService 只取最新一帧，避免队列积压造成的额外一帧延迟。VSync 不可用时退回定时节拍（`PaceFrame`：8ms / 16ms）。空闲检测机制：在无输入事件（触摸、参数修改、场景切换、尺寸改变、纹理更新）且物理弹簧累积位移低于阈值时跳过重绘，Surface 维持当前帧。位移量由所有弹簧步进统一累加（`gSpringMotion`）并按速率判定（与单轮步长无关）；判定停止时页签弹簧须已贴近目标才对齐到目标值，确保物理过渡动画平滑执行完毕、静止画面与帧时序无关。
- ArkTS 内容纹理管线：渲染线程创建 `OH_NativeImage` 并将 Surface ID 传递至 ArkTS 层；`BuilderNode(RENDER_TYPE_TEXTURE)` 将 ArkUI 组件树离屏渲染至该 Surface。渲染线程在 VSync 信号到达时获取最新图像帧（`OH_NativeImage_UpdateSurfaceImage`），随后 Blit 为标准 2D 纹理以适配材质着色器的 `sampler2D` 采样要求。针对外部纹理直接采样的垂直镜像问题，结合 `OH_NativeImage` 的变换矩阵进行坐标校正。
- `photo.rgba` 是无压缩 RGBA 缓冲，前缀 8 字节为 `u32` LE 宽、`u32` LE 高，内容为 Kyant demo 的壁纸（`wallpaper_light.webp`）。可用 `python3 demo/tools/make-photo-raw.py <图片> [宽度]` 从任意图片重新生成。
- 源码注释中的“参考实现”指本效果所对齐的 Compose Multiplatform 版本；本仓库是对应的独立 GLES 重实现。

## 功耗与帧率

渲染模块内置了自适应重绘与动态帧率调度机制，应用层仅需在生命周期切换时同步可见状态：

| 运行场景 | 渲染调度策略 | 应用层接入建议 |
| --- | --- | --- |
| 静止（无触摸、无动画、无参数更新） | 暂停重绘，维持低频空闲检测 | 无需处理 |
| 触摸交互 / 物理弹簧过渡中 | 触摸后 400ms 内按 120fps 调度，常规动画维持 60fps | 无需处理 |
| 参数 / 场景 / 背景图片更新 | 触发单帧重绘 | 无需处理 |
| 页面不可见（后台挂起、页面遮挡、路由切换） | 暂停渲染循环 | 在 `onPageShow` / `onPageHide` 中调用 `GlassGLSetPaused(false/true)` |
| ArkTS 内容纹理场景 | 随 ArkUI 帧产出驱动重绘 | 若无持续动画需求，建议避免包含常驻动画组件（如 `LoadingProgress`、`TextClock`），防止频繁触发重绘 |

性能监控：过滤 `PERF_GL` 日志可观察渲染状态。其中 `fps/frames` 为实际绘制帧数，`idle` 为空闲检测跳过的帧数；静止状态下预期表现为 `frames=0, idle≈60`。

## 许可证

Apache License 2.0，见 [`LICENSE`](./LICENSE)。Copyright 2025 Kyant（backdrop 库），Copyright 2026 Liugt（鸿蒙移植）。

感谢 Kyant 的液态玻璃效果设计。
