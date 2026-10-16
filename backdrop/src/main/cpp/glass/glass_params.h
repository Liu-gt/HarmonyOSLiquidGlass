#ifndef GLASS_PARAMS_H
#define GLASS_PARAMS_H

#include <mutex>

namespace glass {

/**
 * 参数表容量。唯一事实来源：数组尺寸、构造函数的初始化循环、SetNumber 与 N 的
 * 上界检查均统一引用该常量。
 * 新增参数时需同步更新该数值并配置默认值。
 */
constexpr int kNumValues = 140; // number id 0..139
constexpr int kNumBools = 16;   // bool id 0..15

/**
 * 玻璃参数表。编号与取值须与 ArkTS 侧 Specs.ets 中的参数定义保持一致：
 *
 *   number 0..93 ：数值参数，详见构造函数中的默认值
 *   bool   0..7  ：0=底座色散 1=底座提饱和 2=指示器色散 3=性能HUD 4=静默采样 5=环境光
 *                  6=深度效应 7=纯色高光
 *
 * 之所以按"编号数组"存而不是具名字段：原生侧要能直接承接
 * `GlassGLSetNumber(id, value)`，和 ArkUI 菜单一一对应，不做任何二次映射。
 *
 * 单位约定：所有长度类参数**以 dp 存**，使用时用 `px(id)` 乘 density 换成像素。
 */
class GlassParams {
public:
    GlassParams();

    void SetNumber(int id, float value);
    void SetBool(int id, bool value);

    /** 原始值（dp / 比例 / 刚度…） */
    float N(int id) const;
    bool B(int id) const;

    /** dp -> px */
    float Px(int id) const;

