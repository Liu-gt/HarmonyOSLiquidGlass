#ifndef GLASS_GLES_RENDERER_H
#define GLASS_GLES_RENDERER_H

#include <EGL/egl.h>
#include <EGL/eglplatform.h>
#include <GLES3/gl3.h>
#include <native_image/native_image.h>
#include <native_vsync/native_vsync.h>

#include <atomic>
#include <condition_variable>

#include "glass_params.h"
#include "glass_scene.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>

namespace glass {

/**
 * 极简 GLES 渲染器（Milestone 1）。
 *
 * 结构：
 *   XComponent surface -> EGL(window surface) -> [pass1: 背景进 FBO] -> [pass2: 折射/色散合成到屏幕]
 *
 * 设计要点：
 * - 渲染跑在**独立线程**（`eglSwapBuffers` 天然被 buffer queue 按 vsync 节流），
 *   不依赖 ArkUI 的帧回调语义。
 * - 所有 GL 对象只能在该线程创建/销毁；surface 生命周期回调（UI 线程）只做
 *   "置标志 + 传窗口指针 + join"。
 */
class GlesRenderer {
public:
    static GlesRenderer &Instance();

    /** UI 线程：surface 就绪，传入 OHNativeWindow*（以 void* 传递，避免头文件外泄）。 */
    void OnSurfaceCreated(void *nativeWindow, uint64_t width, uint64_t height);

    /** UI 线程：尺寸变化。 */
    void OnSurfaceChanged(uint64_t width, uint64_t height);

    /** UI 线程：surface 销毁，阻塞直到渲染线程退出。 */
    void OnSurfaceDestroyed();

    /** 是否正在渲染（给 napi 查询用）。 */
    bool IsRunning() const { return running_.load(); }

    void SetFloat(int id, float value);
    void SetBool(int id, bool value);

    /** 切目录场景（`glass_scene.h` 的 Scene 枚举）。切场景会重置该场景的动画状态。 */
    void SetScene(int scene);
    int Scene() const { return scene_.load(); }

    /** 显示密度（vp -> px）。ArkTS 侧在页面上报，缺省 4。 */
    void SetDensity(float density) {
        params_.SetDensity(density);
        labelsDirty_.store(true);
        contentTextDirty_.store(true);
        overlayTextDirty_.store(true);
        buttonLabelsDirty_.store(true);
        dialogBtnLabelDirty_.store(true);
        forceRedraw_.store(true);
    }

    /** 深浅色模式同步（深色模式下弹窗等组件自动切换暗色通透玻璃与浅色文字）。 */
    void SetDarkMode(bool dark);
    bool IsDarkMode() const { return darkMode_.load(); }

    /** 消融/调试：0=正常 1=只画背景 2=只画折射 3=不画玻璃 */
    void SetDebugMode(int mode);

    /**
     * 上层暂停/恢复绘制（功耗开关）。页面不可见（被覆盖、进后台、切到别的 tab）时调用，
     * 渲染线程会停止出帧、也不再步进动画；surface 保留最后一帧。
     *
     * 仍会继续消费 ArkTS 内容纹理的帧，避免把 ArkUI 的生产端堵住。恢复后会自动补画一帧。
     */
    void SetPaused(bool paused);

    /**
     * 触摸（屏幕坐标，y 从**顶部**起算）。由 XComponent 的 DispatchTouchEvent 调用。
     * 只关心底部的玻璃条区域；在条上按下即可拖动指示器。
     */
    void OnTouch(int action, float x, float y, int64_t timestampMs);

    /** 照片背景的裸 RGBA 数据（前 8 字节 = u32 LE 宽、u32 LE 高）。由 ArkTS 读 rawfile 后传入。 */
    void SetPhotoData(const void *data, size_t length);

    /**
     * ArkTS 内容纹理的 surfaceId（`kArkTsGlass` 场景用）。
     *
     * 渲染线程启动时创建 `OH_NativeImage`（消费端），ArkTS 侧把这个 surfaceId 交给
     * `BuilderNode(RENDER_TYPE_TEXTURE)`，ArkUI 就会把组件树渲染进这张纹理；
     * 渲染线程每帧 `OH_NativeImage_UpdateSurfaceImage` 取最新帧（GPU 直达，不过 CPU）。
     * 返回空串表示渲染线程还没起来，调用方需要稍后重试。
     */
    std::string ContentSurfaceId() const;

