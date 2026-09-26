#include "glass_napi.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <arkui/native_node.h>
#include <arkui/native_node_napi.h>
#include <hilog/log.h>
#include <native_window/external_window.h>

#include <cstdint>
#include <cstring>
#include <mutex>

#include "gles_renderer.h"

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "GLASS"
#define LOG_DOMAIN 0x0000

#define GLOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)
#define GERR(...) OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)

namespace glass {

namespace {

constexpr uint32_t kMaxIdLen = 128;

ArkUI_NodeHandle g_node = nullptr;
OH_ArkUI_SurfaceHolder *g_holder = nullptr;
OH_ArkUI_SurfaceCallback *g_callback = nullptr;

void OnSurfaceCreated(OH_ArkUI_SurfaceHolder *holder) {
    auto *window = OH_ArkUI_XComponent_GetNativeWindow(holder);
    if (window == nullptr) {
        GERR("OnSurfaceCreated: native window is null");
        return;
    }
    // 尺寸在 OnSurfaceChanged 里给；这里先以 0 启动，渲染线程会等尺寸。
    GlesRenderer::Instance().OnSurfaceCreated(window, 0, 0);
}

void OnSurfaceChanged(OH_ArkUI_SurfaceHolder * /*holder*/, uint64_t width, uint64_t height) {
    GlesRenderer::Instance().OnSurfaceChanged(width, height);
}

void OnSurfaceDestroyed(OH_ArkUI_SurfaceHolder * /*holder*/) {
    GlesRenderer::Instance().OnSurfaceDestroyed();
}

void ReadIdArg(napi_env env, napi_value value, char *out, size_t outSize) {
    size_t len = 0;
    napi_get_value_string_utf8(env, value, out, outSize, &len);
}

}  // namespace

napi_value BindNode(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2 || args[0] == nullptr || args[1] == nullptr) {
        GERR("BindNode: need (id: string, node: FrameNode)");
        return nullptr;
    }

    char id[kMaxIdLen] = {0};
    ReadIdArg(env, args[0], id, sizeof(id));

    GLOG("BindNode step1: got args, id=%{public}s", id);

    ArkUI_NodeHandle handle = nullptr;
    int32_t ret = OH_ArkUI_GetNodeHandleFromNapiValue(env, args[1], &handle);
    GLOG("BindNode step2: getRet=%{public}d handle=%{public}llu", ret,
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)));

    if (ret != 0 || handle == nullptr) {
        GERR("BindNode: GetNodeHandleFromNapiValue failed, ret=%{public}d", ret);
        return nullptr;
    }

    // 重复绑定先清理
    UnbindNode(env, nullptr);
    GLOG("BindNode step3: cleared previous binding");

    g_node = handle;
    g_holder = OH_ArkUI_SurfaceHolder_Create(handle);
    GLOG("BindNode step4: holder=%{public}llu",
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_holder)));
    if (g_holder == nullptr) {
        GERR("BindNode: SurfaceHolder_Create failed");
        return nullptr;
    }
    OH_ArkUI_SurfaceHolder_SetUserData(g_holder, &GlesRenderer::Instance());

    g_callback = OH_ArkUI_SurfaceCallback_Create();
    OH_ArkUI_SurfaceCallback_SetSurfaceCreatedEvent(g_callback, OnSurfaceCreated);
    OH_ArkUI_SurfaceCallback_SetSurfaceChangedEvent(g_callback, OnSurfaceChanged);
    OH_ArkUI_SurfaceCallback_SetSurfaceDestroyedEvent(g_callback, OnSurfaceDestroyed);
    OH_ArkUI_SurfaceHolder_AddSurfaceCallback(g_holder, g_callback);

    // 允许跑到 120Hz（面板支持 120/90/72/60/45/40/36/30）。
    // 这只是给系统的提示：真正的帧率控制与空闲跳过在渲染线程里自己做
    // （实测 eglSwapBuffers 不保证按 vsync 阻塞，见 gles_renderer.cpp 的 PaceFrame）。
    OH_NativeXComponent_ExpectedRateRange range{0, 120, 120};
    int32_t rateRet = OH_ArkUI_XComponent_SetExpectedFrameRateRange(handle, range);

    GLOG("BindNode ok: id=%{public}s rateRet=%{public}d", id, rateRet);
    return nullptr;
}

napi_value UnbindNode(napi_env /*env*/, napi_callback_info /*info*/) {
    GlesRenderer::Instance().OnSurfaceDestroyed();
    if (g_holder != nullptr && g_callback != nullptr) {
        OH_ArkUI_SurfaceHolder_RemoveSurfaceCallback(g_holder, g_callback);
    }
    if (g_callback != nullptr) {
        OH_ArkUI_SurfaceCallback_Dispose(g_callback);
        g_callback = nullptr;
    }
    if (g_holder != nullptr) {
        OH_ArkUI_SurfaceHolder_Dispose(g_holder);
        g_holder = nullptr;
    }
    g_node = nullptr;
    GLOG("UnbindNode done");
    return nullptr;
}

