#ifndef GLASS_SCENE_H
#define GLASS_SCENE_H

namespace glass {

/**
 * 目录（catalog）场景编号。**必须与 ArkTS 侧 `catalog/Scenes.ets` 的
 * `SceneId` 一一对应**；`GlassGLSetScene(id)` 直接以本枚举落座。
 *
 * 设计：一个 XComponent 只画一个场景；页面切换时 ArkTS 先 `SetScene` 再挂载。
 * 0 保持为页签场景，兼容旧入口（默认值）。
 */
enum class Scene : int {
    kTabs = 0,             // 玻璃页签条（当前 demo 的保真对照面）
    kButtons,              // 液态玻璃按钮（Buttons）
    kToggle,               // 液态玻璃开关（Toggle）
    kSlider,               // 液态玻璃滑块（Slider）
    kDialog,               // 玻璃对话框（Dialog）
    kControlCenter,        // 控制中心（Control center）
    kLockScreen,           // 锁屏（Lock screen）
    kMagnifier,            // 放大镜（Magnifier）
    kProgressiveBlur,      // 渐进模糊（Progressive blur）
    kAdaptiveLuminance,    // 自适应亮度玻璃（Adaptive luminance glass）
    kScrollContainer,      // 滚动容器（Scroll container）
    kLazyScrollContainer,  // 懒加载滚动容器（Lazy scroll container）
    kPlayground,           // 玻璃游乐场（Glass playground）
    kArkTsGlass,           // ArkTS 实时内容玻璃（BuilderNode 纹理导出 -> GL 采样）
    kCount
};

}  // namespace glass

#endif  // GLASS_SCENE_H