    /**
     * 顶部三个玻璃按钮被点击时回调（target：0=底座，1=指示器·静止，2=指示器·拖动）。
     *
     * 之所以要这条回链：菜单本身是 ArkUI 画的，而按钮画在 GL 里；原生只能把
     * "哪个按钮被点了"告诉 ArkTS。实现见 `glass_napi.cpp`（napi threadsafe function）。
     */
    using MenuRequestFn = void (*)(int target);
    void SetMenuRequestFn(MenuRequestFn fn) { menuRequestFn_.store(fn); }

private:
    GlesRenderer() = default;
    ~GlesRenderer() = default;
    GlesRenderer(const GlesRenderer &) = delete;
    GlesRenderer &operator=(const GlesRenderer &) = delete;

    // ---- 渲染线程内部 ----
    void RenderLoop(void *window);
    bool InitEgl(void *window);
    bool InitGl();
    void DestroyGl();
    void DestroyEgl();
    bool EnsureTargets(int w, int h);
    void DrawFrame(float seconds);
    void LogFps();

    // -------------------------------------------------------------------------
    // 目录场景（catalog）—— 一个 XComponent 画一个场景，见 glass_scene.h
    // -------------------------------------------------------------------------

    /** 通用玻璃件描述。`DrawGlass` 把这里的字段映射到 progGlass_ 的 uniform。 */
    struct GlassSpec {
        float x = 0.0f;          // 左下角（GL 坐标，y 从屏幕底部起算）
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        float rTL = 0.0f;
        float rTR = 0.0f;
        float rBR = 0.0f;
        float rBL = 0.0f;
        float scaleX = 1.0f;     // 形状缩放（绕中心；按压变形用）
        float scaleY = 1.0f;
        float refractionHeight = 0.0f;
        float refractionAmount = 0.0f;
        bool dispersion = false;
        bool depthEffect = false;
        float surfaceAlpha = 0.0f;
        float surfaceR = 1.0f;
        float surfaceG = 1.0f;
        float surfaceB = 1.0f;
        float vibrancy = 1.0f;
        float blurRadiusPx = 0.0f;
        float brightness = 0.0f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float effectOpacity = 1.0f;
        float highlight = 0.0f;          // 描边环亮度
        bool plainHighlight = false;
        bool ambient = false;
        float ambientIntensity = 0.38f;  // 环境光强度（开关旋钮按按压进度缩放）
        float highlightWidthPx = 2.0f;
        float highlightBlurPx = 1.0f;
        bool hasShadow = true;
        float shadowBlurPx = 0.0f;
        float shadowOffsetPx = 0.0f;
        float shadowAlpha = 0.0f;
        float innerRadiusPx = 0.0f;
        float innerAlpha = 0.0f;
        float g2Factor = 0.45f;          // G2 连续曲率平滑系数（0 = 标准圆弧，0.45 = 默认 G2）
        float rimClarity = 0.38f;        // 边缘清晰折射采样混入比例（0..1）
        float bevelAlphaScale = 0.68f;   // 边缘折射棱面处表面底色保留比例（0..1）
        // 采样变换（放大镜用）：samplePx = pivot + (px - pivot) / scale
        float sampleScaleX = 1.0f;
        float sampleScaleY = 1.0f;
        float samplePivotX = 0.0f;
        float samplePivotY = 0.0f;
        // 采样源用**背景**而不是 sceneTex_。用于"要画进 sceneTex_ 的底层玻璃"：
        // 目标纹理 == 采样纹理会构成 GL 的读写反馈（未定义行为），换成 bgTex_ 即可避免。
        bool useBackground = false;
    };

    /** 纯色圆角矩形描述（轨道 / 填充 / 卡片 / 按钮底色）。 */
    struct SolidSpec {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        float rTL = 0.0f;
        float rTR = 0.0f;
        float rBR = 0.0f;
        float rBL = 0.0f;
        float r = 0.0f;          // 四值相等时的快捷写法（覆盖上面的独立圆角）
        float a = 1.0f;
        float colorR = 1.0f;
        float colorG = 1.0f;
        float colorB = 1.0f;
    };

