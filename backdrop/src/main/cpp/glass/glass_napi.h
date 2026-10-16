#ifndef GLASS_NAPI_H
#define GLASS_NAPI_H

// 目录场景：一个 XComponent 画一个场景。
// 命名前缀与其它接口一致（GlassGL*），ArkTS 侧见 backdrop/Index.ets。

#include "napi/native_api.h"

namespace glass {

/** 把 XComponent 的 FrameNode 交给原生渲染器（创建 SurfaceHolder 并绑定生命周期）。 */
napi_value BindNode(napi_env env, napi_callback_info info);

/** 解绑（页面销毁时调用）。 */
napi_value UnbindNode(napi_env env, napi_callback_info info);

/** 调参：id 与参考实现参数编号保持一致。 */
napi_value SetNumber(napi_env env, napi_callback_info info);
napi_value SetBool(napi_env env, napi_callback_info info);

/** 目录场景：切场景 / 上报显示密度。 */
napi_value SetScene(napi_env env, napi_callback_info info);
napi_value SetDensity(napi_env env, napi_callback_info info);

/** 消融/调试模式。 */
napi_value SetDebugMode(napi_env env, napi_callback_info info);

/** 暂停/恢复绘制（页面不可见时调用，省电）。 */
napi_value SetPaused(napi_env env, napi_callback_info info);

/** 设置系统深浅色模式（true=深色模式，false=浅色模式）。 */
napi_value SetDarkMode(napi_env env, napi_callback_info info);

/** 传照片背景的裸 RGBA（前 8 字节 = u32 LE 宽、高）。 */
napi_value SetPhoto(napi_env env, napi_callback_info info);

/**
 * ArkTS 内容纹理的 surfaceId（kArkTsGlass 场景）。
 *
 * 用法：ArkTS 轮询直到返回非空串，然后 `new BuilderNode(uiContext, { type:
 * RENDER_TYPE_TEXTURE, surfaceId })`。返回空串 = 渲染线程还没把 OH_NativeImage 建好。
 */
napi_value GetContentSurfaceId(napi_env env, napi_callback_info info);

/** 获取渲染线程运行状态。 */
napi_value IsRunning(napi_env env, napi_callback_info info);

/**
 * 注册/注销"GL 里画的顶部按钮被点击"的回调（ArkTS：`GlassGLRegisterMenuCallback`）。
 *
 * `target`：0=底座，1=指示器·静止，2=指示器·拖动。
 * 触摸发生在 XComponent 上（UI 线程），但回调必须回到 JS 线程执行，所以内部用
 * `napi_threadsafe_function` 转发。
 */
napi_value RegisterMenuCallback(napi_env env, napi_callback_info info);
napi_value UnregisterMenuCallback(napi_env env, napi_callback_info info);

/**
 * 在模块 `Init` 里调用：如果 exports 里带了 XComponent 对象
 * （ArkTS 侧写了 `XComponent({ id, libraryname: 'backdrop' })`），就注册经典
 * `OH_NativeXComponent` 回调，由 ArkUI 直接驱动 Surface 生命周期。
 *
 * 这条路不依赖 `getAttachedFrameNodeById` → `OH_ArkUI_GetNodeHandleFromNapiValue`
 * 的映射（实测那条路拿到的 handle 无效：SurfaceHolder_Create 返回 null）。
 */
void RegisterNativeXComponent(napi_env env, napi_value exports);

}  // namespace glass

#endif  // GLASS_NAPI_H
