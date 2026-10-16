// ---------------------------------------------------------------------------
// backdrop：鸿蒙液态玻璃库的 ArkTS 出口。
//
// 真正的实现在 src/main/cpp（XComponent + EGL/GLES 自渲染 + native_drawing 文字），
// 这里把原生 NAPI 模块 `libbackdrop.so` 原样转出；使用者按名导入即可。
// ---------------------------------------------------------------------------

export {
  GlassGLBindNode,
  GlassGLUnbindNode,
  GlassGLSetNumber,
  GlassGLSetBool,
  GlassGLSetDebugMode,
  GlassGLSetPaused,
  GlassGLSetDarkMode,
  GlassGLSetPhoto,
  GlassGLGetContentSurfaceId,
  GlassGLIsRunning,
  GlassGLRegisterMenuCallback,
  GlassGLUnregisterMenuCallback,
  GlassGLSetScene,
  GlassGLSetDensity
} from 'libbackdrop.so';