    void SetDensity(float density);
    float Density() const;

private:
    mutable std::mutex mutex_;
    float values_[kNumValues];   // 下标 0..82（68 起都是本项目追加的）
    bool bools_[kNumBools];
    float density_ = 4.0f;  // 本机实测 1280px / 320vp = 4
};

// 参数编号（可读性用；值必须与 glass_params.cpp 的 `id N` 注释一致）
namespace pid {
// 底座
constexpr int kBarHeight = 0;
constexpr int kBlurRadius = 1;
constexpr int kRefractionHeight = 2;
constexpr int kRefractionAmount = 3;
constexpr int kContainerAlpha = 4;
constexpr int kPressGrowth = 5;
// 跟手光斑（InteractiveHighlight）：平面分量 / 径向分量 / 光斑半径系数
constexpr int kGlowFlat = 6;
constexpr int kGlowRadial = 7;
constexpr int kGlowRadiusFactor = 8;
constexpr int kInnerPadding = 20;
constexpr int kPanelParallax = 21;
constexpr int kOuterPaddingH = 22;
constexpr int kOuterPaddingV = 23;
constexpr int kLabelFontSize = 24;
constexpr int kLabelSelectedAlpha = 25;
constexpr int kLabelUnselectedAlpha = 26;
constexpr int kPanelReturnDamping = 27;
constexpr int kPanelReturnStiffness = 28;
constexpr int kIconButtonSize = 9;
constexpr int kIconBlurBase = 10;
constexpr int kIconBlurScale = 11;
constexpr int kIconRefractionRest = 12;
constexpr int kIconRefractionPress = 13;
constexpr int kIconHighlightRest = 14;
constexpr int kIconHighlightPress = 15;
constexpr int kIconShadowRest = 16;
constexpr int kIconShadowPress = 17;
constexpr int kIconInnerShadowRadius = 18;
constexpr int kIconPressScale = 19;
constexpr int kIconPressDamping = 29;
constexpr int kIconPressStiffness = 30;
// 跟手光斑的按压进度弹簧（与指示器的 press 弹簧分开，上游就是这么分的）
constexpr int kGlowPressDamping = 31;
constexpr int kGlowPressStiffness = 32;
constexpr int kBackgroundScrollRange = 64;
// 追加（超出参考参数表）：底板圆角半径
constexpr int kPanelCornerRadius = 68;
// 追加：棱边高光的环宽（dp）。上游 Highlight.width 默认 0.5dp
constexpr int kHighlightWidth = 69;
// 追加：外投影（对齐上游 Shadow 默认值 radius 24dp / offset radius/6 / 黑 alpha 0.1）
constexpr int kShadowBlur = 70;
constexpr int kShadowOffset = 71;
constexpr int kShadowAlpha = 72;
// 追加：浮动玻璃件的**四角独立圆角**（dp），对齐上游 Shaders.kt radiusAt 的能力。
// 缺省 25dp = 底板圆角，所以默认外观不变；拖浮动件后滑这四项即可看出四角差异。
constexpr int kFloatCornerTL = 73;
constexpr int kFloatCornerTR = 74;
constexpr int kFloatCornerBR = 75;
constexpr int kFloatCornerBL = 76;
// 追加：colorControls（对齐上游 effects/ColorFilter.kt 的亮度/对比度/饱和度）+ 效果不透明度
constexpr int kColorBrightness = 77;
constexpr int kColorContrast = 78;
constexpr int kColorSaturation = 79;
constexpr int kEffectOpacity = 80;

// 指示器 · 静止
constexpr int kRefractionHeightRest = 33;
constexpr int kRefractionAmountRest = 34;
constexpr int kTintRest = 35;
constexpr int kSurfaceWhiteRest = 36;
constexpr int kHighlightRest = 37;
constexpr int kShadowRest = 38;
constexpr int kInnerShadowRadiusRest = 39;
constexpr int kInnerShadowAlphaRest = 40;
constexpr int kIndicatorHeight = 41;
constexpr int kIndicatorPadding = 42;
constexpr int kIndicatorBlurRest = 65;
constexpr int kIndicatorExpand = 67;

// 指示器 · 拖动
constexpr int kRefractionHeightDrag = 43;
constexpr int kRefractionAmountDrag = 44;
constexpr int kTintDrag = 45;
constexpr int kSurfaceWhiteDrag = 46;
constexpr int kHighlightDrag = 47;
constexpr int kShadowDrag = 48;
constexpr int kInnerShadowRadiusDrag = 49;
constexpr int kInnerShadowAlphaDrag = 50;
constexpr int kIndicatorBlurDrag = 66;

// 指示器 · 形变
constexpr int kPressedScale = 51;
constexpr int kVelocityStretch = 52;
constexpr int kVelocitySquash = 53;
// 追加：按压时**玻璃内部看到的内容**缩小（采样缩放 < 1，绕胶囊中心）。
// 与 kPressedScale（形状膨胀）正交：形状往外长、内容往回收，才是透镜缩微的感觉。
constexpr int kPressSampleShrink = 81;
// 追加：按压时胶囊的横向形状放大倍率（id 51 是纵向）
constexpr int kPressedScaleX = 82;
// 追加：页签图标尺寸（dp）
constexpr int kTabIconSize = 83;
// 追加：底板 / 指示器表面填充色（线性 0..1 的 R/G/B）
constexpr int kSurfaceColorR = 84;
constexpr int kSurfaceColorG = 85;
constexpr int kSurfaceColorB = 86;
// 追加：选中胶囊的底色不透明度（静止 / 拖动）
constexpr int kIndicatorFillRest = 87;
constexpr int kIndicatorFillDrag = 88;
// 追加：按压时指示器里的页签文字 / 图标放大比例（0 = 关，0.2 = 放大到 1.2 倍）
constexpr int kPressLabelScale = 89;
// 追加：指示器里那层凸显色文字的图标尺寸 / 字号（dp）
constexpr int kTabIconSizeAccent = 90;
constexpr int kLabelFontSizeAccent = 91;
// 追加：按压态底板外投影（模糊半径 dp / 不透明度）
constexpr int kShadowBlurPress = 92;
constexpr int kShadowAlphaPress = 93;

// 指示器 · 动画
constexpr int kDragValueDamping = 54;
constexpr int kDragValueStiffness = 55;
constexpr int kDragVelocityDamping = 56;
constexpr int kDragVelocityStiffness = 57;
constexpr int kPressDamping = 58;
constexpr int kPressStiffness = 59;
constexpr int kScaleXDamping = 60;
constexpr int kScaleXStiffness = 61;
constexpr int kScaleYDamping = 62;
constexpr int kScaleYStiffness = 63;

// Dialog · 弹窗材质（94..116）
constexpr int kDialogDimAlpha = 94;
constexpr int kDialogCornerRadius = 95;
constexpr int kDialogG2Factor = 96;
constexpr int kDialogBlurRadius = 97;
constexpr int kDialogRefractionHeight = 98;
constexpr int kDialogRefractionAmount = 99;
constexpr int kDialogRimClarity = 100;
constexpr int kDialogBevelAlphaScale = 101;
constexpr int kDialogVibrancy = 102;
constexpr int kDialogBrightness = 103;
constexpr int kDialogContrast = 104;
constexpr int kDialogSurfaceAlpha = 105;
constexpr int kDialogSurfaceR = 106;
constexpr int kDialogSurfaceG = 107;
constexpr int kDialogSurfaceB = 108;
constexpr int kDialogHighlight = 109;
constexpr int kDialogHighlightWidth = 110;
constexpr int kDialogAmbientIntensity = 111;
constexpr int kDialogInnerRadius = 112;
constexpr int kDialogInnerAlpha = 113;
constexpr int kDialogShadowBlur = 114;
constexpr int kDialogShadowOffset = 115;
constexpr int kDialogShadowAlpha = 116;

// Dialog · 内部按钮材质（117..139）
constexpr int kDialogBtnCornerRadius = 117;
constexpr int kDialogBtnG2Factor = 118;
constexpr int kDialogBtnBlurRadius = 119;
constexpr int kDialogBtnRefractionHeight = 120;
constexpr int kDialogBtnRefractionAmount = 121;
constexpr int kDialogBtnRimClarity = 122;
constexpr int kDialogBtnBevelAlphaScale = 123;
constexpr int kDialogBtnVibrancy = 124;
constexpr int kDialogBtnBrightness = 125;
constexpr int kDialogBtnContrast = 126;
constexpr int kDialogBtnSurfaceAlpha = 127;
constexpr int kDialogBtnSurfaceR = 128;
constexpr int kDialogBtnSurfaceG = 129;
constexpr int kDialogBtnSurfaceB = 130;
constexpr int kDialogBtnHighlight = 131;
constexpr int kDialogBtnHighlightWidth = 132;
constexpr int kDialogBtnAmbientIntensity = 133;
constexpr int kDialogBtnInnerRadius = 134;
constexpr int kDialogBtnInnerAlpha = 135;
constexpr int kDialogBtnShadowBlur = 136;
constexpr int kDialogBtnShadowOffset = 137;
constexpr int kDialogBtnShadowAlpha = 138;
constexpr int kDialogBtnPressGrow = 139;
}  // namespace pid

// ---------------------------------------------------------------------------
// 开关编号。开关是与数值参数**分离的 id 空间**（SetBool / B(id) 用）。
// 3/4 是调试开关（性能 HUD / 静默采样），页面上没有对应条目。
// ---------------------------------------------------------------------------
namespace bid {
constexpr int kPanelDispersion = 0;
constexpr int kPanelVibrancy = 1;
constexpr int kIndicatorDispersion = 2;
constexpr int kPerfHud = 3;
constexpr int kSilentProbe = 4;
constexpr int kAmbient = 5;
constexpr int kDepthEffect = 6;      // 底座/指示器：折射方向叠径向分量（上游 lens 的 depthEffect）
constexpr int kPlainHighlight = 7;   // 底座：纯色高光（上游 Highlight.Plain）替代描边环

// Dialog 场景开关（8..15）
constexpr int kDialogDispersion = 8;
constexpr int kDialogDepthEffect = 9;
constexpr int kDialogAmbient = 10;
constexpr int kDialogPlainHighlight = 11;
constexpr int kDialogBtnDispersion = 12;
constexpr int kDialogBtnDepthEffect = 13;
constexpr int kDialogBtnAmbient = 14;
constexpr int kDialogBtnPlainHighlight = 15;
}  // namespace bid

}  // namespace glass

#endif  // GLASS_PARAMS_H