    /**
     * 画一个玻璃件：采样 sceneTex_（清晰）与该半径的模糊结果。
     *
     * targetFbo = 0 时画到屏幕（正常）；非 0 时画进 sceneTex_（游乐场的叠层，
     * 让上层玻璃能采样到它）。半径相同的连续调用会复用同一张模糊结果（帧内缓存）。
     */
    void DrawGlass(const GlassSpec &spec, GLuint targetFbo, int w, int h);
    /** 画一个纯色圆角矩形（调用方负责 framebuffer 与混合状态）。 */
    void DrawSolid(const SolidSpec &spec, int w, int h);
    /** 贴一张位图到屏幕矩形（带混合；调用方负责 framebuffer）。alpha 做整体淡入淡出。 */
    void DrawSprite(GLuint tex, float x, float y, float w, float h, int screenW, int screenH,
                    float alpha = 1.0f);
    /** 背景 -> sceneTex_ 的拷贝。 */
    void CopyBackgroundToScene(int w, int h);
    /** 把一张纹理原样铺满屏幕（progCopy_，不透明）。 */
    void BlitTexture(GLuint tex, int w, int h);
    /** pass 1：背景（照片）-> bgTex_。 */
    void RenderBackground(int w, int h);
    /** 玻璃件默认材质（对齐上游 Shadow.Default + Highlight.Default）。 */
    GlassSpec DefaultGlass(float x, float y, float w, float h, float radius, float d) const;
    /** 开关 / 滑块旋钮**共用**的材质（胶囊形，半径 = 高 / 2）。 */
    GlassSpec KnobGlass(float x, float y, float w, float h, float d, float press) const;
    /** 同一采样源 + 半径的模糊结果帧内缓存。 */
    GLuint BlurFor(GLuint sourceTex, float radiusPx, int w, int h);

    // 各场景的绘制入口（DrawTabsScene 从 pass 2 开始；pass 1 在 DrawFrame 里统一画）
    void DrawTabsScene(int w, int h, int mode);
    void DrawCatalogScene(int w, int h);
    void DrawButtonsScene(int w, int h);
    void DrawToggleScene(int w, int h);
    void DrawSliderScene(int w, int h);
    void DrawDialogScene(int w, int h);
    void DrawControlCenterScene(int w, int h);
    void DrawLockScreenScene(int w, int h);
    void DrawMagnifierScene(int w, int h);
    void DrawProgressiveBlurScene(int w, int h);
    void DrawAdaptiveScene(int w, int h);
    void DrawScrollScene(int w, int h, bool lazy);
    void DrawPlaygroundScene(int w, int h);
    void DrawArkTsGlassScene(int w, int h);

    // ---- 目录场景的文字层 ----
    // 文字分两层：content 层画进 sceneTex_（会被上层玻璃折射，如锁屏钟表），
    // overlay 层画在玻璃之上（如按钮文字、对话框正文）。两层各一张位图，按需重传。
    GLuint contentTextTex_ = 0;
    GLuint overlayTextTex_ = 0;
    int contentTextW_ = 0;
    int contentTextH_ = 0;
    int overlayTextW_ = 0;
    int overlayTextH_ = 0;
    // 位图对应的屏幕矩形（GL 坐标）；文字位图不必覆盖全屏（如自适应亮度的数值）。
    float contentTextX_ = 0.0f;
    float contentTextY_ = 0.0f;
    float overlayTextX_ = 0.0f;
    float overlayTextY_ = 0.0f;
    std::atomic<bool> contentTextDirty_{true};
    std::atomic<bool> overlayTextDirty_{true};
    bool UploadContentText(int w, int h);
    bool UploadOverlayText(int w, int h);
    /** 构造文字层内容（返回 false 表示本场景不需要该层）。 */
    bool BuildContentText(int scene, int w, int h, std::vector<struct TextRun> &runs);
    bool BuildOverlayText(int scene, int w, int h, std::vector<struct TextRun> &runs);
    void DrawContentText(int w, int h);
    void DrawOverlayText(int w, int h);

    // 按钮场景的独立文字纹理（各按钮随按压缩放同步缩放）
    GLuint buttonLabelTex_[4] = {0, 0, 0, 0};
    int buttonLabelW_[4] = {0, 0, 0, 0};
    int buttonLabelH_[4] = {0, 0, 0, 0};
    std::atomic<bool> buttonLabelsDirty_{true};
    bool UploadButtonLabels(int w, int h);

    // 对话框按钮的独立文字纹理（随按钮按压缩放同步缩放）
    GLuint dialogBtnLabelTex_ = 0;
    int dialogBtnLabelW_ = 0;
    int dialogBtnLabelH_ = 0;
    std::atomic<bool> dialogBtnLabelDirty_{true};
    std::atomic<bool> darkMode_{false};
    bool UploadDialogBtnLabel(int w, int h);

    /** 控制中心 / 游乐场的飞机图标（一次性栅格化）。 */
    GLuint planeIconTex_ = 0;
    int planeIconSizePx_ = 0;

    std::atomic<int> scene_{static_cast<int>(Scene::kTabs)};