napi_value SetNumber(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        return nullptr;
    }
    int32_t id = 0;
    double value = 0;
    napi_get_value_int32(env, args[0], &id);
    napi_get_value_double(env, args[1], &value);
    GlesRenderer::Instance().SetFloat(id, static_cast<float>(value));
    return nullptr;
}

napi_value SetBool(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        return nullptr;
    }
    int32_t id = 0;
    bool value = false;
    napi_get_value_int32(env, args[0], &id);
    napi_get_value_bool(env, args[1], &value);
    GlesRenderer::Instance().SetBool(id, value);
    return nullptr;
}

napi_value SetScene(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        return nullptr;
    }
    int32_t scene = 0;
    napi_get_value_int32(env, args[0], &scene);
    GlesRenderer::Instance().SetScene(scene);
    return nullptr;
}

napi_value SetDensity(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        return nullptr;
    }
    double density = 4.0;
    napi_get_value_double(env, args[0], &density);
    GlesRenderer::Instance().SetDensity(static_cast<float>(density));
    return nullptr;
}

napi_value SetDebugMode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        return nullptr;
    }
    int32_t mode = 0;
    napi_get_value_int32(env, args[0], &mode);
    GlesRenderer::Instance().SetDebugMode(mode);
    return nullptr;
}

napi_value SetPaused(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1 || args[0] == nullptr) {
        return nullptr;
    }
    bool paused = false;
    napi_get_value_bool(env, args[0], &paused);
    GlesRenderer::Instance().SetPaused(paused);
    GLOG("SetPaused: %{public}d", paused ? 1 : 0);
    return nullptr;
}

napi_value SetPhoto(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1 || args[0] == nullptr) {
        GERR("SetPhoto: need (data: ArrayBuffer)");
        return nullptr;
    }
    bool isBuffer = false;
    napi_is_arraybuffer(env, args[0], &isBuffer);
    if (!isBuffer) {
        GERR("SetPhoto: argument is not an ArrayBuffer");
        return nullptr;
    }
    void *data = nullptr;
    size_t length = 0;
    if (napi_get_arraybuffer_info(env, args[0], &data, &length) != napi_ok || data == nullptr) {
        GERR("SetPhoto: get_arraybuffer_info failed");
        return nullptr;
    }
    GlesRenderer::Instance().SetPhotoData(data, length);
    return nullptr;
}

napi_value GetContentSurfaceId(napi_env env, napi_callback_info /*info*/) {
    const std::string id = GlesRenderer::Instance().ContentSurfaceId();
    napi_value result = nullptr;
    napi_create_string_utf8(env, id.c_str(), id.size(), &result);
    return result;
}

napi_value IsRunning(napi_env env, napi_callback_info /*info*/) {
    napi_value result = nullptr;
    napi_get_boolean(env, GlesRenderer::Instance().IsRunning(), &result);
    return result;
}

// ---------------------------------------------------------------------------
// 顶部按钮点击 -> ArkTS 菜单（原生 -> JS 的反向调用）
//
// 渲染器只持有一个 `void (*)(int)`，由这里的 trampoline 用 threadsafe function
// 把请求排队到 JS 线程（JS 回调里再去开 ArkUI 菜单）。
// ---------------------------------------------------------------------------

namespace {

napi_threadsafe_function g_menuTsfn = nullptr;
std::mutex g_menuMutex;

/** 在 JS 线程上执行 ArkTS 注册进来的回调。 */
void MenuCallJs(napi_env env, napi_value jsCb, void * /*context*/, void *data) {
    if (env == nullptr || jsCb == nullptr) {
        return;
    }
    const int target = static_cast<int>(reinterpret_cast<intptr_t>(data));
    napi_value arg = nullptr;
    napi_create_int32(env, target, &arg);
    napi_value recv = nullptr;
    napi_get_undefined(env, &recv);
    napi_value result = nullptr;
    napi_call_function(env, recv, jsCb, 1, &arg, &result);
}

/** 渲染器（任意线程）调用：把"哪个按钮被点了"排到 JS 线程。 */
void OnMenuRequestFromRenderer(int target) {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    if (g_menuTsfn == nullptr) {
        return;
    }
    napi_call_threadsafe_function(g_menuTsfn,
                                  reinterpret_cast<void *>(static_cast<intptr_t>(target)),
                                  napi_tsfn_nonblocking);
}

}  // namespace

napi_value RegisterMenuCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1 || args[0] == nullptr) {
        GERR("RegisterMenuCallback: need (callback: (target: number) => void)");
        return nullptr;
    }
    napi_valuetype type = napi_undefined;
    napi_typeof(env, args[0], &type);
    if (type != napi_function) {
        GERR("RegisterMenuCallback: argument is not a function");
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_menuMutex);
    if (g_menuTsfn != nullptr) {   // 重复注册：先放掉旧的
        napi_release_threadsafe_function(g_menuTsfn, napi_tsfn_release);
        g_menuTsfn = nullptr;
    }
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "GlassGLMenuRequest", NAPI_AUTO_LENGTH, &resourceName);
    // 队列 4：按钮点击是稀疏事件，排不满；满了就丢（nonblocking），不会卡住触摸线程。
    napi_status status = napi_create_threadsafe_function(
        env, args[0], nullptr, resourceName, 4, 1, nullptr, nullptr, nullptr, MenuCallJs,
        &g_menuTsfn);
    if (status != napi_ok || g_menuTsfn == nullptr) {
        g_menuTsfn = nullptr;
        GERR("RegisterMenuCallback: create_threadsafe_function failed, status=%{public}d",
             static_cast<int>(status));
        return nullptr;
    }
    GlesRenderer::Instance().SetMenuRequestFn(&OnMenuRequestFromRenderer);
    GLOG("menu callback registered");
    return nullptr;
}

