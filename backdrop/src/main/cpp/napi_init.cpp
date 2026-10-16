#include "napi/native_api.h"
#include "hilog/log.h"
#include "glass/glass_napi.h"

// 避免工程侧未定义 LOG_DOMAIN 时编译失败
#ifndef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#endif

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        // ---- GLES 自渲染（pages/GlassPage.ets 用）----
        {"GlassGLBindNode", nullptr, glass::BindNode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLUnbindNode", nullptr, glass::UnbindNode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLSetNumber", nullptr, glass::SetNumber, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLSetBool", nullptr, glass::SetBool, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ---- 目录场景（pages/catalog/*）----
        {"GlassGLSetScene", nullptr, glass::SetScene, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLSetDensity", nullptr, glass::SetDensity, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLSetDebugMode", nullptr, glass::SetDebugMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLSetPaused", nullptr, glass::SetPaused, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLSetDarkMode", nullptr, glass::SetDarkMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLIsRunning", nullptr, glass::IsRunning, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLSetPhoto", nullptr, glass::SetPhoto, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLGetContentSurfaceId", nullptr, glass::GetContentSurfaceId, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLRegisterMenuCallback", nullptr, glass::RegisterMenuCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"GlassGLUnregisterMenuCallback", nullptr, glass::UnregisterMenuCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    // 若本次 Init 是 XComponent({libraryname:'backdrop'}) 触发的，注册经典 Surface 生命周期回调
    glass::RegisterNativeXComponent(env, exports);
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "backdrop",
    .nm_priv = ((void*)0),
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterBackdropModule(void)
{
    napi_module_register(&demoModule);
}