    // ---- 目录场景的动画状态（各场景独立；切场景时重置） ----
    static constexpr int kScenePressMax = 8;
    float scenePress_[kScenePressMax] = {0.0f};
    float scenePressVel_[kScenePressMax] = {0.0f};
    int sceneDown_ = -1;

    // 开关
    float toggleFraction_ = 0.0f;
    float toggleFractionVel_ = 0.0f;
    float togglePress_ = 0.0f;
    float togglePressVel_ = 0.0f;
    bool toggleOn_ = false;
    bool toggleDragging_ = false;
    float toggleDownX_ = 0.0f;              // 按下时的屏幕 x（拖动按位移换算）
    float toggleGrabStartFraction_ = 0.0f;  // 按下时的 fraction
    float toggleDragTarget_ = 0.0f;         // 拖动时手指映射出的目标 fraction（fraction 弹簧追随它）
    bool toggleDidDrag_ = false;
    float toggleScaleX_ = 1.0f;
    float toggleScaleXVel_ = 0.0f;
    float toggleScaleY_ = 1.0f;
    float toggleScaleYVel_ = 0.0f;
    // 拖动速度（fraction/s，弹簧平滑）与上一帧 fraction：按压拖动时拉长/压扁形变用
    float toggleVelocity_ = 0.0f;
    float toggleVelocityVel_ = 0.0f;
    float toggleFractionLast_ = 0.0f;

    // 滑块
    float sliderValue_ = 50.0f;      // 0..100（手指给的目标值）
    // 旋钮的动画值：弹簧追 sliderValue_ —— 拖动时旋钮**滞后**手指一点点，
    // 松手/停手时再弹回去（q 弹的来源）。轨道填充仍用目标值，读数不拖泥带水。
    float sliderValueAnim_ = 50.0f;
    float sliderValueAnimVel_ = 0.0f;
    float sliderValueAnimLast_ = 50.0f;
    // 弹簧平滑后的动画值速度（%/s）：驱动拖动时的拉长/压扁形变
    float sliderVelocity_ = 0.0f;
    float sliderVelocityVel_ = 0.0f;
    float sliderPress_ = 0.0f;
    float sliderPressVel_ = 0.0f;
    bool sliderDragging_ = false;
    float sliderGrabValue_ = 0.0f;
    float sliderGrabX_ = 0.0f;
    bool sliderDidDrag_ = false;
    float sliderScaleX_ = 1.0f;
    float sliderScaleXVel_ = 0.0f;
    float sliderScaleY_ = 1.0f;
    float sliderScaleYVel_ = 0.0f;

    // 对话框按钮按压与弹簧缩放
    float dialogPress_[2] = {0.0f, 0.0f};
    float dialogPressVel_[2] = {0.0f, 0.0f};
    int dialogDown_ = -1;
    float dialogScale_ = 1.0f;
    float dialogScaleVel_ = 0.0f;

    // 控制中心：展开进度（1 = 展开）
    float ccProgress_ = 1.0f;
    float ccProgressVel_ = 0.0f;
    bool ccDragging_ = false;
    float ccDragStartProgress_ = 0.0f;
    float ccDragStartY_ = 0.0f;

    // 锁屏：钟表拖动
    float lockOffsetX_ = 0.0f;
    float lockOffsetY_ = 0.0f;
    bool lockDragging_ = false;
    float lockGrabDX_ = 0.0f;
    float lockGrabDY_ = 0.0f;

    // 放大镜：光标位置（GL 坐标）
    float magX_ = 0.0f;
    float magY_ = 0.0f;
    bool magDragging_ = false;
    float magGrabDX_ = 0.0f;
    float magGrabDY_ = 0.0f;
    bool magInited_ = false;

    // 滚动容器：列表位移（px，0 = 顶部）
    float listScroll_ = 0.0f;
    float listScrollVel_ = 0.0f;
    bool listDragging_ = false;
    float listGrabScroll_ = 0.0f;
    float listGrabY_ = 0.0f;