napi_value UnregisterMenuCallback(napi_env /*env*/, napi_callback_info /*info*/) {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    GlesRenderer::Instance().SetMenuRequestFn(nullptr);
    if (g_menuTsfn != nullptr) {
        napi_release_threadsafe_function(g_menuTsfn, napi_tsfn_release);
        g_menuTsfn = nullptr;
    }
    GLOG("menu callback unregistered");
    return nullptr;
}

// ---------------------------------------------------------------------------
// 经典 OH_NativeXComponent 路线
// ---------------------------------------------------------------------------

namespace {

OH_NativeXComponent_Callback g_xcCallback;

void XcOnSurfaceCreated(OH_NativeXComponent *component, void *window) {
    uint64_t w = 0;
    uint64_t h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    GLOG("XcOnSurfaceCreated: window=%{public}llu size=%{public}llux%{public}llu",
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(window)),
         static_cast<unsigned long long>(w), static_cast<unsigned long long>(h));
    if (window == nullptr) {
        GERR("XcOnSurfaceCreated: window is null");
        return;
    }
    GlesRenderer::Instance().OnSurfaceCreated(window, w, h);
}

void XcOnSurfaceChanged(OH_NativeXComponent *component, void *window) {
    uint64_t w = 0;
    uint64_t h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    GLOG("XcOnSurfaceChanged: %{public}llux%{public}llu",
         static_cast<unsigned long long>(w), static_cast<unsigned long long>(h));
    GlesRenderer::Instance().OnSurfaceChanged(w, h);
}

void XcOnSurfaceDestroyed(OH_NativeXComponent * /*component*/, void * /*window*/) {
    GLOG("XcOnSurfaceDestroyed");
    GlesRenderer::Instance().OnSurfaceDestroyed();
}

/**
 * 触摸：直接交给渲染器按当前场景分发（页签拖动 / 按钮按压 / 滑块拖动…）。
 *
 * 注：旧版这里有一个"点屏幕中上部循环切消融模式"的调试手势；目录场景里
 * 它是误触源（点按钮会顺带切调试模式），已移除。需要消融对比时用
 * `GlassGLSetDebugMode` 显式设置。
 */
void XcDispatchTouchEvent(OH_NativeXComponent *component, void *window) {
    OH_NativeXComponent_TouchEvent touchEvent;
    if (OH_NativeXComponent_GetTouchEvent(component, window, &touchEvent) != 0) {
        return;
    }
    // OH_NativeXComponent_TouchEventType: DOWN=0, UP=1, MOVE=2, CANCEL=3
    const int action = static_cast<int>(touchEvent.type);
    GlesRenderer::Instance().OnTouch(action, touchEvent.x, touchEvent.y, 0);
}

}  // namespace

void RegisterNativeXComponent(napi_env env, napi_value exports) {
    napi_value xcObj = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xcObj) != napi_ok ||
        xcObj == nullptr) {
        return;  // 这次 Init 与 XComponent 无关
    }
    OH_NativeXComponent *component = nullptr;
    if (napi_unwrap(env, xcObj, reinterpret_cast<void **>(&component)) != napi_ok ||
        component == nullptr) {
        GERR("RegisterNativeXComponent: unwrap failed");
        return;
    }
    char id[kMaxIdLen] = {0};
    uint64_t idLen = sizeof(id);
    OH_NativeXComponent_GetXComponentId(component, id, &idLen);

    // 只接管 GL 页自己的 XComponent：其它同样以 `libraryname: 'backdrop'` 注册的
    // XComponent 也会走到本模块的 Init，若不按 id 排除，渲染器会接管错误的 surface。
    if (strcmp(id, "glassSurface") != 0) {
        GLOG("RegisterNativeXComponent: skip non-GL surface (id=%{public}s)", id);
        return;
    }

    g_xcCallback.OnSurfaceCreated = XcOnSurfaceCreated;
    g_xcCallback.OnSurfaceChanged = XcOnSurfaceChanged;
    g_xcCallback.OnSurfaceDestroyed = XcOnSurfaceDestroyed;
    g_xcCallback.DispatchTouchEvent = XcDispatchTouchEvent;
    int32_t ret = OH_NativeXComponent_RegisterCallback(component, &g_xcCallback);
    GLOG("RegisterNativeXComponent: id=%{public}s ret=%{public}d", id, ret);
}

}  // namespace glass
