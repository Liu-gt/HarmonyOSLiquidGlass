# HarmonyOS Liquid Glass

English | [简体中文](./README-zh_CN.md)

Liquid Glass (backdrop) for HarmonyOS. The glass material and the glass tab bar are rendered with `XComponent` + EGL/GLES in C++; text is rasterized with `native_drawing`. The repository root is the library project (the `backdrop/` HAR module); the runnable demo is a standalone DevEco project in [`demo/`](./demo).

## Features

- Liquid Glass material: blur, refraction, chromatic aberration, edge highlight, drop shadow, inner shadow.
- **Backdrop Catalog** demo: a home page mirroring Kyant's catalog, with one native GLES scene per component.
- Glass tab bar: panel, draggable indicator, press deformation, velocity deformation, spring return, interactive highlight.
- More glass components: buttons, toggle, slider, dialog, control center, lock screen, magnifier, adaptive luminance glass, progressive blur, scroll containers, glass playground.
- Three glass buttons rendered inside the GL surface (tab bar scene).
- **Live ArkTS content through the glass**: an ArkUI component tree is exported to a GPU texture with `BuilderNode(RENDER_TYPE_TEXTURE)` (receiving end: `OH_NativeImage`) and sampled by the GL renderer every frame — GPU-to-GPU, no CPU readback, about one frame of latency. The content keeps animating and stays interactive under the glass.
- 83 numeric parameters and 8 toggles for the tab bar, adjustable in-app. The other scenes use fixed values aligned with the reference (Kyant's Compose) implementation.
- Static photo background: cover-fit scaling with vertical drag-to-scroll support.
- Power-efficient: rendering is paused during idle states; frame rates are dynamically paced at 120 fps during interaction and 60 fps otherwise, and `GlassGLSetPaused` pauses rendering completely when the page is not visible.

## Demo catalog

The home page lists catalog destinations; each entry opens a full-screen `XComponent(SURFACE)` scene rendered by native `libbackdrop.so`:

| Group | Entries |
| --- | --- |
| Liquid glass components | Buttons, Toggle, Slider, Bottom tabs, Dialog |
| System UIs | Lock screen (clock), Control center, Magnifier |
| Experiments | Glass playground, Adaptive luminance glass, Progressive blur, Scroll container, Lazy scroll container |
| ArkUI (ArkTS) | ArkTS content glass |

"ArkTS content glass" demonstrates hybrid rendering: the background is a standard ArkUI component tree rendered off-screen into an `OH_NativeImage` texture via `BuilderNode(RENDER_TYPE_TEXTURE)`, which the GL renderer samples directly as the backdrop. The glass material refracts live ArkUI content (including loading spinners, `TextClock`, sliders, and toggles); touch events are forwarded from a transparent overlay above the XComponent using `postInputEvent`, keeping underlying components interactive.

"Glass playground" provides sliders to tune material panel parameters in real time; "Bottom tabs" offers a full in-app parameter adjustment menu.

## Layout

```
.
├── backdrop/                          # Library: HAR module
│   ├── Index.ets                      # ArkTS entry (re-exports libbackdrop.so)
│   ├── oh-package.json5
│   ├── build-profile.json5
│   └── src/main/
│       ├── module.json5
│       └── cpp/
│           ├── CMakeLists.txt         # builds libbackdrop.so
│           ├── napi_init.cpp          # NAPI module registration
│           ├── glass/                 # GLES renderer
│           │   ├── glass_napi.{h,cpp}
│           │   ├── gles_renderer.{h,cpp}
│           │   ├── glass_params.{h,cpp}
│           │   └── text_layer.{h,cpp}
│           └── types/libbackdrop/     # NAPI type declarations
├── demo/                              # Sample app: standalone DevEco project
│   ├── AppScope/
│   ├── build-profile.json5            # Project build and signing configuration
│   ├── entry/
│   │   ├── oh-package.json5           # "backdrop": "file:../../backdrop"
│   │   └── src/main/
│   │       ├── ets/
│   │       │   ├── entryability/EntryAbility.ets
│   │       │   ├── pages/Index.ets    # entry: catalog home <-> scene page
│   │       │   └── catalog/
│   │       │       ├── Scenes.ets     # scene table (ids must match glass_scene.h)
│   │       │       ├── CatalogHome.ets
│   │       │       ├── ScenePage.ets  # XComponent host + parameter menu
│   │       │       ├── ArkTsGlassContent.ets # ArkUI content exported to a GL texture
│   │       │       └── Specs.ets      # tab bar parameter table
│   │       ├── module.json5
│   │       └── resources/rawfile/photo.rgba
│   └── tools/make-photo-raw.py
├── build-and-run.sh                   # build / install / launch the demo
└── LICENSE
```

## Requirements

- DevEco Studio with the HarmonyOS SDK (compatible/target SDK `26.0.0`).
- `hdc` for device deployment.
- A connected HarmonyOS device or emulator.

## Build and run

### Demo application (needs signing)

In DevEco Studio:

1. Open the `demo/` directory.
2. `File → Project Structure… → Signing Configs`: enable automatic signing and sign in with a Huawei developer account.
3. Select the `entry` module and Run.

From the command line (from the repository root):

```bash
./build-and-run.sh              # package, install, launch
./build-and-run.sh --no-deploy  # package only
```

Note: device deployment requires a signed HAP; please configure signing in DevEco Studio first.
The demo project depends on library sources via `file:../../backdrop`, so building the demo compiles the library automatically without needing a separate HAR build.

### Library (no signing)

Open the repository root in DevEco Studio, or run `assembleHar`:

```bash
<DevEco>/tools/node/bin/node <DevEco>/tools/hvigor/bin/hvigorw.js \
  --no-daemon --mode module -p module=backdrop@default -p product=default assembleHar
```

The HAR is written to `backdrop/build/default/outputs/default/backdrop.har`.

## Parameters

Three in-app menus cover 83 numeric parameters and 8 toggles.

| Menu | Contents |
| --- | --- |
| Panel | Material (blur, refraction, tint), buttons, layout, animation |
| Indicator · rest | Rest refraction, highlight, shadow, layout, blur |
| Indicator · drag | Press refraction, highlight, shadow, velocity deformation, spring |

Parameter IDs (`number 0..82`, `bool 0..7`) must stay consistent between
`backdrop/src/main/cpp/glass/glass_params.{h,cpp}` and
`demo/entry/src/main/ets/catalog/Specs.ets`.

Scene IDs must stay consistent between `backdrop/src/main/cpp/glass/glass_scene.h`
and `demo/entry/src/main/ets/catalog/Scenes.ets`.

## Library API

| Function | Purpose |
| --- | --- |
| `GlassGLSetScene(scene)` | Switch display scene (0 = tab bar) |
| `GlassGLSetDensity(vp2px)` | Set display density for dp/vp layout in native code |
| `GlassGLSetNumber/SetBool(id, value)` | Set tab bar numeric/boolean configuration parameters |
| `GlassGLSetPhoto(buffer)` | Set custom background RGBA buffer (prefixed with 8-byte width/height metadata) |
| `GlassGLGetContentSurfaceId()` | Surface ID of the ArkTS content texture (`OH_NativeImage`) for `BuilderNode(RENDER_TYPE_TEXTURE)`; returns empty string before render thread is ready |
| `GlassGLSetPaused(paused)` | Pause/resume rendering: pass true when page is not visible to conserve power, false when visible |
| `GlassGLRegisterMenuCallback(cb)` | Register callback for native glass buttons requesting parameter menu |

## Implementation notes

- The renderer runs on its own thread and draws into the XComponent Surface through an EGL context.
- Frame pacing and idle scheduling: `eglSwapBuffers` does not reliably block on vsync for an XComponent surface across certain devices and window states. The loop therefore paces frames itself, aligned to `OH_NativeVSync` (`WaitForRenderSlot`): at most one frame per vsync period, rendered 0.4 period after vsync (clamped to 1.5–4 ms so the period's touch and content frames arrive first); every vsync while interacting (120 fps), otherwise spaced by the whole number of vsync periods closest to 16 ms (every other vsync on a 120 Hz panel, ~60 fps). Each swap sets a desired present timestamp (`SET_DESIRED_PRESENT_TIMESTAMP`) so RenderService takes only the newest frame instead of queueing an extra frame of latency. Without vsync it falls back to timer pacing (`PaceFrame`: 8 ms / 16 ms). It also skips drawing entirely when there is no input (touch, parameter updates, scene switches, resize, or texture upload) and accumulated spring displacement is below a threshold. The displacement accumulates across all spring steps (`gSpringMotion`) and is judged as a rate, independent of the loop step; when it stops, the tab springs snap to their targets only once they are already within tolerance, so transitions complete smoothly and the resting frame does not depend on frame timing.
- ArkTS content texture pipeline: the render thread creates an `OH_NativeImage` and provides its surface ID to ArkTS; a `BuilderNode(RENDER_TYPE_TEXTURE)` renders the ArkUI component tree into it. The render thread consumes the latest frame on each VSync (`OH_NativeImage_UpdateSurfaceImage`) and blits it into a standard 2D texture to meet `sampler2D` shader requirements. Any vertical inversion from external texture sampling is compensated using the `OH_NativeImage` transformation matrix.
- `photo.rgba` is a headerless RGBA buffer with an 8-byte prefix (`u32` LE width, `u32` LE height). It holds Kyant's demo wallpaper (`wallpaper_light.webp`); regenerate it from any image with `python3 demo/tools/make-photo-raw.py <image> [width]`.
- Source comments refer to the "reference implementation" (the Compose Multiplatform version of this effect); this repository is a standalone GLES reimplementation aligned with it.

## Power and frame rate

The rendering engine includes built-in on-demand drawing and dynamic frame pacing; the application layer only needs to synchronize visibility states with lifecycle changes:

| Scenario | Render Policy | Application Recommendation |
| --- | --- | --- |
| Idle (no touch, animation, or parameter updates) | Drawing paused, low-overhead idle checks | No action required |
| Touch interaction / spring animation | 120 fps for 400 ms following touch, 60 fps otherwise | No action required |
| Parameter / scene / photo update | Immediately renders a single frame | No action required |
| Page not visible (backgrounded, covered, navigated away) | Rendering paused | Call `GlassGLSetPaused(false/true)` in `onPageShow` / `onPageHide` |
| ArkTS content texture scene | Redraws driven by ArkUI frame production | Avoid permanent animation components (`LoadingProgress`, `TextClock`, etc.) unless necessary to prevent continuous redraws |

Performance monitoring: filter the `PERF_GL` log to inspect render activity. `fps/frames` indicates actual rendered frames, and `idle` indicates frames skipped by idle checks. Expected idle state is `frames=0, idle≈60`.

## License

Apache License 2.0. See [`LICENSE`](./LICENSE). Copyright 2025 Kyant (backdrop library), Copyright 2026 Liugt (HarmonyOS port).

Thanks to Kyant for the original Liquid Glass effect design.