    // 游乐场：玻璃板变换 + 参数
    float pgX_ = 0.0f;
    float pgY_ = 0.0f;
    bool pgInited_ = false;
    bool pgDragging_ = false;
    float pgGrabDX_ = 0.0f;
    float pgGrabDY_ = 0.0f;
    bool pgSheetOpen_ = true;
    int pgSliderDown_ = -1;          // 0..4
    // 五个滑块归一化值：0 圆角 / 1 模糊 / 2 折射高 / 3 折射强 / 4 色散
    float pgSliderValue_[5] = {0.5f, 0.0f, 0.2f, 0.2f, 0.0f};
    float pgSliderGrabValue_[5] = {0.5f, 0.0f, 0.2f, 0.2f, 0.0f};
    float pgSliderGrabX_[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float pgSliderPress_[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float pgSliderPressVel_[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    int pgButtonDown_ = -1;          // 0 = 折叠开关，1 = 重置
    float pgButtonPress_[2] = {0.0f, 0.0f};
    float pgButtonPressVel_[2] = {0.0f, 0.0f};

    // 自适应亮度：亮度弹簧 + 采样节流
    float lumValue_ = 0.25f;
    float lumTarget_ = 0.25f;
    float lumVel_ = 0.0f;
    int64_t lumSampleMs_ = 0;
    int lumShown_ = -1;          // 已显示到文字里的百分数（避免每帧重传文字位图）
    int64_t lumTextMs_ = 0;

    /** 各场景一帧内共用的弹簧步进（与参考实现的 spring 同构）。 */
    void UpdateCatalogAnim(float dt);

    /** 触摸分发（各场景自己的命中测试与拖动）。返回 true 表示事件已消费。 */
    bool OnTouchCatalog(int action, float x, float y, int surfaceW, int surfaceH);

    /** 场景切换时的动画/文本重置。 */
    void ResetCatalogState(int scene);
    /** 帧内模糊缓存（采样源 + 半径）。 */
    float blurCacheRadius_ = -1.0f;
    GLuint blurCacheTex_ = 0;
    GLuint blurCacheSrc_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> width_{0};
    std::atomic<uint64_t> height_{0};

    // -------------------------------------------------------------------------
    // 帧率节流 + 空闲跳过（功耗）
    //
    // 背景：`eglSwapBuffers` 在 XComponent 的 surface 上**不保证**按 vsync 阻塞
    // （实测有 580fps 的机型/窗口状态），所以循环必须自己限速；而且"没变化就不画"
    // 比限速更省 —— 静止时整条链路（背景 / 模糊 / 玻璃 / 指示器）一次都不跑。
    // -------------------------------------------------------------------------

    /** 上层暂停标志（页面不可见时置位）。 */
    std::atomic<bool> paused_{false};
    /** 有输入/参数/场景/尺寸变化 → 下一帧必须画（消费后清零）。 */
    std::atomic<bool> forceRedraw_{true};
    /** 最近一次触摸时刻（交互窗口内用高帧率）。 */
    std::atomic<int64_t> lastTouchMs_{0};
    /** 上一帧绘制时刻（帧率节流基准）。 */
    int64_t lastDrawMs_ = 0;
    /** 空闲跳过计数（只用于日志）。 */
    int skippedFrames_ = 0;

    /** 睡到本帧的时间点（没有 vsync 连接时的退回节拍）。 */
    void PaceFrame();

    // ---- vsync 对齐出帧（见 WaitForRenderSlot）----
    /** 渲染线程：建 vsync 连接（失败则退回 PaceFrame 的时间节拍）。 */
    void CreateVsync();
    void DestroyVsync();
    /** 预约下一个 vsync 回调（一次性，每次出帧后重新预约）。 */
    void RequestVsync();
    static void OnVsyncCallback(long long timestamp, void *data);
    /**
     * 等到本帧的出帧点：每个 vsync 周期最多一帧，出帧点在 vsync + δ；fast = false 时帧间隔取
     * 最接近 16ms 的 vsync 整数倍（沿用非交互 ~60fps 的档位）。没有 vsync 连接时退回 PaceFrame。
     * 返回 true = 现在就画；false = 等过了，回循环顶部重新消费输入后再判定。
     */
    bool WaitForRenderSlot(bool fast);
    OH_NativeVSync *vsync_ = nullptr;
    /** XComponent 的原生窗口（设置期望上屏时间用，生命周期跟 surface）。 */
    void *nativeWindow_ = nullptr;
    /** 最近一次 vsync 时间戳（CLOCK_MONOTONIC ns，vsync 线程写）与周期（渲染线程查询）。 */
    std::atomic<int64_t> vsyncNs_{0};
    std::atomic<int64_t> vsyncPeriodNs_{0};
    /** 渲染线程：上一帧画在哪个 vsync 周期（= 当时的 vsyncNs_），同一周期不画第二帧。 */
    int64_t lastRenderVsyncNs_ = -1;
    int64_t periodQueriedVsyncNs_ = -1;
    bool vsyncClockLogged_ = false;
    std::mutex vsyncWaitMutex_;
    std::condition_variable vsyncWaitCv_;
    /** 节拍等待后回到循环顶部时，把"这一轮要画"带到下一轮（forceRedraw_ 已被消费）。 */
    bool carryActive_ = false;
    /** 上一轮弹簧是否还在动（判停的那一轮做一次落定对齐，见 SettleTabSprings）。 */
    bool wasSpringing_ = false;

    /**
     * 缓存的 uniform location（每帧几百次 glGetUniformLocation 是字符串查找）。
     * name 必须是**字符串字面量**（按指针做键）；程序重建（DestroyGl）时清空。
     */
    GLint Uniform(GLuint program, const char *name);
    struct UniformKey {
        GLuint program;
        const char *name;
        bool operator==(const UniformKey &other) const {
            return program == other.program && name == other.name;
        }
    };
    struct UniformKeyHash {
        size_t operator()(const UniformKey &key) const {
            return std::hash<const void *>()(key.name) ^ (static_cast<size_t>(key.program) * 0x9E3779B97F4A7C15ull);
        }
    };
    std::unordered_map<UniformKey, GLint, UniformKeyHash> uniformCache_;

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLConfig config_ = nullptr;

    GLuint bgFbo_ = 0;
    GLuint bgTex_ = 0;
    GLuint blurFbo_[2] = {0, 0};
    GLuint blurTex_[2] = {0, 0};
    // 顶部按钮的**第二套**模糊链：按钮的模糊半径与底座不同（id 10 + id 1 × id 11），
    // 复用底座那张模糊结果会让按钮内部明显偏"清"。
    GLuint blurFbo2_[2] = {0, 0};
    GLuint blurTex2_[2] = {0, 0};
    // 主模糊链的**降采样金字塔**（第 0/1 级 = 1/2、1/4 分辨率）。
    // σ 大时先降采样再叠核：每级像素少 4×，同样的多趟核在低分辨率上更便宜。
    GLuint blurDownFbo_[2][2] = {{0, 0}, {0, 0}};
    GLuint blurDownTex_[2][2] = {{0, 0}, {0, 0}};
    int blurDownW_[2] = {0, 0};
    int blurDownH_[2] = {0, 0};
    /**
     * 多趟可分离高斯（间距递增，见实现里的方差累加）。
     * sigmaPx 是**高斯 σ**（= 上游 Compose `blur(radius)` 的 radius），不是单趟采样间距。
     */
    GLuint BlurIterations(GLuint sourceTex, float sigmaPx, int w, int h, GLuint fboPair[2],
                          GLuint texPair[2]);
    /** 按钮专用模糊：全分辨率 + scissor（按钮半径通常较小，裁剪比降采样更省）。 */
    GLuint RenderButtonBlur(GLuint sourceTex, float radiusPx);
    /** 主模糊链：小 σ 全分辨率；大 σ 走降采样金字塔。返回可直接线性采样的纹理。 */
    GLuint RenderBlur(GLuint sourceTex, float radiusPx);
    GLuint sceneFbo_ = 0;   // 背景 + 底板玻璃（不含文字；指示器要采样它）
    GLuint sceneTex_ = 0;
    GLuint labelTex_ = 0;   // 文字位图（native_drawing 栅格化，全为常态色）
    // 强调色文字位图：与 labelTex_ **逐像素同布局**，只在指示器内部按同一折射 UV
    // 叠加 —— "透过指示器看到的才是凸显色"（见 kIndicatorShaderBody 的 uLabelsAccent）
    GLuint labelAccentTex_ = 0;
    GLuint iconTex_ = 0;    // 顶部三个按钮的图标（横向排列，native_drawing 栅格化）
    int iconTexSizePx_ = 0; // 当前图标位图的单格边长（px）；尺寸变了要重传
    GLuint photoTex_ = 0;   // 照片背景（裸 RGBA）
    int photoW_ = 0;
    int photoH_ = 0;
    std::vector<unsigned char> photoPixels_;
    std::mutex photoMutex_;
    std::atomic<bool> photoDirty_{false};

    // ---- ArkTS 内容纹理（BuilderNode 纹理导出，见 ContentSurfaceId）----
    // 链路：ArkUI 渲染 BuilderNode -> OH_NativeImage（外纹理 GL_TEXTURE_EXTERNAL_OES）
    //       -> 每帧 blit 到 2D 的 contentBlitTex_ -> 当作背景喂给玻璃。
    // 之所以要 blit 成 2D：模糊链 / CopyBackgroundToScene 都只认 sampler2D。
    GLuint contentTex_ = 0;       // 外纹理（GL_TEXTURE_EXTERNAL_OES）
    GLuint contentBlitTex_ = 0;   // 外纹理每帧拷进来的 2D 纹理
    GLuint contentFbo_ = 0;
    GLuint progExtCopy_ = 0;      // samplerExternalOES + 变换矩阵 -> 2D
    OH_NativeImage *contentImage_ = nullptr;
    float contentMatrix_[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                                0.0f, 1.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 1.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, 1.0f};
    bool contentValid_ = false;   // 至少收到过一帧，bgTex_ 才有内容可采样
    bool contentLogged_ = false;
    std::string contentSurfaceId_;
    mutable std::mutex contentMutex_;

    /** 渲染线程：创建外纹理 + OH_NativeImage（拿 surfaceId）。 */
    void CreateContentImage();
    /** 渲染线程：取最新帧并 blit 到 contentBlitTex_。返回 true 表示这帧有新内容。 */
    bool UpdateContentTexture(int w, int h);
    GLuint progBg_ = 0;
    GLuint progBlur_ = 0;
    GLuint progGlass_ = 0;
    GLuint progIndicator_ = 0;
    GLuint progGlow_ = 0;   // 跟手光斑（BlendMode.Plus 的加性 pass）
    GLuint progIcon_ = 0;   // 按钮图标（采样 iconTex_，叠在按钮之上）
    GLuint progSolid_ = 0;  // 目录场景：纯色圆角矩形（轨道 / 填充 / 卡片 / 对话框按钮）
    GLuint progCopy_ = 0;   // 目录场景：背景 -> sceneTex_ 的拷贝
    GLuint progSprite_ = 0; // 目录场景：文字 / 图标位图（预乘 alpha）
    GLuint progProgBlur_ = 0;  // 渐进模糊（模糊 + 着色按纵向渐变淡出）
    int targetW_ = 0;
    int targetH_ = 0;
    int labelW_ = 0;
    int labelH_ = 0;
    int selectedTab_ = 0;
    std::atomic<bool> labelsDirty_{false};
    bool UploadLabels(int w, int h);
    bool UploadIcons(int iconSizePx);

    // ---- 调试 ----
    // 注：Milestone 1 时代这里还硬编码过 refractionHeight_ / refractionAmount_ /
    // touch 用的 blurRadius_ / dispersionEnabled_，接上 ArkUI 菜单后全部改走 `params_`，
    // 那几个成员已经没人读，删掉以免和参数表混淆。
    std::atomic<int> debugMode_{0};

    // ---- 指示器动画（S3）----
    //
    // 与参考实现的 DampedDragAnimation 对应：
    //   value        连续页签位置 0..2（弹簧追赶 targetValue：始终有过渡位移动画，不瞬移）
    //   targetValue  拖动目标 = 触摸位置（绝对坐标，不累积偏移）；松手后 value 弹簧对齐最近整数
    //   speed        弹簧平滑后的速度（驱动拉伸/压扁形变）
    //   press        按压进度 0..1（驱动折射/色散/高光/阴影/内阴影增强）
    //   scaleX/Y     按压放大（pressedScale），独立弹簧（id 60..63）
    struct IndicatorAnim {
        float value = 0.0f;
        float valueVelocity = 0.0f;
        float targetValue = 0.0f;
        float speed = 0.0f;
        float speedVelocity = 0.0f;
        float press = 0.0f;
        float pressVelocity = 0.0f;
        float scaleX = 1.0f;
        float scaleXVelocity = 0.0f;
        float scaleY = 1.0f;
        float scaleYVelocity = 0.0f;
        int targetIndex = 0;
        bool dragging = false;
    };
    IndicatorAnim anim_;
    // 本帧内累积的目标（手指）位移（value 单位）。OnTouch 写、UpdateAnim 读并清零。
    // 之所以不用"每事件位移 × 60"：指针事件率不一定是 60Hz（真机实测有 120Hz），
    // 那种近似会让速度（以及形变）随事件率漂移。
    float moveAccum_ = 0.0f;

    // ---- 顶部三个玻璃按钮（对齐参考实现的 GlassIconButton）----
    // 图标自左向右：拖动 / 通知 / 更多；点击分别开出 指示器·拖动 / 指示器·静止 / 底座 菜单。
    static constexpr int kIconButtonCount = 3;
    float btnPress_[kIconButtonCount] = {0.0f, 0.0f, 0.0f};
    float btnPressVelocity_[kIconButtonCount] = {0.0f, 0.0f, 0.0f};
    int btnDownIndex_ = -1;   // 当前按下的按钮下标（-1 = 无），由 OnTouch 写、UpdateAnim 读

    // 跟手光斑（InteractiveHighlight）：按压进度有**独立**弹簧（id 31/32），
    // 与指示器的 press 弹簧（58/59）分开 —— 上游就是这么分的（glowPress* vs press*）。
    float glowPress_ = 0.0f;
    float glowPressVelocity_ = 0.0f;

    /** 按钮边长（px）= id 9。 */
    float ButtonSizePx() const;
    /** 按钮间距（px）= 12dp（参考实现: Arrangement.spacedBy(12.dp)）。 */
    float ButtonSpacingPx() const;
    /** 按钮矩形（GL 坐标：y 从**底部**起算）。index 0 = 最左。 */
    void ButtonRectPx(int index, int surfaceW, int surfaceH, float *left, float *bottom) const;
    /** 命中测试（touchX/touchY 用**屏幕坐标**，y 从顶部起算）。 */
    bool HitButton(float touchX, float touchY, int surfaceW, int surfaceH, int *index) const;

    // 浮动玻璃件（与底板同材质、同尺寸，可用手指上下左右拖）
    std::atomic<float> floatX_{0.0f};
    std::atomic<float> floatY_{0.0f};   // y 从底部起算，与 GL 一致
    bool floatInited_ = false;
    bool draggingFloat_ = false;
    float floatGrabDX_ = 0.0f;
    float floatGrabDY_ = 0.0f;

    // 照片背景的滚动（0 = 顶部，1 = 底部）
    std::atomic<float> scroll_{0.0f};
    // 底板视差（拖动时整条底板位移，松手弹簧回零）
    float parallaxAccum_ = 0.0f;     // 累积拖动位移（px）
    float parallaxVel_ = 0.0f;
    std::atomic<float> panelOffsetPx_{0.0f};

    float scrollGrabStart_ = 0.0f;
    float scrollGrabY_ = 0.0f;
    bool scrolling_ = false;   // 指针事件之间的瞬时速度（由 OnTouch 写、UpdateAnim 读）
    std::mutex animMutex_;
    int64_t lastAnimMs_ = 0;
    int64_t lastAnimNs_ = 0;   // 弹簧步长基准（纳秒，见 RenderLoop）
    void UpdateAnim(float dtSeconds);
    /** 判停时页签弹簧的状态（见 SettleTabSprings）。 */
    enum class SpringSettle {
        kFar,       // 还有弹簧离目标较远（欠阻尼弹簧折返点的误判）：当它还在动
        kSnapped,   // 都贴着目标，已对齐、速度清零，有值被改动：要再画一帧
        kExact,     // 本来就在目标上
    };
    /**
     * 速率判据判停时调用：每根页签弹簧都贴着目标（kSettle*Tolerance）才对齐目标值、速度清零。
     * 不对齐的话静止画面停在哪一帧取决于帧时序（判停时离目标还差一点）。
     */
    SpringSettle SettleTabSprings();

    /**
     * 页签格的内缩（px）。**所有"页签格几何"都必须走这里**：触摸映射（TabValueFromX）、
     * 视差累积用的格宽、文字排版、指示器定位 —— 几处各写一份就会各自漂移。
     *
     * 对齐参考实现的 `innerPaddingDp = max(panelInnerPadding, capInset + indicatorExpand)`：
     * 胶囊端头圆弧必须和底板端头圆弧**同心**，所以格子内缩至少要留出
     * 「同心留白 + 外扩量」，否则两端页签的胶囊会探出底板。
     * 默认值下 capInset(4) + expand(10) = 14 = panelInnerPadding，所以默认行为不变。
     */
    float TabInnerPadPx() const;

    /** 两端页签的同心留白（px）= (底板高 - 胶囊高)/2，下限 0（＝ 参考实现的 capInsetPx）。 */
    float IndicatorEndInsetPx() const;

    float TabValueFromX(float x, int surfaceW) const;

    /** 全参数表（编号与 glass_params.cpp 一致） */
    GlassParams params_;

    /** 顶部按钮点击 -> ArkTS 菜单的回链（见 SetMenuRequestFn）。 */
    std::atomic<MenuRequestFn> menuRequestFn_{nullptr};

    // ---- FPS 统计 ----
    int frameCount_ = 0;
    int64_t fpsWindowStartMs_ = 0;
};

}  // namespace glass

#endif  // GLASS_GLES_RENDERER_H
