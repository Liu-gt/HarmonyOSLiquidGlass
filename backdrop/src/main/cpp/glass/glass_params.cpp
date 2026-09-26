#include "glass_params.h"

#include <hilog/log.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "GLASS"
#define LOG_DOMAIN 0x0000

#define GLOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)

namespace glass {

GlassParams::GlassParams() {
    for (int i = 0; i < kNumValues; i++) {
        values_[i] = 0.0f;
    }
    for (int i = 0; i < kNumBools; i++) {
        bools_[i] = false;
    }

    // ---- 默认值：逐条对齐 glass_params.cpp ----
    // 底座 · 材质
    values_[0] = 50.0f;    // barHeight
    values_[1] = 0.2f;     // blurRadius
    values_[2] = 12.0f;    // refractionHeight
    values_[3] = 30.0f;    // refractionAmount
    values_[4] = 0.5f;     // containerAlpha
    values_[5] = 3.0f;     // pressGrowth
    values_[6] = 0.07f;    // glowFlat
    values_[7] = 0.15f;    // glowRadial
    values_[8] = 1.5f;     // glowRadiusFactor
    // 底座 · 按钮
    values_[9] = 46.0f;    // iconButtonSize
    values_[10] = 4.0f;    // iconBlurBase
    values_[11] = 0.3f;    // iconBlurScale
    values_[12] = 5.0f;    // iconRefractionRest
    values_[13] = 11.0f;   // iconRefractionPress
    values_[14] = 0.6f;    // iconHighlightRest
    values_[15] = 0.4f;    // iconHighlightPress
    values_[16] = 0.4f;    // iconShadowRest
    values_[17] = 0.6f;    // iconShadowPress
    values_[18] = 6.0f;    // iconInnerShadowRadius
    values_[19] = 1.14f;   // iconPressScale
    // 底座 · 布局
    values_[20] = 14.0f;   // panelInnerPadding
    values_[21] = 4.0f;    // panelParallax
    values_[22] = 48.0f;   // outerPaddingH
    values_[23] = 26.0f;   // outerPaddingV
    values_[24] = 14.0f;   // labelFontSize
    values_[25] = 1.0f;    // labelSelectedAlpha
    values_[26] = 0.62f;   // labelUnselectedAlpha
    // 底座 · 动画
    values_[27] = 1.0f;    // panelReturnDamping
    values_[28] = 300.0f;  // panelReturnStiffness
    values_[29] = 0.45f;   // iconPressDamping
    values_[30] = 600.0f;  // iconPressStiffness
    values_[31] = 0.5f;    // glowPressDamping
    values_[32] = 300.0f;  // glowPressStiffness
    // 指示器 · 静止
    values_[33] = 0.0f;    // refractionHeightRest
    values_[34] = 0.0f;    // refractionAmountRest
    values_[35] = 0.25f;   // tintRest
    values_[36] = 0.0f;    // surfaceWhiteRest
    values_[37] = 0.0f;    // highlightRest
    values_[38] = 0.2f;    // shadowRest（鼓出时用于和底座分离）
    values_[39] = 0.0f;    // innerShadowRadiusRest
    values_[40] = 0.0f;    // innerShadowAlphaRest
    // 指示器 · 布局
    values_[41] = 42.0f;   // indicatorHeight（底板 50dp − 上下各 4dp，与旧 60/52 同留白）
    values_[42] = 0.0f;    // indicatorPadding
    // 指示器 · 拖动
    values_[43] = 13.0f;   // refractionHeightDrag
    values_[44] = 25.0f;   // refractionAmountDrag
    values_[45] = 0.0f;    // tintDrag
    values_[46] = 0.0f;    // surfaceWhiteDrag
    values_[47] = 0.0f;    // highlightDrag
    values_[48] = 0.9f;    // shadowDrag
    values_[49] = 0.0f;    // innerShadowRadiusDrag
    values_[50] = 0.0f;    // innerShadowAlphaDrag
    // 指示器 · 形变
    values_[51] = 1.7f;           // pressedScale
    values_[52] = 0.75f;          // velocityStretch
    values_[53] = 0.4f;           // velocitySquash
    // 指示器 · 动画
    values_[54] = 1.0f;    // dragValueDamping
    values_[55] = 400.0f;  // dragValueStiffness
    values_[56] = 0.45f;   // dragVelocityDamping
    values_[57] = 400.0f;  // dragVelocityStiffness
    values_[58] = 1.0f;    // pressDamping
    values_[59] = 1000.0f; // pressStiffness
    values_[60] = 0.6f;    // scaleXDamping
    values_[61] = 250.0f;  // scaleXStiffness
    values_[62] = 0.7f;    // scaleYDamping
    values_[63] = 250.0f;  // scaleYStiffness
    // 追加
    values_[64] = 1.0f;    // backgroundScrollRange
    values_[65] = 0.0f;    // indicatorBlurRest
    values_[66] = 0.0f;    // indicatorBlurDrag
    values_[67] = 10.0f;   // indicatorExpand
    // 追加：底板圆角半径。默认 = 底板高的一半（50/2 = 25dp → 胶囊形状）
    values_[68] = 25.0f;
    // 追加：棱边高光环宽（dp）。上游 Highlight.width = 0.5dp，blurRadius = width/2
    values_[69] = 0.5f;
    // 追加：外投影（上游 Shadow() 默认值：radius 24dp、offset.y = radius/6、黑 alpha 0.1）
    values_[70] = 24.0f;
    values_[71] = 4.0f;
    values_[72] = 0.10f;
    // 追加：浮动件四角圆角（dp）。缺省 25 = 底板圆角，默认外观不变
    values_[73] = 25.0f;   // 左上
    values_[74] = 25.0f;   // 右上
    values_[75] = 25.0f;   // 右下
    values_[76] = 25.0f;   // 左下
    // 追加：colorControls（0/1 = 原样）+ 效果不透明度
    values_[77] = 0.0f;    // brightness
    values_[78] = 1.0f;    // contrast
    values_[79] = 1.0f;    // saturation
    values_[80] = 1.0f;    // effectOpacity
    // 追加：按压时指示器内部内容的采样缩小比例（0 = 关，0.1 = 缩到 90%）
    values_[81] = 0.10f;   // pressSampleShrink
    // 追加：按压时指示器里的页签文字/图标放大比例（0 = 关，0.2 = 放大到 1.2 倍）
    values_[82] = 0.20f;   // pressLabelScale

    // ---- 开关 ----
    bools_[0] = false;  // 底座色散（用户定版：默认关）
    bools_[1] = true;   // 底座提饱和
    bools_[2] = true;   // 指示器色散
    bools_[3] = false;  // 性能 HUD
    bools_[4] = false;  // 静默采样
    bools_[5] = false;  // 环境光（上游 spike 未启用，默认关）
    bools_[6] = false;  // 深度效应（上游页签页未启用，默认关）
    bools_[7] = false;  // 纯色高光（上游默认是描边环，默认关）
}

void GlassParams::SetNumber(int id, float value) {
    if (id < 0 || id >= kNumValues) {
        GLOG("SetNumber: id %{public}d out of range", id);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    values_[id] = value;
}

void GlassParams::SetBool(int id, bool value) {
    if (id < 0 || id >= kNumBools) {
        GLOG("SetBool: id %{public}d out of range", id);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    bools_[id] = value;
}

float GlassParams::N(int id) const {
    if (id < 0 || id >= kNumValues) {
        return 0.0f;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return values_[id];
}

bool GlassParams::B(int id) const {
    if (id < 0 || id >= kNumBools) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return bools_[id];
}

float GlassParams::Px(int id) const {
    return N(id) * Density();
}

void GlassParams::SetDensity(float density) {
    std::lock_guard<std::mutex> lock(mutex_);
    density_ = (density > 0.0f) ? density : 4.0f;
}

float GlassParams::Density() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return density_;
}

}  // namespace glass
