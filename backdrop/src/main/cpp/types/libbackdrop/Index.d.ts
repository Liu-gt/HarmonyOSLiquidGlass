// ---------------------------------------------------------------------------
// GLES 原生自渲染接口
// ---------------------------------------------------------------------------

/** 将 XComponent 的 FrameNode 传递给原生渲染器，建立 SurfaceHolder 绑定。 */
export const GlassGLBindNode: (id: string, node: FrameNode) => void

/** 解绑并停止渲染线程。 */
export const GlassGLUnbindNode: (id: string) => void

/** 设置数值调节参数。id 为参数编号，详见 demo/entry/src/main/ets/catalog/Specs.ets。 */
export const GlassGLSetNumber: (id: number, value: number) => void

/** 设置布尔开关参数。id 为开关编号，详见 demo/entry/src/main/ets/catalog/Specs.ets。 */
export const GlassGLSetBool: (id: number, value: boolean) => void

/**
 * 切换展示场景（详见 demo/entry/src/main/ets/catalog/Scenes.ets 的 SceneId）。
 * 0 = 玻璃页签条（默认），其余为组件展示场景。
 */
export const GlassGLSetScene: (scene: number) => void

/** 设置屏幕像素密度（vp -> px），供原生层进行 dp/vp 布局计算。缺省为 4。 */
export const GlassGLSetDensity: (density: number) => void

/** 调试/呈现模式：0=正常，1=仅背景，2=仅折射，3=无玻璃。 */
export const GlassGLSetDebugMode: (mode: number) => void

/**
 * 暂停或恢复渲染。页面不可见（后台挂起、遮挡覆盖、路由切出）时传入 true：
 * 渲染线程暂停重绘并停止物理动画步进（Surface 保留当前帧），恢复可见时传入 false。
 */
export const GlassGLSetPaused: (paused: boolean) => void

/** 同步系统深浅色模式（true = 深色模式，false = 浅色模式）。 */
export const GlassGLSetDarkMode: (dark: boolean) => void

/** 传入自定义背景 RGBA 数据（前 8 字节为 u32 LE 宽、高元数据）。 */
export const GlassGLSetPhoto: (data: ArrayBuffer) => void

/**
 * 获取 ArkTS 内容纹理的 Surface ID（供 ArkTS 内容实时透射场景使用）。
 *
 * 将获取到的 Surface ID 传入 `BuilderNode(uiContext, { type: NodeRenderType.RENDER_TYPE_TEXTURE, surfaceId })`，
 * ArkUI 即可将组件树渲染至 GL 外部纹理（GPU 内部流转，无 CPU 回读开销）。
 * 若渲染线程尚未就绪，则返回空字符串。
 */
export const GlassGLGetContentSurfaceId: () => string

/** 获取渲染线程运行状态。 */
export const GlassGLIsRunning: () => boolean

/**
 * 注册原生层玻璃交互按钮点击回调。
 * target: 0=底座材质，1=指示器·静止，2=指示器·拖动。
 * 建议在组件销毁前调用 `GlassGLUnregisterMenuCallback` 及时注销。
 */
export const GlassGLRegisterMenuCallback: (callback: (target: number) => void) => void

/** 注销原生层玻璃交互按钮点击回调。 */
export const GlassGLUnregisterMenuCallback: () => void
