#include "gles_renderer.h"

#include <GLES2/gl2ext.h>   // GL_TEXTURE_EXTERNAL_OES（OH_NativeImage 的外纹理）
#include <hilog/log.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "glass_params.h"
#include "text_layer.h"

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "GLASS"
#define LOG_DOMAIN 0x0000

#define GLOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)
#define GERR(...) OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)

namespace glass {

namespace {

/**
 * 空帧可用回调：只是把 OH_NativeImage 的 listener 占上。
 *
 * 不注册的话，ArkUI 每产一帧，消费端都会打一条
 * `SurfaceImage::OnBufferAvailable listener is nullptr` 的错误日志（约 30 条/秒）。
 * 帧的获取仍走渲染线程每帧的 `OH_NativeImage_UpdateSurfaceImage`，这里什么都不做
 * （文档也要求回调里不要调用 OH_NativeImage 的其它接口）。
 */
void OnContentFrameAvailableNoop(void * /*context*/) {}

// ---------------------------------------------------------------------------
// 底板与指示器**共用**的 GLSL 前导与 SDF/透镜 helper。
// 两个 shader 都从这里拼装，避免"两套实现"漂移 —— 与上游 SkSL 用
// `$RoundedRectSDF` 模板变量拼字符串是同一个做法。
// ---------------------------------------------------------------------------
constexpr char kGlslPrelude[] = R"(#version 300 es
precision highp float;
in vec2 vUv;

// 8bit 目标写入时的抖动：把 1/255 的量化台阶打散成噪点。
// 大半径模糊会把画面压成极平坦的渐变，8bit 量化在这种渐变上会显出宽色带（Mach band）；
// 每次写入抖 ±0.5LSB 可以打断台阶。用交错梯度噪声：比 sin 哈希规整、比蓝噪声便宜。
// ⚠️ 每个会写进 8bit 纹理/屏幕的 shader 都要加一次，链路上任何一处不抖都会重新长出阶梯。
float ditherNoise(vec2 px) {
    return (fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))) - 0.5) / 255.0;
}
)";

// 底板与指示器**共用**的 SDF / 透镜 helper。
// 两个 shader 都从这里拼装，避免"两套实现"漂移 —— 与上游 SkSL 用
// `$RoundedRectSDF` 模板变量拼字符串是同一个做法。
constexpr char kGlslSdfHelpers[] = R"(
float sdRoundedRect(vec2 coord, vec2 halfSize, float radius) {
    vec2 cornerCoord = abs(coord) - (halfSize - vec2(radius));
    float outside = length(max(cornerCoord, 0.0)) - radius;
    float inside = min(max(cornerCoord.x, cornerCoord.y), 0.0);
    return outside + inside;
}

// 距离场梯度（= 表面外法线）。**用中央差分而不是上游的解析分支式**：
// 解析式在形状中轴 / 内矩形处会 `normalize(vec2(0))` 突变，直接拿来做棱边高光
// 会出现硬边楔形（之前是靠"边缘衰减遮罩"硬压掉的，那是权宜之计）。
// 差分版本处处平滑，高光就能用上游的原始公式而不需要遮罩。
vec2 gradSdRoundedRect(vec2 coord, vec2 halfSize, float radius) {
    float e = 1.0;
    float dx = sdRoundedRect(coord + vec2(e, 0.0), halfSize, radius)
             - sdRoundedRect(coord - vec2(e, 0.0), halfSize, radius);
    float dy = sdRoundedRect(coord + vec2(0.0, e), halfSize, radius)
             - sdRoundedRect(coord - vec2(0.0, e), halfSize, radius);
    vec2 g = vec2(dx, dy);
    float len = length(g);
    return len > 1e-4 ? g / len : vec2(0.0);
}

// 透镜位移量曲线：0（折射带内缘）-> 1（形状最外缘）
float circleMap(float x) {
    return 1.0 - sqrt(1.0 - x * x);
}

// 四角独立半径（对齐上游 Shaders.kt 的 radiusAt）。
// ⚠️ 上游是在 **y 向下的 Skia 坐标** 里写的，这里换成 GL 的 y 向上，
// 所以分量顺序是：x=左上，y=右上，z=右下，w=左下。
float radiusAt(vec2 coord, vec4 radii) {
    if (coord.y >= 0.0) {
        return coord.x >= 0.0 ? radii.y : radii.x;
    }
    return coord.x >= 0.0 ? radii.z : radii.w;
}

// colorControls（对齐上游 effects/ColorFilter.kt）：亮度（-1..1）、对比度（0..2）、饱和度（0..2）。
// 上游用 4x4 颜色矩阵（偏移按 0..255 归一化）；这里按同一公式逐像素算，单位换算到 0..1。
vec3 colorControls(vec3 c, float brightness, float contrast, float saturation) {
    float invSat = 1.0 - saturation;
    float r = 0.213 * invSat;
    float g = 0.715 * invSat;
    float b = 0.072 * invSat;
    float cr = contrast * r;
    float cg = contrast * g;
    float cb = contrast * b;
    float cs = contrast * saturation;
    float t = 0.5 - contrast * 0.5 + brightness;
    return vec3(
        dot(c, vec3(cr + cs, cg, cb)) + t,
        dot(c, vec3(cr, cg + cs, cb)) + t,
        dot(c, vec3(cr, cg, cb + cs)) + t
    );
}
)";

// ---------------------------------------------------------------------------
// 顶点着色器：全屏三角形，不需要 VBO
// ---------------------------------------------------------------------------
constexpr char kVertexShader[] = R"(#version 300 es
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    // ⚠️ Y 轴约定（真机用四角标记色块标定）：GL 呈现后 vUv.y = 0 落在**物理屏幕底部**。
    //    所以"左上为原点"的布局参数要用 uResolution.y - y 换算（见背景 shader）。
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// ---------------------------------------------------------------------------
// pass 1：背景。静态照片（cover 铺满 + 纵向滚动，对齐参考实现的 PhotoBackground）。
// ---------------------------------------------------------------------------
constexpr char kBackgroundShader[] = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform vec2 uResolution;
uniform sampler2D uPhoto;
uniform vec2 uPhotoSize;
uniform float uScrollRange;   // 参数 id 64
uniform float uScroll;        // 0..1
uniform float uContentMode;   // 1 = uPhoto 已是 ArkTS 内容（全屏 1:1，无 cover / 无滚动）
out vec4 outColor;

void main() {
    // ArkTS 内容纹理：blit 阶段已按屏幕方向摆正，直接 1:1 采样
    if (uContentMode > 0.5) {
        outColor = vec4(texture(uPhoto, vUv).rgb, 1.0);
        return;
    }

    // 照片还没上传时先铺一层深色，避免采样未初始化的纹理
    if (uPhotoSize.x < 1.0 || uPhotoSize.y < 1.0) {
        outColor = vec4(0.055, 0.060, 0.075, 1.0);
        return;
    }

    // 全屏展示：未启用滚动（uScrollRange <= 1.0）时直接全屏铺满呈现，不作放大与裁切；
    // 调节滚动幅度（uScrollRange > 1.0）时按对应范围滚动展示。
    vec2 uv;
    if (uScrollRange > 1.001) {
        float h = uResolution.y;
        vec2 p = vec2(vUv.x * uResolution.x, h - vUv.y * h);
        float contentH = h * uScrollRange;
        float s = max(uResolution.x / uPhotoSize.x, contentH / uPhotoSize.y);
        vec2 drawn = uPhotoSize * s;
        float maxScroll = max(0.0, contentH - h);
        vec2 origin = vec2((uResolution.x - drawn.x) * 0.5, (contentH - drawn.y) * 0.5);
        uv = (vec2(p.x, p.y + uScroll * maxScroll) - origin) / drawn;
    } else {
        uv = vec2(vUv.x, 1.0 - vUv.y);
    }
    outColor = vec4(texture(uPhoto, uv).rgb, 1.0);
}
)";

// ---------------------------------------------------------------------------
// ArkTS 内容纹理：OH_NativeImage 的外纹理 -> 2D（每帧一次全屏 blit）。
//
// 外纹理只能当"采样源"，不能进模糊链 / CopyBackgroundToScene（那两处只认 sampler2D），
// 所以先按 OH_NativeImage 给的变换矩阵（含生产端的翻转）拷成 2D，后续全走原管线。
// ---------------------------------------------------------------------------
constexpr char kExternalCopyShader[] = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
in vec2 vUv;
uniform samplerExternalOES uExternal;
uniform mat4 uTexMatrix;
out vec4 outColor;

void main() {
    // 直接按 GL 约定采外纹理得到的是上下颠倒的（生产端 buffer 是 top-down），
    // 而 OH_NativeImage 的变换矩阵正好带一次翻转（实测 m5 = -1），乘上去即正立。
    vec2 uv = (uTexMatrix * vec4(vUv, 0.0, 1.0)).xy;
    outColor = vec4(texture(uExternal, uv).rgb, 1.0);
}
)";

// ---------------------------------------------------------------------------
// pass 2/3：可分离高斯模糊（9 tap）
//
// ⚠️ uRadius 是**采样间距（texel）**，不是模糊半径：核的方差是 2.854·间距²，
//    即等效 σ ≈ 1.69 × 间距。调用方（BlurIterations）负责用"间距递增的多趟"把
//    总方差补到目标 σ²；单趟把间距拉到目标半径是之前 30px 级鬼影/色带的来源。
// ---------------------------------------------------------------------------
constexpr char kBlurShader[] = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uSource;
uniform vec2 uTexel;
uniform vec2 uDir;
uniform float uRadius;
out vec4 outColor;

float ditherNoise(vec2 px) {
    return (fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))) - 0.5) / 255.0;
}

void main() {
    vec2 step1 = uDir * uTexel * uRadius;
    vec3 sum = texture(uSource, vUv).rgb * 0.2270270270;
    sum += texture(uSource, vUv + step1 * 1.0).rgb * 0.1945945946;
    sum += texture(uSource, vUv - step1 * 1.0).rgb * 0.1945945946;
    sum += texture(uSource, vUv + step1 * 2.0).rgb * 0.1216216216;
    sum += texture(uSource, vUv - step1 * 2.0).rgb * 0.1216216216;
    sum += texture(uSource, vUv + step1 * 3.0).rgb * 0.0540540541;
    sum += texture(uSource, vUv - step1 * 3.0).rgb * 0.0540540541;
    sum += texture(uSource, vUv + step1 * 4.0).rgb * 0.0162162162;
    sum += texture(uSource, vUv - step1 * 4.0).rgb * 0.0162162162;
    outColor = vec4(sum + ditherNoise(gl_FragCoord.xy), 1.0);
}
)";

// ---------------------------------------------------------------------------
// pass 4：底板。背景 + 玻璃(折射/色散/模糊/提饱和/棱边高光/阴影)
// （页签场景的文字不在这里画：由 pass 5 叠加，见 kIndicatorShaderBody）
// 折射/色散主体逐行移植自 upstream Shaders.kt 的
// RoundedRectRefractionWithDispersionShaderString。
// ---------------------------------------------------------------------------
constexpr char kGlassShaderBody[] = R"(
uniform sampler2D uBackdrop;     // 清晰背景
uniform sampler2D uBlurred;      // 模糊后的背景
uniform vec2 uResolution;
uniform vec2 uTexel;
uniform vec4 uGlassRect;         // xy = 左上角（屏幕坐标，y 从底部起算），zw = 尺寸
uniform vec4 uRadii;             // 四角半径（左上/右上/右下/左下，px）；四值相等即普通圆角矩形
uniform float uScaleX;           // 形状缩放（绕中心；非 1 时做按压/速度形变）
uniform float uScaleY;
uniform float uRefractionHeight;
uniform float uRefractionAmount;
uniform float uDispersion;       // 0/1
uniform float uDepthEffect;      // 0/1：折射方向叠加径向分量（上游 lens 的 depthEffect）
uniform float uSurfaceAlpha;
uniform vec3 uSurfaceColor;      // 表面填充色（默认白；tinted 按钮传 tint）
uniform float uBlurRadius;
uniform float uVibrancy;
// 采样变换（放大镜）：samplePx = uSamplePivot + (px - uSamplePivot) / uSampleScale
uniform vec2 uSampleScale;
uniform vec2 uSamplePivot;
uniform float uBrightness;       // colorControls：亮度（-1..1，0 = 原样）
uniform float uContrast;         // colorControls：对比度（0..2，1 = 原样）
uniform float uSaturation;       // colorControls：饱和度（0..2，1 = 原样）
uniform float uEffectOpacity;    // 效果不透明度（0..1，1 = 原样）
uniform float uHighlight;
uniform float uPlainHighlight;   // 0/1：纯色高光（上游 Highlight.Plain）替代描边环
uniform float uHighlightWidth;   // 环宽（px）
uniform float uHighlightBlur;    // 环外缘柔化（px）
uniform float uAmbient;          // 0/1 环境光
uniform float uAmbientIntensity; // 环境光强度（上游 Ambient.intensity 默认 0.38）
uniform float uShadow;
uniform float uInnerRadius;      // 内阴影半径（px）＝ InnerShadow.radius（底座/浮动件恒 0）
uniform float uInnerAlpha;       // 内阴影强度（已含颜色黑 alpha 0.15）
uniform float uShadowBlur;       // 投影模糊半径（px）＝上游 Shadow.radius
uniform float uShadowOffset;     // 向下偏移（px）＝radius/6
uniform float uShadowAlpha;      // 有效不透明度
uniform sampler2D uLabels;       // 文字位图（覆盖整个底板矩形）
uniform vec2 uLabelSize;
uniform float uShowLabels;       // 0/1（页签场景改用 pass 5 的文字叠加后，这里恒 0）
// 0 = 直接写屏（底板：形状外画清晰背景 + 投影，alpha 恒 1）
// 1 = 叠加混合（浮动件：形状外只输出自己的投影，形状内输出覆盖率 alpha）
uniform float uOverlayMode;
out vec4 outColor;

vec3 saturate3(vec3 c, float amount) {
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(luma), c, amount);
}

void main() {
    // 屏幕坐标（y 从底部起算，与 uGlassRect 一致）
    vec2 px = vUv * uResolution;
    vec2 halfSize = uGlassRect.zw * 0.5;
    vec2 center = uGlassRect.xy + halfSize;
    // 形状缩放：把屏幕坐标映回"未缩放形状"的空间（绕中心）。scale = 1 时与原式逐像素等价。
    vec2 centeredCoord = (px - center) / vec2(uScaleX, uScaleY);
    vec2 local = centeredCoord + halfSize;           // 相对形状左上角（未缩放空间）

    // 采样变换（放大镜）：默认 scale=1 / pivot=0 时 samplePx == px
    vec2 samplePx = uSamplePivot + (px - uSamplePivot) / uSampleScale;
    vec2 sampleUv = samplePx / uResolution;

    vec3 sharp = texture(uBackdrop, sampleUv).rgb;
    vec3 blurred = texture(uBlurred, sampleUv).rgb;

    float radius = radiusAt(centeredCoord, uRadii);
    float sd = sdRoundedRect(centeredCoord, halfSize, radius);

    // ---- 形状外：清晰背景 + 外投影 ----
    // 对齐上游 Shadow 默认值：radius 24dp（模糊半径）、offset.y = radius/6、颜色黑 alpha 0.1。
    // 阴影形状 = 本形状**向下平移** offset；判 P 是否落在阴影里要把 P 往上移回去。
    // ⚠️ 衰减必须从**形状边缘向外**算：sdShadow=0 处最强、到 blur 距离衰减到 0。
    //    之前写的 smoothstep(-96, 0, sdShadow) 在形状外恒为 1 →
    //    等于把整屏形状外**均匀压暗 10%**，根本没有随距离衰减。
    float sdShadow = sdRoundedRect(centeredCoord + vec2(0.0, uShadowOffset), halfSize, radius);
    float shade = 1.0 - smoothstep(0.0, max(uShadowBlur, 1.0), sdShadow);
    vec3 outsideCol = sharp * (1.0 - shade * uShadowAlpha * uShadow);

    // ---- 形状内 ----
    float edge = max(0.0, -sd);                      // 距边缘的距离（AA 过渡带内钳到 0）

    // 提饱和 + 模糊后的底色
    vec3 body = saturate3(blurred, uVibrancy);

    vec3 result;
    if (edge >= uRefractionHeight) {
        result = body;
    } else {
        // ===== 折射 / 色散（与 SkSL 版同结构）=====
        float d = circleMap(1.0 - edge / uRefractionHeight) * uRefractionAmount;
        float gradRadius = min(radius * 1.5, min(halfSize.x, halfSize.y));
        // 退化保护：形状中轴 / 内矩形里的 SDF 差分是**零向量**，而 gradSdRoundedRect
        // 对零向量返回 (0,0) —— 外面再套一层 normalize 就是 normalize(vec2(0)) = NaN。
        // 折射高度调到 > 半高时（id 2 最大 48dp > 底板半高 30dp）中轴会落进折射带，
        // 于是底板正中会出现一个 NaN 像素（黑块/彩点）。这里对长度再检一次。
        vec2 g = gradSdRoundedRect(centeredCoord, halfSize, gradRadius);
        // depthEffect（上游 Shaders.kt:80）：在法线方向上再叠一层**径向**分量，
        // 做出"有厚度"的凸起折射。radial 在中轴处也要防零向量。
        if (uDepthEffect > 0.001) {
            g += uDepthEffect * centeredCoord / max(length(centeredCoord), 1e-4);
        }
        float gradLen = length(g);
        vec2 grad = gradLen > 1e-4 ? g / gradLen : vec2(0.0);

        vec2 refractedCoord = samplePx + d * grad;
        vec2 uv = refractedCoord / uResolution;

        // 透镜采样模糊后背景（上游管线 blur() -> lens()，透镜计算以模糊图像作为输入源），
        // 确保底板主体与折射边缘均具备一致的模糊与色散质感。
        if (uDispersion <= 0.0) {
            result = saturate3(texture(uBlurred, uv).rgb, uVibrancy);
        } else {
            float dispersionIntensity =
                ((centeredCoord.x * centeredCoord.y) / (halfSize.x * halfSize.y));
            vec2 dispersedCoord = (d * grad * dispersionIntensity) / uResolution;

            vec3 col = vec3(0.0);
            vec4 c;
            c = texture(uBlurred, uv + dispersedCoord);
            col.r += c.r / 3.5;
            c = texture(uBlurred, uv + dispersedCoord * (2.0 / 3.0));
            col.r += c.r / 3.5; col.g += c.g / 7.0;
            c = texture(uBlurred, uv + dispersedCoord * (1.0 / 3.0));
            col.r += c.r / 3.5; col.g += c.g / 3.5;
            c = texture(uBlurred, uv);
            col.g += c.g / 3.5;
            c = texture(uBlurred, uv - dispersedCoord * (1.0 / 3.0));
            col.g += c.g / 3.5; col.b += c.b / 3.0;
            c = texture(uBlurred, uv - dispersedCoord * (2.0 / 3.0));
            col.b += c.b / 3.0;
            c = texture(uBlurred, uv - dispersedCoord);
            col.r += c.r / 7.0; col.b += c.b / 3.0;
            result = saturate3(col, uVibrancy);
        }
    }

    // ---- 色彩控制（对齐上游 effects/ColorFilter.kt 的 colorControls）----
    // 放在表面填充之前：效果链上它作用在"采样到的背景"上。
    result = colorControls(result, uBrightness, uContrast, uSaturation);

    // ---- 表面填充（浅色偏透）：containerAlpha；tinted 按钮用 uSurfaceColor 换色 ----
    result = mix(result, uSurfaceColor, uSurfaceAlpha);

    // ---- 内阴影（顶部按钮用；底座 / 浮动件把 uInnerAlpha 传 0，等价于关）----
    // 上游 InnerShadow(radius = 按钮参数 18 × press, alpha = press)，颜色黑 alpha 0.15。
    if (uInnerAlpha > 0.001) {
        float inner = 1.0 - smoothstep(0.0, max(uInnerRadius, 1.0), edge);
        result *= (1.0 - inner * uInnerAlpha);
    }

    // ---- 棱边高光 ----
    // 上游沿形状**描边**：strokeWidth = ceil(width) * 2 且被形状裁掉外侧一半，
    // 所以可见的是一条**内宽 ≈ width 的环**，环内亮度 = |dot(法线, 45° 光向)| ^ falloff。
    // （之前把强度铺满整个形状，是"整条发白"的根因。）
    float dl = 0.0;
    if (uPlainHighlight > 0.5) {
        // 上游 Highlight.Plain：整块铺白（Plus 混合）。这里在形状内叠一层恒定白，
        // 不做描边环。默认 alpha 仍走 uHighlight。
        result += vec3(uHighlight);
    } else {
        float gradRadius = min(radius * 1.5, min(halfSize.x, halfSize.y));
        vec2 nrm = gradSdRoundedRect(centeredCoord, halfSize, gradRadius);
        dl = dot(nrm, vec2(0.70710678, 0.70710678));
        float ring = 1.0 - smoothstep(uHighlightWidth, uHighlightWidth + uHighlightBlur, edge);
        result += vec3(abs(dl) * uHighlight * ring);
    }

    // ---- 环境光（对照上游 AmbientHighlightShaderString）----
    //   同一个环，但取"受光那一半"（step(0, dot)），并且是**普通混合**不是加法。
    if (uAmbient > 0.5) {
        float ring = 1.0 - smoothstep(uHighlightWidth, uHighlightWidth + uHighlightBlur, edge);
        float t = step(0.0, dl) * abs(dl);
        result = mix(result, vec3(1.0), ring * t * uAmbientIntensity);
    }

    // ---- 效果不透明度（对齐上游 effects/ColorFilter.kt 的 opacity()）----
    // 直写模式：朝清晰背景回退 = 整个玻璃材质变淡（形状覆盖不受影响）；
    // 叠加模式（浮动件 / 按钮）：输出 alpha 也会乘一遍，见 uOverlayMode 分支。
    result = mix(sharp, result, uEffectOpacity);

    // ---- 页签文字：画在底板之上、指示器之下（指示器才能折射到它）----
    if (uShowLabels > 0.5) {
        // 位图行 0 在顶部，局部坐标 y 从底部起算 → v 要翻；
        // uv 钳到 [0,1]：AA 过渡带会采到形状外的 texel，不钳会出现一条细亮边
        vec2 labelUv = clamp(vec2(local.x / uLabelSize.x, 1.0 - local.y / uLabelSize.y), 0.0, 1.0);
        vec4 label = texture(uLabels, labelUv);
        result = mix(result, label.rgb, label.a);
    }

    // ---- 抗锯齿：屏幕空间导数得到约 1 物理像素的覆盖率过渡 ----
    float aa = max(fwidth(sd), 0.75);
    float coverage = 1.0 - smoothstep(-aa, aa, sd);

    if (uOverlayMode > 0.5) {
        // 叠加混合（浮动件）：形状内 = 玻璃色 + 覆盖率；形状外 = 只输出自己的投影
        // （黑色 + 阴影 alpha，靠 GL 的 src-alpha 混合压暗底下）
        float shAlpha = (1.0 - smoothstep(0.0, max(uShadowBlur, 1.0), sdShadow)) *
                        uShadowAlpha * uShadow;
        bool outside = sd > 0.0;
        outColor = vec4(outside ? vec3(0.0) : result + ditherNoise(gl_FragCoord.xy),
                        (outside ? shAlpha : coverage) * uEffectOpacity);
        return;
    }
    outColor = vec4(mix(outsideCol, result, coverage) + ditherNoise(gl_FragCoord.xy), 1.0);
}
)";


// ---------------------------------------------------------------------------
// pass 5：指示器。采样"背景 + 底板 + 文字"的合成结果，套同一套透镜（折射+色散）。
// 这就是参考图里最亮眼的细节：文字被透镜拉扯 + 分光。
// ---------------------------------------------------------------------------
constexpr char kIndicatorShaderBody[] = R"(
uniform sampler2D uScene;
uniform vec2 uResolution;
uniform vec4 uRect;              // 指示器矩形（屏幕坐标，y 从底部起算）
uniform vec4 uRadii;             // 四角半径（左上/右上/右下/左下）；胶囊 = 四值相等
uniform float uRefractionHeight;
uniform float uRefractionAmount;
uniform float uDispersion;
uniform float uDepthEffect;      // 0/1：折射方向叠加径向分量（上游 lens 的 depthEffect）
uniform float uTint;
uniform float uSurfaceWhite;  // 表面白填充（id 36 静止 / 46 拖动，按 press 插值）
uniform float uHighlight;
uniform float uHighlightWidth;
uniform float uHighlightBlur;
uniform float uShadowBlur;
uniform float uShadowOffset;
uniform float uShadowAlpha;
uniform float uScaleX;
uniform float uInnerRadius;   // 内阴影半径（px）＝ InnerShadow.radius
uniform float uInnerAlpha;    // 内阴影强度（已含颜色黑 alpha 0.15）
uniform float uSceneBlur;     // 指示器模糊半径（px）；0 = 关
uniform float uScaleY;
uniform float uInflate;       // 按压：每边向外膨胀的像素（等边距，不是绕中心等比缩放）
// 按压时**玻璃内部内容**的采样变换（与 progGlass_ 的同名 uniform 同式）：
//   samplePx = uSamplePivot + (px - uSamplePivot) / uSampleScale
// scale < 1 = 透镜缩微（形状扩张的同时内容往中心收）。
uniform vec2 uSampleScale;
uniform vec2 uSamplePivot;
// 页签文字/图标的两张位图（逐像素同布局，只有颜色不同）：
//   uLabels       = 常态色 —— 形状外按**屏幕位置**叠加，所以拖指示器时它们纹丝不动
//   uLabelsAccent = 凸显色 —— 形状内按**折射位置**叠加，于是"透过指示器看到的"才变色，
//                   并且跟指示器的透镜一起被拉扯/分光（参考实现的图层顺序）
// 关键：底板 pass 不再把文字画进场景，否则指示器的透镜会先采到常态字形，
// 再叠一层凸显色字形 = 两套文字重影。
uniform sampler2D uLabels;
uniform sampler2D uLabelsAccent;
uniform vec4 uLabelRect;        // 文字位图对应的底板矩形（屏幕坐标，y 从底部起算）
uniform vec2 uLabelSize;        // 文字位图尺寸（px）
uniform float uLabelSlots;      // 文字位图里有几个页签格
uniform float uShowTabLabels;   // 0/1
// 按压时凸显色文字的放大比例（参考实现 LocalLiquidBottomTabScale = lerp(1, 1.2, press)）。
// 缩放锚点是**各自页签格的中心**，不是胶囊中心 —— 胶囊滑动不会把字形带着走。
uniform float uLabelPressScale;
out vec4 outColor;

// 采样"背景 + 底板"的合成结果（指示器专有：用的是 uScene）
vec4 sampleScene(vec2 px) { return texture(uScene, px / uResolution); }

// 场景坐标 -> 文字位图局部坐标（相对底板左上角，y 从底部起算）
vec2 labelLocalAt(vec2 px) { return px - uLabelRect.xy; }

// 把局部坐标换算成位图 uv：行 0 在顶部，而局部 y 从底部起算，所以 v 要翻
vec2 labelUv(vec2 local) {
    return clamp(vec2(local.x / uLabelSize.x, 1.0 - local.y / uLabelSize.y), 0.0, 1.0);
}

// 按压放大：把局部坐标换算成"放大后位图"里的等价位置（绕所在页签格中心裁到 1+s 倍）
vec2 labelLocalMagnified(vec2 local) {
    if (uLabelPressScale < 0.0001 || uLabelSlots < 1.0) {
        return local;
    }
    float slot = uLabelSize.x / uLabelSlots;
    float index = clamp(floor(local.x / slot), 0.0, uLabelSlots - 1.0);
    vec2 center = vec2((index + 0.5) * slot, uLabelSize.y * 0.5);
    return center + (local - center) / (1.0 + uLabelPressScale);
}

// 位图是预乘 alpha：按 alpha 混合即"over"
vec3 overLabels(vec3 base, vec4 label) { return mix(base, label.rgb, label.a); }

// 指示器自己的模糊（参数 65/66）：3x3 圆盘，半径 = uSceneBlur（px，换算到 uv）。
// 采样对象是**场景纹理**，所以它模糊的是"这个位置真实的内容"；
// 若拿背景的模糊结果冒充，与底板重叠时就会错。
vec3 sampleSceneBlurredUV(vec2 uv) {
    if (uSceneBlur < 0.5) {
        return texture(uScene, uv).rgb;
    }
    vec2 st = vec2(uSceneBlur) / uResolution;
    vec3 acc = vec3(0.0);
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            acc += texture(uScene, uv + vec2(float(i), float(j)) * st).rgb;
        }
    }
    return acc / 9.0;
}

void main() {
    vec2 px = vUv * uResolution;
    // 采样变换只作用于形状**内部**：形状外仍是 1:1 的清晰场景（这趟是全屏三角形，
    // 形状外跟着缩放会把整屏都带歪）。pivot 取胶囊中心，scale = 1 时与原式逐像素等价。
    vec2 samplePx = uSamplePivot + (px - uSamplePivot) / uSampleScale;
    vec2 sampleUv = samplePx / uResolution;
    vec2 restHalf = uRect.zw * 0.5;
    vec2 center = uRect.xy + restHalf;
    // 按压 = **等边距膨胀**：每边向外多 uInflate 像素。不用绕中心等比缩放——
    // 胶囊宽(≈85dp) ≫ 高(42dp)，等比 1.5× 时横向每边多 21dp、竖向只多 10.5dp，
    // 四周永远不会等距。半径同步 +uInflate 后仍是胶囊（圆弧上是精确等距外扩）。
    vec2 halfSize = restHalf + uInflate;
    // 拖动速度的拉长/压扁仍走非等比缩放，与按压膨胀解耦。
    vec2 centeredCoord = (px - center) / vec2(uScaleX, uScaleY);
    // 四角半径：胶囊四值相等；半径同步 +uInflate 后仍是精确等距外扩。
    float radius = radiusAt(centeredCoord, uRadii) + uInflate;
    float sd = sdRoundedRect(centeredCoord, halfSize, radius);

    // 指示器投影：鼓出底座时靠它和底座边框"断开"，做出 iOS 那种空隙/分离感。
    float sdShadow = sdRoundedRect(centeredCoord + vec2(0.0, uShadowOffset),
                                   halfSize, radius);
    float shade = 1.0 - smoothstep(0.0, max(uShadowBlur, 1.0), sdShadow);
    // 形状外：清晰场景 + 常态色页签。页签按屏幕坐标采（不含透镜位移）——
    // 指示器滑到哪儿都不会带动它们；指示器自己的投影仍压在这一层之上。
    vec3 outsideScene = sampleScene(px).rgb;
    if (uShowTabLabels > 0.5) {
        outsideScene = overLabels(outsideScene, texture(uLabels, labelUv(labelLocalAt(px))));
    }
    vec3 outsideCol = outsideScene * (1.0 - shade * uShadowAlpha);

    float edge = max(0.0, -sd);
    vec3 result;
    // 凸显色页签的采样位置：**只跟透镜位移，不跟"按压采样缩小"**。
    // 缩小是绕胶囊中心的整体变换，带上它文字就会绕中心缩放 —— 胶囊一动，
    // 字形就跟着胶囊跑（参考实现的指示器没有这一层，文字不随指示器移动）。
    vec2 labelPx = px;
    if (edge >= uRefractionHeight) {
        result = sampleSceneBlurredUV(sampleUv);
    } else {
        float d = circleMap(1.0 - edge / uRefractionHeight) * uRefractionAmount;
        float gradRadius = min(radius * 1.5, min(halfSize.x, halfSize.y));
        // 退化保护：同底板 shader —— 中轴零梯度会让 normalize 出 NaN。
        vec2 g = gradSdRoundedRect(centeredCoord, halfSize, gradRadius);
        // depthEffect：与底板同式（上游 Shaders.kt:80）。
        if (uDepthEffect > 0.001) {
            g += uDepthEffect * centeredCoord / max(length(centeredCoord), 1e-4);
        }
        float gradLen = length(g);
        vec2 grad = gradLen > 1e-4 ? g / gradLen : vec2(0.0);
        vec2 refractedCoord = samplePx + d * grad;
        vec2 uv = refractedCoord / uResolution;
        labelPx = px + d * grad;

        if (uDispersion <= 0.0) {
            result = sampleSceneBlurredUV(uv);
        } else {
            float intensity = (centeredCoord.x * centeredCoord.y) / (halfSize.x * halfSize.y);
            vec2 dispersed = (d * grad * intensity) / uResolution;
            // 色散走单点采样（7 次 × 9-tap 模糊太贵）；模糊默认 0，实践中不冲突
            vec3 col = vec3(0.0);
            vec4 c;
            c = texture(uScene, uv + dispersed);
            col.r += c.r / 3.5;
            c = texture(uScene, uv + dispersed * (2.0 / 3.0));
            col.r += c.r / 3.5; col.g += c.g / 7.0;
            c = texture(uScene, uv + dispersed * (1.0 / 3.0));
            col.r += c.r / 3.5; col.g += c.g / 3.5;
            c = texture(uScene, uv);
            col.g += c.g / 3.5;
            c = texture(uScene, uv - dispersed * (1.0 / 3.0));
            col.g += c.g / 3.5; col.b += c.b / 3.0;
            c = texture(uScene, uv - dispersed * (2.0 / 3.0));
            col.b += c.b / 3.0;
            c = texture(uScene, uv - dispersed);
            col.r += c.r / 7.0; col.b += c.b / 3.0;
            result = col;
        }
    }

    // ---- 凸显色：指示器范围内的页签文字/图标换成强调色 ----
    // 位置用 labelPx（透镜位移，但不含按压采样缩小）→ 与形状外的常态字形在同一位置，
    // 所以静止时逐像素重合、只换了颜色；拖动时它只是被透镜的折射带轻微拉扯，
    // 不会跟着胶囊整体平移。形状外 result 会被 coverage 丢掉 —— 盖住半个字就只有半个字变色。
    // 按压时再绕**页签格中心**放大 uLabelPressScale（参考实现的 LocalLiquidBottomTabScale）：
    // 只有这一层（透过指示器看到的文字）会变大，底座里的常态文字不变。
    if (uShowTabLabels > 0.5) {
        vec4 accent = texture(uLabelsAccent, labelUv(labelLocalMagnified(labelLocalAt(labelPx))));
        result = overLabels(result, accent);
    }

    // 选中态底色：onDrawSurface 里的 black alpha = lerp(tintRest 0.2, tintDrag 0, press)
    result *= (1.0 - uTint);
    // 表面白：同一层 onDrawSurface 上再叠一层白（id 36 / 46）。
    // 上游是 drawRect(白 alpha=lerp(rest, drag, press))，普通混合 → mix 即等价。
    result = mix(result, vec3(1.0), uSurfaceWhite);

    // 内阴影（上游 InnerShadow(radius = lerp(2dp, 8dp, press), alpha = lerp(0.14, 1.0, press))，
    // 颜色黑 alpha 0.15）：靠近内边缘压一圈暗，做出厚度感。
    // 上游 offset = (0, radius) 偏下 → 用一点"下重上轻"近似。
    if (uInnerAlpha > 0.001) {
        float inner = 1.0 - smoothstep(0.0, max(uInnerRadius, 1.0), edge);
        float lowerBias = clamp(0.65 - centeredCoord.y / max(halfSize.y, 1.0) * 0.35, 0.0, 1.0);
        result *= (1.0 - inner * uInnerAlpha * lowerBias);
    }

    // 棱边高光：与底板**逐字同一套**（DefaultHighlight 移植）—— 细描边环。
    // 之前这里用的是遗留的"从棱边向内 max(8px, 短半轴*0.35) 的粗渐变遮罩"，
    // 所以指示器的高光又粗又靠内，和底板的细亮边对不上。改为与底板相同的
    // `1 - smoothstep(环宽, 环宽+柔化, edge)`。（法线已用中央差分，本身平滑，
    // 不再需要那层遮罩压楔形。）
    {
        float gradRadius = min(radius * 1.5, min(halfSize.x, halfSize.y));
        vec2 nrm = gradSdRoundedRect(centeredCoord, halfSize, gradRadius);
        float dl = dot(nrm, vec2(0.70710678, 0.70710678));
        float ring = 1.0 - smoothstep(uHighlightWidth, uHighlightWidth + uHighlightBlur, edge);
        result += vec3(abs(dl) * uHighlight * ring);
    }

    // ---- 抗锯齿（同底板）----
    float aa = max(fwidth(sd), 0.75);
    float coverage = 1.0 - smoothstep(-aa, aa, sd);
    outColor = vec4(mix(outsideCol, result, coverage), 1.0);
}
)";

// ---------------------------------------------------------------------------
// pass 4b：跟手光斑（对齐参考实现的 InteractiveHighlight）
//
// 上游是**独立图层 + BlendMode.Plus**：一层铺满玻璃的白（glowFlat）+ 一层以手指为中心
// 的径向白（glowRadial，半径 = 短边 × glowRadiusFactor），两者都乘以按压进度。
// 这里做成同一个 scene FBO 上的加性 pass —— 加性混合与"独立图层 + Plus"数学等价，
// 而且因为它坐在 sceneTex_ 里，指示器的透镜会把光斑一起折射（上游也是这个相对顺序）。
// ---------------------------------------------------------------------------
constexpr char kGlowShaderBody[] = R"(
uniform vec2 uResolution;
uniform vec4 uGlassRect;      // xy = 左下角（GL 坐标），zw = 尺寸
uniform vec4 uRadii;          // 四角半径；光斑跟底板的形状走
uniform float uProgress;      // 0..1，独立弹簧（参数 31/32）
uniform float uFlat;          // id 6
uniform float uRadial;        // id 7
uniform float uRadiusFactor;  // id 8
uniform vec2 uCenter;         // 光斑中心（屏幕 px，GL 坐标）
out vec4 outColor;

void main() {
    vec2 px = vUv * uResolution;
    vec2 halfSize = uGlassRect.zw * 0.5;
    vec2 centered = px - uGlassRect.xy - halfSize;
    float sd = sdRoundedRect(centered, halfSize, radiusAt(centered, uRadii));
    // 光斑被玻璃形状裁掉（上游 drawWithContent 画在自己的 layer 里，同样被形状裁）
    float aa = max(fwidth(sd), 0.75);
    float coverage = 1.0 - smoothstep(-aa, aa, sd);
    if (coverage <= 0.0) {
        outColor = vec4(0.0);
        return;
    }
    float radius = min(uGlassRect.z, uGlassRect.w) * uRadiusFactor;
    float intensity = smoothstep(radius, radius * 0.5, distance(px, uCenter));
    // 混合函数是 (ONE, ONE)，所以这里输出的是"要加多少"，alpha 不参与
    outColor = vec4(vec3((uFlat + uRadial * intensity) * uProgress * coverage), 0.0);
}
)";

// ---------------------------------------------------------------------------
// pass 7：顶部三个玻璃按钮的图标（采样 iconTex_，三格横向排列）
// ---------------------------------------------------------------------------
constexpr char kIconShaderBody[] = R"(
uniform vec2 uResolution;
uniform vec2 uFirstRect;     // 最左按钮的左下角（GL 坐标）
uniform float uBtnSize;
uniform float uSpacing;
uniform float uIconSize;
uniform sampler2D uIcons;    // 3 格横向排列、预乘 alpha 的白色图标
out vec4 outColor;

void main() {
    vec2 px = vUv * uResolution;
    for (int i = 0; i < 3; i++) {
        vec2 base = uFirstRect + vec2(float(i) * (uBtnSize + uSpacing), 0.0);
        vec2 iconBase = base + vec2((uBtnSize - uIconSize) * 0.5);
        vec2 local = px - iconBase;
        if (local.x < 0.0 || local.x > uIconSize || local.y < 0.0 || local.y > uIconSize) {
            continue;
        }
        // 位图行 0 在顶部、GL 的 y 从底部起算（同文字纹理的翻转）
        vec2 uv = vec2((local.x / uIconSize + float(i)) / 3.0, 1.0 - local.y / uIconSize);
        outColor = texture(uIcons, uv);   // 预乘：混合函数用 (ONE, ONE_MINUS_SRC_ALPHA)
        return;
    }
    outColor = vec4(0.0);
}
)";

// ---------------------------------------------------------------------------
// 目录场景 pass：纯色圆角矩形（轨道 / 填充 / 卡片 / 对话框按钮底色）
// ---------------------------------------------------------------------------
constexpr char kSolidShaderBody[] = R"(
uniform vec2 uResolution;
uniform vec4 uRect;        // xy = 左下角（GL 坐标），zw = 尺寸
uniform vec4 uRadii;       // 左上/右上/右下/左下
uniform vec4 uColor;       // 非预乘
out vec4 outColor;

void main() {
    vec2 px = vUv * uResolution;
    vec2 halfSize = uRect.zw * 0.5;
    vec2 centered = px - uRect.xy - halfSize;
    float radius = radiusAt(centered, uRadii);
    float sd = sdRoundedRect(centered, halfSize, radius);
    float aa = max(fwidth(sd), 0.75);
    float coverage = 1.0 - smoothstep(-aa, aa, sd);
    outColor = vec4(uColor.rgb, uColor.a * coverage);
}
)";

// ---------------------------------------------------------------------------
// 目录场景 pass：背景 -> sceneTex_ 的拷贝（不带模糊）
// ---------------------------------------------------------------------------
constexpr char kCopyShaderBody[] = R"(
uniform sampler2D uSource;
out vec4 outColor;
void main() {
    outColor = vec4(texture(uSource, vUv).rgb + ditherNoise(gl_FragCoord.xy), 1.0);
}
)";

// ---------------------------------------------------------------------------
// 目录场景 pass：文字 / 图标位图（预乘 alpha，位图行 0 在顶部）
// ---------------------------------------------------------------------------
constexpr char kSpriteShaderBody[] = R"(
uniform vec2 uResolution;
uniform sampler2D uSprite;
uniform vec4 uRect;        // xy = 左下角（GL 坐标），zw = 尺寸
uniform float uAlpha;      // 整体不透明度（内容层的淡入淡出）
out vec4 outColor;
void main() {
    vec2 px = vUv * uResolution;
    vec2 local = (px - uRect.xy) / uRect.zw;
    if (local.x < 0.0 || local.x > 1.0 || local.y < 0.0 || local.y > 1.0) {
        outColor = vec4(0.0);
        return;
    }
    // GL 的 vUv.y = 0 在屏幕底部；位图行 0 在顶部 → 翻 y
    outColor = texture(uSprite, vec2(local.x, 1.0 - local.y)) * uAlpha;
}
)";

// ---------------------------------------------------------------------------
// 渐进模糊（对齐上游 ProgressiveBlurContent 的 AlphaMask runtime shader）：
// 上半个band完全模糊、向下淡出；同时叠一层 tint（纵向同样渐变）。
// ---------------------------------------------------------------------------
constexpr char kProgressiveBlurShaderBody[] = R"(
uniform vec2 uResolution;
uniform sampler2D uSharp;
uniform sampler2D uBlurred;
uniform vec4 uRect;        // xy = 左下角（GL 坐标），zw = 尺寸
uniform vec3 uTint;
uniform float uTintIntensity;
out vec4 outColor;

void main() {
    vec2 px = vUv * uResolution;
    vec2 halfSize = uRect.zw * 0.5;
    vec2 centered = px - uRect.xy - halfSize;
    // 全圆角矩形（band 无圆角）
    float sd = sdRoundedRect(centered, halfSize, 0.0);
    float aa = max(fwidth(sd), 0.75);
    float coverage = 1.0 - smoothstep(-aa, aa, sd);
    if (coverage <= 0.0) {
        outColor = vec4(0.0);
        return;
    }
    // coord.y 按上游是**自上而下**：0 = band 顶部，size.y = 底部
    float y = (uRect.y + uRect.w) - px.y;
    float blurAlpha = smoothstep(uRect.w, uRect.w * 0.5, y);
    vec3 sharp = texture(uSharp, vUv).rgb;
    vec3 blurred = texture(uBlurred, vUv).rgb;
    vec3 col = mix(sharp, blurred, blurAlpha);
    col = mix(col, uTint, blurAlpha * uTintIntensity);
    outColor = vec4(col + ditherNoise(gl_FragCoord.xy), coverage);
}
)";


GLuint CompileShader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[2048] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        GERR("shader compile failed: %{public}s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint LinkProgram(const char *vs, const char *fs) {
    GLuint v = CompileShader(GL_VERTEX_SHADER, vs);
    GLuint f = CompileShader(GL_FRAGMENT_SHADER, fs);
    if (v == 0 || f == 0) {
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        GERR("program link failed: %{public}s", log);
        glDeleteProgram(prog);
        prog = 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return prog;
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void SetupTexture(GLuint tex) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/**
 * 把裁剪盒设成"某个矩形外扩 margin"（GL 坐标，y 从**底部**起算，与 glScissor 一致）。
 *
 * 为什么要它：玻璃 shader 是**全屏三角形 + 逐像素 SDF**，形状只占屏幕很小一块。
 * 新增的按钮 / 光斑 / 按钮模糊如果各来一遍全屏，帧率立刻掉到面板刷新率以下
 * （真机实测：1280×2598、60Hz 面板，从 60.2fps 掉到 54.1fps）。
 * 这些 pass 的输出只在给定矩形内非零，所以用 scissor 裁掉盒外像素**完全等价**，
 * 而 fragment 成本降到千分之几。
 *
 * 调用方负责 `glEnable/glDisable(GL_SCISSOR_TEST)`。
 */
void SetScissorRect(float left, float bottom, float width, float height, float margin, int screenW,
                    int screenH) {
    int x0 = static_cast<int>(std::floor(left - margin));
    int y0 = static_cast<int>(std::floor(bottom - margin));
    int x1 = static_cast<int>(std::ceil(left + width + margin));
    int y1 = static_cast<int>(std::ceil(bottom + height + margin));
    x0 = std::max(0, std::min(x0, screenW));
    y0 = std::max(0, std::min(y0, screenH));
    x1 = std::max(0, std::min(x1, screenW));
    y1 = std::max(0, std::min(y1, screenH));
    if (x1 <= x0 || y1 <= y0) {
        glScissor(0, 0, 0, 0);   // 空盒 = 这一段什么都不画
        return;
    }
    glScissor(x0, y0, x1 - x0, y1 - y0);
}

// ---------------------------------------------------------------------------
// 顶部三个玻璃按钮的布局常量（对齐参考实现的 Row + GlassIconButton）
// ---------------------------------------------------------------------------
constexpr float kIconButtonSpacingDp = 12.0f;  // Arrangement.spacedBy(12.dp)
constexpr float kIconButtonPaddingH = 20.0f;   // Row padding(end = 20.dp)
constexpr float kIconButtonPaddingV = 20.0f;   // Row padding(top = 20.dp)
constexpr float kIconButtonIconDp = 19.0f;     // 图标 Canvas 的 size(19.dp)

// 主模糊链的降采样阈值（σ，px）：σ 小走全分辨率多趟，大 σ 先降采样再叠核。
// level 0 = 1/2 分辨率（1 texel = 2 px），level 1 = 1/4（1 texel = 4 px）。
constexpr float kBlurPyramidHalfMinPx = 8.0f;
constexpr float kBlurPyramidQuarterMinPx = 48.0f;

// ---------------------------------------------------------------------------
// 目录场景（catalog）通用常量
// ---------------------------------------------------------------------------

/** 飞机图标（控制中心 / 游乐场）。24×24 视口，只含绝对 M/L/Q/Z（与页签图标同一解析器）。 */
constexpr char kPlaneIconPath[] =
    "M12 2 Q12.85 2 12.85 3.1 L12.85 9.3 L21 14 L21 16.1 L12.85 13.6 L12.85 18.4 "
    "L15.5 20.3 L15.5 22 L12 20.9 L8.5 22 L8.5 20.3 L11.15 18.4 L11.15 13.6 "
    "L3 16.1 L3 14 L11.15 9.3 L11.15 3.1 Q11.15 2 12 2 Z";

constexpr float kPlaneIconDp = 24.0f;

// 目录场景常用颜色（对齐上游 dark theme 值）
constexpr float kAccentR = 0.0f;
constexpr float kAccentG = 0.5686f;   // 0x91 / 255
constexpr float kAccentB = 1.0f;      // #0091FF
constexpr float kTrackR = 0.4706f;    // #787880
constexpr float kTrackG = 0.4706f;
constexpr float kTrackB = 0.5020f;
constexpr float kTrackAlpha = 0.36f;

// 按压时**玻璃内部内容**的采样缩小比例（采样缩放 = 1 - k × press，绕形状中心）。
// 与形状的按压缩放（scale > 1）方向相反：形状往外长、内容往回收，才有透镜被按下去的感觉。
// 旋钮（开关 / 滑块）与顶部玻璃按钮共用这个量级；页签指示器走参数 81（应用内可调）。
constexpr float kKnobPressSampleShrink = 0.08f;
constexpr float kButtonPressSampleShrink = 0.08f;

/**
 * Highlight.Plain 的强度。上游是「白 0.38 + Plus（整块加白）」。
 *
 * ⚠️ 这里按视觉配平降到 0.18：Plain 是**常量加法**，而暗色对话框的素材在采样前
 * 已经被 dim 压到 ~0.44×、再经模糊只剩 ~0.05-0.1 的起伏 —— 常量 0.38 会占掉面板
 * 亮度的绝大部分，等于把材质抬成一块没有层次的死灰（发灰的主因）。
 * 降到 0.18 后背景的明暗/色相仍然可读，同时保留 Plain "整块提亮"的语义。
 */
constexpr float kPlainHighlight = 0.18f;

/** 菜单目标编号：：0=底座，1=指示器·静止，2=指示器·拖动（0=底座，1=指示器·静止，2=指示器·拖动） */
constexpr int kIconButtonMenuTargets[3] = {2, 1, 0};

/**
 * 本帧所有弹簧的位移累积（只由渲染线程读写）。
 *
 * 用途：判定"动画是不是停了" —— 低于阈值就跳过绘制（见 RenderLoop 的空闲分支）。
 * 之所以累积**位移**而不是速度：各弹簧的量纲不同（页数 / 0..1 进度 / px / %），
 * 位移是唯一能直接和"肉眼可见的移动量"挂钩的指标。
 */
float gSpringMotion = 0.0f;

// 帧率节流参数：交互中 120fps，其余 60fps；触摸后 400ms 内算"交互中"。
constexpr int64_t kInteractiveFrameMs = 8;
constexpr int64_t kNormalFrameMs = 16;
constexpr int64_t kInteractiveWindowMs = 400;
// 弹簧位移累积低于此值就认为动画停了（单位：值/帧，1e-3 已经远低于可见阈值）。
constexpr float kIdleMotionEps = 1e-3f;

/**
 * 半隐式欧拉步进一个弹簧（本文件所有弹簧共用同一套公式：c = 2√k × ratio）。
 *
 * 与参考实现的 `spring(dampingRatio, stiffness, visibilityThreshold)` 同构；
 * 这里不做 visibilityThreshold 的提前收敛（各自的调用点按需加死区），
 * 但会把本帧位移累加到 `gSpringMotion`（空闲检测用）。
 */
void SpringStep(float &value, float &velocity, float target, float stiffness, float damping,
                float dt) {
    const float k = std::max(stiffness, 0.0f);
    if (k <= 0.0f) {
        velocity = 0.0f;   // 刚度 0 = 不动（不产生 NaN）
        return;
    }
    const float c = 2.0f * std::sqrt(k) * std::max(damping, 0.0f);
    velocity += (-k * (value - target) - c * velocity) * dt;
    const float delta = velocity * dt;
    value += delta;
    gSpringMotion += std::fabs(delta);
}

}  // namespace


// ---------------------------------------------------------------------------
// 页签图标：与参考实现的 TabIconPaths 完全相同（24×24 视口，绝对 M/L/Q/Z）
// ---------------------------------------------------------------------------
const std::vector<std::string> kTabIconPaths = {
    // house
    R"(M19.99 12Q19.68 12 19.46 12.23Q19.25 12.46 19.25 12.77L19.25 19.99Q19.25 20.3 19.02 20.52Q18.79 20.74 18.48 20.74L15.72 20.74L15.72 16.44Q15.72 15.43 15.22 14.58Q14.71 13.73 13.86 13.22Q13.01 12.72 12 12.72Q10.97 12.72 10.12 13.22Q9.26 13.73 8.76 14.58Q8.26 15.43 8.26 16.44L8.26 20.74L5.47 20.74Q5.18 20.74 4.96 20.51Q4.73 20.28 4.73 19.97L4.73 12.77Q4.73 12.46 4.51 12.23Q4.3 12 3.98 12Q3.67 12 3.46 12.23Q3.24 12.46 3.24 12.77L3.24 19.97Q3.24 20.59 3.54 21.11Q3.84 21.62 4.36 21.92Q4.87 22.22 5.47 22.22L18.48 22.22Q19.08 22.22 19.6 21.92Q20.11 21.62 20.42 21.11Q20.74 20.59 20.74 19.99L20.74 12.77Q20.74 12.46 20.52 12.23Q20.3 12 19.99 12ZM9.77 16.44Q9.77 15.53 10.43 14.87Q11.09 14.21 12 14.21Q12.91 14.21 13.57 14.87Q14.23 15.53 14.23 16.44L14.23 20.74L9.77 20.74L9.77 16.44ZM22.54 11.45Q22.73 11.23 22.73 10.92Q22.73 10.61 22.51 10.39L14.45 2.76Q13.78 2.11 12.89 1.88Q12 1.66 11.11 1.88Q10.22 2.11 9.55 2.76L1.51 10.39Q1.27 10.61 1.27 10.92Q1.27 11.23 1.49 11.47Q1.68 11.66 2.02 11.68Q2.35 11.69 2.54 11.5L10.61 3.84Q11.21 3.26 12.01 3.26Q12.82 3.26 13.42 3.84L21.48 11.47Q21.7 11.69 22.01 11.69Q22.32 11.69 22.54 11.45Z)",
    // magnifyingglass
    R"(M17.18 16.08Q18.1 14.95 18.61 13.56Q19.13 12.17 19.13 10.68Q19.13 8.38 17.98 6.42Q16.82 4.46 14.87 3.32Q12.91 2.18 10.61 2.18Q8.3 2.18 6.35 3.32Q4.39 4.46 3.25 6.42Q2.11 8.38 2.11 10.68Q2.11 12.98 3.25 14.94Q4.39 16.9 6.35 18.04Q8.3 19.18 10.61 19.18Q12.17 19.18 13.57 18.65Q14.98 18.12 16.13 17.16L20.59 21.6Q20.81 21.82 21.12 21.82Q21.43 21.82 21.65 21.6Q21.86 21.38 21.86 21.07Q21.86 20.76 21.65 20.54L17.18 16.08ZM3.62 10.68Q3.62 8.78 4.57 7.18Q5.52 5.57 7.12 4.62Q8.71 3.67 10.61 3.67Q12.5 3.67 14.11 4.62Q15.72 5.57 16.67 7.18Q17.62 8.78 17.62 10.68Q17.62 12.58 16.67 14.18Q15.72 15.79 14.11 16.74Q12.5 17.69 10.61 17.69Q8.71 17.69 7.12 16.74Q5.52 15.79 4.57 14.18Q3.62 12.58 3.62 10.68Z)",
    // person
    R"(M12 10.97Q13.27 10.97 14.34 10.33Q15.41 9.7 16.04 8.62Q16.68 7.54 16.68 6.26Q16.68 4.99 16.04 3.91Q15.41 2.83 14.34 2.2Q13.27 1.56 12 1.56Q10.7 1.56 9.62 2.2Q8.54 2.83 7.92 3.91Q7.3 4.99 7.3 6.26Q7.3 7.54 7.92 8.62Q8.54 9.7 9.62 10.33Q10.7 10.97 12 10.97ZM12 3.05Q12.86 3.05 13.6 3.48Q14.33 3.91 14.76 4.66Q15.19 5.4 15.19 6.26Q15.19 7.13 14.76 7.86Q14.33 8.59 13.6 9.02Q12.86 9.46 12 9.46Q11.14 9.46 10.39 9.02Q9.65 8.59 9.22 7.86Q8.78 7.13 8.78 6.26Q8.78 5.4 9.22 4.66Q9.65 3.91 10.39 3.48Q11.14 3.05 12 3.05ZM18.74 22.06Q19.42 22.06 19.97 21.73Q20.52 21.41 20.84 20.86Q21.17 20.3 21.17 19.63Q21.17 17.59 19.92 15.88Q18.67 14.16 16.73 13.16Q14.78 12.17 12.79 12.17L11.18 12.17Q9.19 12.17 7.25 13.16Q5.3 14.16 4.07 15.88Q2.83 17.59 2.83 19.63Q2.83 20.3 3.16 20.86Q3.48 21.41 4.03 21.73Q4.58 22.06 5.23 22.06L18.74 22.06ZM12.79 13.68Q14.4 13.68 16 14.48Q17.59 15.29 18.62 16.66Q19.66 18.02 19.66 19.63Q19.66 20.02 19.39 20.29Q19.13 20.57 18.74 20.57L5.26 20.57Q4.87 20.57 4.6 20.29Q4.32 20.02 4.32 19.63Q4.32 18.02 5.35 16.66Q6.38 15.29 7.99 14.48Q9.6 13.68 11.21 13.68L12.79 13.68Z)",
};

// 页签内容颜色（对齐参考实现的 TabContentSelected / TabContentUnselected）
constexpr uint32_t kTabSelectedRgb = 0x00007DFF;    // #007DFF
constexpr uint32_t kTabUnselectedRgb = 0x00000000;  // 黑
constexpr float kTabIconSizeDp = 20.0f;             // TabIconSize
constexpr float kTabIconGapDp = 2.0f;               // TabIconLabelGap
constexpr float kTabIconBoldStroke = 0.8f;          // TabIconBoldStroke（24 视口单位）

GlesRenderer &GlesRenderer::Instance() {
    static GlesRenderer instance;
    return instance;
}

void GlesRenderer::OnSurfaceCreated(void *nativeWindow, uint64_t width, uint64_t height) {
    OnSurfaceDestroyed();

    width_.store(width);
    height_.store(height);
    stopRequested_.store(false);
    running_.store(true);
    forceRedraw_.store(true);
    lastDrawMs_ = 0;
    thread_ = std::thread(&GlesRenderer::RenderLoop, this, nativeWindow);
    GLOG("surface created: %{public}llu x %{public}llu, render thread started",
         static_cast<unsigned long long>(width), static_cast<unsigned long long>(height));
}

void GlesRenderer::OnSurfaceChanged(uint64_t width, uint64_t height) {
    width_.store(width);
    height_.store(height);
}

void GlesRenderer::OnSurfaceDestroyed() {
    if (thread_.joinable()) {
        stopRequested_.store(true);
        thread_.join();
    }
    running_.store(false);
}

void GlesRenderer::SetFloat(int id, float value) {
    params_.SetNumber(id, value);
    forceRedraw_.store(true);   // 参数发生更新，触发重绘

    // 文字位图由 UploadLabels() 栅格化生成（涉及字号、透明度、边距与底板尺寸等参数），
    // 当以下排版相关参数变更时，需要重建文字纹理以使效果实时生效。
    switch (id) {
        case pid::kLabelFontSize:
        case pid::kLabelSelectedAlpha:
        case pid::kLabelUnselectedAlpha:
        case pid::kOuterPaddingH:
        case pid::kBarHeight:
        case pid::kInnerPadding:
        case pid::kIndicatorExpand:
        case pid::kIndicatorHeight:
            labelsDirty_.store(true);
            break;
        default:
            break;
    }
}

void GlesRenderer::SetBool(int id, bool value) {
    params_.SetBool(id, value);
    forceRedraw_.store(true);
}

void GlesRenderer::SetPhotoData(const void *data, size_t length) {
    if (data == nullptr || length < 8) {
        GERR("SetPhotoData: bad input (len=%{public}zu)", length);
        return;
    }
    const auto *bytes = static_cast<const unsigned char *>(data);
    uint32_t w = 0;
    uint32_t h = 0;
    memcpy(&w, bytes, 4);
    memcpy(&h, bytes + 4, 4);
    const size_t expected = 8 + static_cast<size_t>(w) * h * 4;
    if (w == 0 || h == 0 || length < expected) {
        GERR("SetPhotoData: size mismatch %{public}u x %{public}u, len=%{public}zu",
             w, h, length);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(photoMutex_);
        photoPixels_.assign(bytes + 8, bytes + expected);
        photoW_ = static_cast<int>(w);
        photoH_ = static_cast<int>(h);
    }
    photoDirty_.store(true);
    forceRedraw_.store(true);
    GLOG("SetPhotoData: %{public}u x %{public}u, %{public}.1f MB", w, h,
         static_cast<double>(expected) / 1048576.0);
}

std::string GlesRenderer::ContentSurfaceId() const {
    std::lock_guard<std::mutex> lock(contentMutex_);
    return contentSurfaceId_;
}

void GlesRenderer::SetDebugMode(int mode) {
    debugMode_.store(mode);
    forceRedraw_.store(true);
}

void GlesRenderer::SetPaused(bool paused) {
    paused_.store(paused);
    // 恢复时补画一帧：暂停期间可能改过参数/场景
    forceRedraw_.store(true);
}

void GlesRenderer::PaceFrame() {
    const int64_t now = NowMs();
    const bool interacting = (now - lastTouchMs_.load()) < kInteractiveWindowMs;
    const int64_t intervalMs = interacting ? kInteractiveFrameMs : kNormalFrameMs;
    if (lastDrawMs_ != 0) {
        const int64_t due = lastDrawMs_ + intervalMs;
        if (now < due) {
            std::this_thread::sleep_for(std::chrono::milliseconds(due - now));
        }
    }
    lastDrawMs_ = NowMs();   // 用睡醒后的真实时刻，避免误差累积
}

void GlesRenderer::SetScene(int scene) {
    if (scene < 0 || scene >= static_cast<int>(Scene::kCount)) {
        GERR("SetScene: id %{public}d out of range", scene);
        return;
    }
    const int previous = scene_.exchange(scene);
    if (previous == scene) {
        return;
    }
    ResetCatalogState(scene);
    forceRedraw_.store(true);
    GLOG("scene changed: %{public}d -> %{public}d", previous, scene);
}

void GlesRenderer::ResetCatalogState(int scene) {
    std::lock_guard<std::mutex> lock(animMutex_);
    for (int i = 0; i < kScenePressMax; i++) {
        scenePress_[i] = 0.0f;
        scenePressVel_[i] = 0.0f;
    }
    sceneDown_ = -1;

    toggleFraction_ = toggleOn_ ? 1.0f : 0.0f;
    toggleFractionVel_ = 0.0f;
    togglePress_ = 0.0f;
    togglePressVel_ = 0.0f;
    toggleDragging_ = false;
    toggleDidDrag_ = false;

    sliderPress_ = 0.0f;
    sliderPressVel_ = 0.0f;
    sliderDragging_ = false;
    sliderDidDrag_ = false;
    sliderValueAnim_ = sliderValue_;
    sliderValueAnimVel_ = 0.0f;
    sliderValueAnimLast_ = sliderValue_;
    sliderVelocity_ = 0.0f;
    sliderVelocityVel_ = 0.0f;

    dialogPress_[0] = dialogPress_[1] = 0.0f;
    dialogPressVel_[0] = dialogPressVel_[1] = 0.0f;
    dialogDown_ = -1;

    ccProgress_ = 1.0f;
    ccProgressVel_ = 0.0f;
    ccDragging_ = false;

    lockOffsetX_ = 0.0f;
    lockOffsetY_ = 0.0f;
    lockDragging_ = false;

    magInited_ = false;
    magDragging_ = false;

    listScroll_ = 0.0f;
    listScrollVel_ = 0.0f;
    listDragging_ = false;

    pgInited_ = false;
    pgDragging_ = false;
    pgSheetOpen_ = true;
    pgSliderDown_ = -1;
    const float sliderDefaults[5] = {0.5f, 0.0f, 0.2f, 0.2f, 0.0f};
    for (int i = 0; i < 5; i++) {
        pgSliderValue_[i] = sliderDefaults[i];
        pgSliderPress_[i] = 0.0f;
        pgSliderPressVel_[i] = 0.0f;
    }
    pgButtonDown_ = -1;
    for (int i = 0; i < 2; i++) {
        pgButtonPress_[i] = 0.0f;
        pgButtonPressVel_[i] = 0.0f;
    }

    lumValue_ = 0.25f;
    lumTarget_ = 0.25f;
    lumVel_ = 0.0f;
    lumSampleMs_ = 0;
    lumShown_ = -1;
    lumTextMs_ = 0;

    contentTextDirty_.store(true);
    overlayTextDirty_.store(true);
    GLOG("catalog state reset for scene %{public}d", scene);
}

// ---------------------------------------------------------------------------
// 指示器动画（S3）
// ---------------------------------------------------------------------------

float GlesRenderer::IndicatorEndInsetPx() const {
    // 参考实现: capInsetDp = ((panel.barHeight - indicator.indicatorHeight) / 2).coerceAtLeast(0)
    return std::max(0.0f, (params_.Px(pid::kBarHeight) - params_.Px(pid::kIndicatorHeight)) * 0.5f);
}

float GlesRenderer::TabInnerPadPx() const {
    // 参考实现: innerPaddingDp = max(panel.panelInnerPadding, capInsetDp + indicator.indicatorExpand)
    return std::max(params_.Px(pid::kInnerPadding),
                    IndicatorEndInsetPx() + params_.Px(pid::kIndicatorExpand));
}

/** 触摸 x -> 连续页签位置 0..2（屏幕坐标，与 GL 同一套 x） */
float GlesRenderer::TabValueFromX(float x, int surfaceW) const {
    const float barW = static_cast<float>(surfaceW) - params_.Px(pid::kOuterPaddingH) * 2.0f;
    const float innerPad = TabInnerPadPx();
    const float tabW = (barW - innerPad * 2.0f) / 3.0f;
    // tabW 可能被极端参数（外边距拉满 + 内缩拉满）压到 0 甚至负数，
    // 那时候这一帧不做位移，避免除零产生 NaN/Inf 污染 anim_。
    if (tabW < 1.0f) {
        return 1.0f;  // 0..2 的正中（页签数 3）；不改变指示器位置
    }
    const float local = x - (params_.Px(pid::kOuterPaddingH) + innerPad);
    return local / tabW - 0.5f;
}

// ---------------------------------------------------------------------------
// 顶部三个玻璃按钮（几何）
// ---------------------------------------------------------------------------

float GlesRenderer::ButtonSizePx() const {
    return params_.Px(pid::kIconButtonSize);
}

float GlesRenderer::ButtonSpacingPx() const {
    return kIconButtonSpacingDp * params_.Density();
}

void GlesRenderer::ButtonRectPx(int index, int surfaceW, int surfaceH, float *left,
                                float *bottom) const {
    const float size = ButtonSizePx();
    const float spacing = ButtonSpacingPx();
    const float padH = kIconButtonPaddingH * params_.Density();
    const float padV = kIconButtonPaddingV * params_.Density();
    const float w = static_cast<float>(surfaceW);
    const float h = static_cast<float>(surfaceH);
    // 从右往左排：index 2（更多）贴右上角，index 0（拖动）在最左
    const int fromRight = kIconButtonCount - 1 - index;
    if (left != nullptr) {
        *left = w - padH - static_cast<float>(fromRight) * (size + spacing) - size;
    }
    if (bottom != nullptr) {
        *bottom = h - padV - size;   // GL 坐标：y 从底部起算
    }
}

bool GlesRenderer::HitButton(float touchX, float touchY, int surfaceW, int surfaceH,
                             int *index) const {
    const float size = ButtonSizePx();
    if (size <= 0.0f) {
        return false;
    }
    const float glY = static_cast<float>(surfaceH) - touchY;   // 触摸 y 从顶部起算
    for (int i = 0; i < kIconButtonCount; i++) {
        float left = 0.0f;
        float bottom = 0.0f;
        ButtonRectPx(i, surfaceW, surfaceH, &left, &bottom);
        if (touchX >= left && touchX <= left + size && glY >= bottom && glY <= bottom + size) {
            if (index != nullptr) {
                *index = i;
            }
            return true;
        }
    }
    return false;
}

void GlesRenderer::UpdateAnim(float dt) {
    if (dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;   // 夹一下，避免掉帧时弹簧炸掉

    std::lock_guard<std::mutex> lock(animMutex_);

    // ---- value：弹簧追赶目标（拖动 = 触摸位置；松手 = 最近页签）----
    // 拖动时不做瞬移：按下 / 移动都通过同一根弹簧产生过渡位移动画。
    // 目标是绝对位置（手指），所以不存在累积偏移，划出页签条再划回来也不会和手指错位。
    {
        const float target =
            anim_.dragging ? anim_.targetValue : static_cast<float>(anim_.targetIndex);
        SpringStep(anim_.value, anim_.valueVelocity, target, params_.N(pid::kDragValueStiffness),
                   params_.N(pid::kDragValueDamping), dt);
    }

    // ---- speed：弹簧平滑后的速度，驱动拉伸/压扁形变（参数 56/57）----
    // 口径对齐参考实现的 DampedDragAnimation.updateVelocity()：
    // VelocityTracker 给出的是 value 单位/秒，再除以 valueRange 的跨度（0..2）。
    // 本帧的位移在 OnTouch 里累积到 moveAccum_，这里换算成"每秒"再除以 2；
    // 松手后目标回 0（参考实现那边速度停在最后一次的值，这里取衰减到 0，
    // 否则拖动结束后形变会一直挂着）。
    {
        const float rate = dt > 0.0f ? (moveAccum_ / dt) : 0.0f;
        moveAccum_ = 0.0f;
        const float target = anim_.dragging ? (rate * 0.5f) : 0.0f;
        SpringStep(anim_.speed, anim_.speedVelocity, target,
                   params_.N(pid::kDragVelocityStiffness), params_.N(pid::kDragVelocityDamping),
                   dt);
        if (std::fabs(anim_.speed) < 0.01f && std::fabs(target) < 0.01f) {
            anim_.speed = 0.0f;
            anim_.speedVelocity = 0.0f;
        }
    }

    // ---- 底板视差：拖动时累积、松手弹簧回零（对齐参考实现的 offsetAnimation + panelOffset）----
    {
        if (anim_.dragging) {
            parallaxVel_ = 0.0f;
        } else {
            const float k = params_.N(pid::kPanelReturnStiffness);
            const float ratio = params_.N(pid::kPanelReturnDamping);
            const float c = 2.0f * std::sqrt(k) * ratio;
            const float accel = -k * parallaxAccum_ - c * parallaxVel_;
            parallaxVel_ += accel * dt;
            const float parallaxDelta = parallaxVel_ * dt;
            parallaxAccum_ += parallaxDelta;
            gSpringMotion += std::fabs(parallaxDelta);
            if (std::fabs(parallaxAccum_) < 0.5f) {
                parallaxAccum_ = 0.0f;
                parallaxVel_ = 0.0f;
            }
        }
        const float maxW = static_cast<float>(width_.load());
        float fraction = (maxW > 0.0f) ? (parallaxAccum_ / maxW) : 0.0f;
        fraction = std::max(-1.0f, std::min(1.0f, fraction));
        // 参考实现用 EaseOut.transform(|fraction|)；这里用等价的标准 ease-out cubic
        const float eased = 1.0f - std::pow(1.0f - std::fabs(fraction), 3.0f);
        const float amount = params_.Px(pid::kPanelParallax);
        panelOffsetPx_.store((fraction >= 0.0f ? 1.0f : -1.0f) * amount * eased);
    }

    // ---- press：按下 1 / 松开 0，spring（pressStiffness 1000 / pressDamping 1）----
    {
        const float k = params_.N(pid::kPressStiffness);
        const float ratio = params_.N(pid::kPressDamping);
        const float c = 2.0f * std::sqrt(k) * ratio;
        const float target = anim_.dragging ? 1.0f : 0.0f;
        const float accel = -k * (anim_.press - target) - c * anim_.pressVelocity;
        anim_.pressVelocity += accel * dt;
        const float pressDelta = anim_.pressVelocity * dt;
        anim_.press += pressDelta;
        gSpringMotion += std::fabs(pressDelta);
        if (anim_.press < 0.0f) { anim_.press = 0.0f; anim_.pressVelocity = 0.0f; }
        if (anim_.press > 1.0f) { anim_.press = 1.0f; anim_.pressVelocity = 0.0f; }
    }

    // ---- scaleX / scaleY：按压缩放的**独立弹簧**（参数 60..63）----
    // 对齐参考实现的 DampedDragAnimation：press() 里把两个 scale 弹到 pressedScale，
    // release() 里弹回 1（初始缩放）。之前 GL 侧是"press 进度 × 目标值"直接换算，
    // 少了这层弹簧 —— 观感上按下去/弹回来都会"硬"一档。
    {
        const float target = anim_.dragging ? params_.N(pid::kPressedScale) : 1.0f;
        SpringStep(anim_.scaleX, anim_.scaleXVelocity, target, params_.N(pid::kScaleXStiffness),
                   params_.N(pid::kScaleXDamping), dt);
        SpringStep(anim_.scaleY, anim_.scaleYVelocity, target, params_.N(pid::kScaleYStiffness),
                   params_.N(pid::kScaleYDamping), dt);
        // 防御：极端参数（pressedScale 1.8）下也不让缩放变成 0 或负
        anim_.scaleX = std::max(0.1f, anim_.scaleX);
        anim_.scaleY = std::max(0.1f, anim_.scaleY);
    }

    // ---- 跟手光斑：按压进度有独立弹簧（参数 31/32）----
    SpringStep(glowPress_, glowPressVelocity_, anim_.dragging ? 1.0f : 0.0f,
               params_.N(pid::kGlowPressStiffness), params_.N(pid::kGlowPressDamping), dt);
    if (glowPress_ < 0.0f) { glowPress_ = 0.0f; glowPressVelocity_ = 0.0f; }
    if (glowPress_ > 1.0f) { glowPress_ = 1.0f; glowPressVelocity_ = 0.0f; }

    // ---- 顶部三个玻璃按钮：按压缩放弹簧（参数 29/30）----
    for (int i = 0; i < kIconButtonCount; i++) {
        SpringStep(btnPress_[i], btnPressVelocity_[i], (btnDownIndex_ == i) ? 1.0f : 0.0f,
                   params_.N(pid::kIconPressStiffness), params_.N(pid::kIconPressDamping), dt);
        if (btnPress_[i] < 0.0f) { btnPress_[i] = 0.0f; btnPressVelocity_[i] = 0.0f; }
        if (btnPress_[i] > 1.0f) { btnPress_[i] = 1.0f; btnPressVelocity_[i] = 0.0f; }
    }
}

// ===========================================================================
// 目录场景：几何 / 动画 / 触摸
//
// 坐标约定：所有 RectF 都是 **GL 坐标**（y 从屏幕底部起算），与 DrawGlass 一致；
// 需要"自上而下"排版时用 `screenH - y - h` 换过来。
// ===========================================================================

namespace {

struct RectF {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

bool RectHit(const RectF &r, float x, float y, float pad = 0.0f) {
    return x >= r.x - pad && x <= r.x + r.w + pad && y >= r.y - pad && y <= r.y + r.h + pad;
}

const char *kButtonLabels[4] = {
    "Transparent Liquid Button",
    "Surface Liquid Button",
    "Tinted Liquid Button",
    "Tinted Liquid Button",
};

const char *kLoremIpsum =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut "
    "labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco "
    "laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in "
    "voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat "
    "cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.";

RectF ButtonRectAt(int screenW, int screenH, float d, int index) {
    const float btnH = 48.0f * d;
    const float gap = 16.0f * d;
    const float textW = MeasureTextWidth(kButtonLabels[index], 15.0f * d, false);
    RectF r;
    r.w = textW + 32.0f * d;
    r.h = btnH;
    r.x = (static_cast<float>(screenW) - r.w) * 0.5f;
    const float total = 4.0f * btnH + 3.0f * gap;
    const float topDown =
        (static_cast<float>(screenH) - total) * 0.5f + static_cast<float>(index) * (btnH + gap);
    r.y = static_cast<float>(screenH) - topDown - btnH;
    return r;
}

RectF ToggleTrackRect(int screenW, int screenH, float d) {
    RectF r;
    r.w = 64.0f * d;
    r.h = 28.0f * d;
    r.x = (static_cast<float>(screenW) - r.w) * 0.5f;
    r.y = (static_cast<float>(screenH) - r.h) * 0.5f;
    return r;
}

RectF ToggleKnobRect(int screenW, int screenH, float d, float fraction) {
    const RectF t = ToggleTrackRect(screenW, screenH, d);
    RectF r;
    // 比参考实现的 40dp 窄一点（按压放大 1.7 后 ≈ 61dp，与 64dp 的轨道基本齐宽）
    r.w = 36.0f * d;
    r.h = 24.0f * d;
    // 两端留 2dp 内缩（与上下 2dp 相同）：旋钮端头圆弧与轨道端头圆弧**同心**，
    // 圆角视觉一致；行程 = 轨道宽 - 2*2dp - 旋钮宽 = 24dp
    r.x = t.x + 2.0f * d + fraction * (t.w - 4.0f * d - r.w);
    r.y = t.y + (t.h - r.h) * 0.5f;
    return r;
}

RectF SliderTrackRect(int screenW, int screenH, float d) {
    RectF r;
    r.w = static_cast<float>(screenW) - 64.0f * d;
    r.h = 6.0f * d;
    r.x = 32.0f * d;
    r.y = (static_cast<float>(screenH) - r.h) * 0.5f;
    return r;
}

RectF SliderKnobRect(int screenW, int screenH, float d, float progress) {
    const RectF t = SliderTrackRect(screenW, screenH, d);
    RectF r;
    r.w = 40.0f * d;
    r.h = 24.0f * d;
    const float centerX = t.x + t.w * progress;
    const float clamped =
        std::max(t.x + 10.0f * d, std::min(t.x + t.w - 10.0f * d, centerX));
    r.x = clamped - r.w * 0.5f;
    r.y = (static_cast<float>(screenH) - r.h) * 0.5f;
    return r;
}

RectF DialogPanelRect(int screenW, int screenH, float d) {
    const float titleBlock = 24.0f * d * 1.35f;
    const float bodyBlock = 5.0f * 15.0f * d * 1.35f;
    RectF r;
    r.x = 40.0f * d;
    r.w = static_cast<float>(screenW) - 80.0f * d;
    r.h = 24.0f * d + titleBlock + 12.0f * d + bodyBlock + 12.0f * d + 48.0f * d + 24.0f * d;
    r.y = (static_cast<float>(screenH) - r.h) * 0.5f;
    return r;
}

void DialogButtonRects(int screenW, int screenH, float d, RectF out[2]) {
    const RectF p = DialogPanelRect(screenW, screenH, d);
    const float pad = 24.0f * d;
    const float gap = 16.0f * d;
    const float btnH = 48.0f * d;
    const float btnW = (p.w - pad * 2.0f - gap) * 0.5f;
    out[0] = RectF{p.x + pad, p.y + pad, btnW, btnH};
    out[1] = RectF{p.x + pad + btnW + gap, p.y + pad, btnW, btnH};
}

RectF LockPanelRect(int screenW, int screenH, float d) {
    RectF r;
    r.w = 300.0f * d;
    r.h = 170.0f * d;
    r.x = (static_cast<float>(screenW) - r.w) * 0.5f;
    const float topDown = static_cast<float>(screenH) * 0.26f;
    r.y = static_cast<float>(screenH) - topDown - r.h;
    return r;
}

RectF MagCardRect(int screenW, int screenH, float d) {
    RectF r;
    r.x = 24.0f * d;
    r.w = static_cast<float>(screenW) - 48.0f * d;
    r.h = 380.0f * d;
    r.y = static_cast<float>(screenH) - 24.0f * d - r.h;
    return r;
}

RectF MagLensRect(float magX, float magY, float d) {
    RectF r;
    r.w = 128.0f * d;
    r.h = 96.0f * d;
    r.x = magX - r.w * 0.5f;
    r.y = magY + 32.0f * d;
    return r;
}

RectF MagCursorRect(float magX, float magY, float d) {
    RectF r;
    r.w = 4.0f * d;
    r.h = 24.0f * d;
    r.x = magX - r.w * 0.5f;
    r.y = magY - r.h * 0.5f;
    return r;
}

RectF ProgBlurBandRect(int screenW, int screenH, float d) {
    RectF r;
    r.x = 0.0f;
    r.w = static_cast<float>(screenW);
    r.h = 128.0f * d;
    r.y = (static_cast<float>(screenH) - r.h) * 0.5f;
    return r;
}

RectF AdaptivePanelRect(int screenW, int screenH, float d) {
    RectF r;
    r.w = 160.0f * d;
    r.h = 160.0f * d;
    r.x = (static_cast<float>(screenW) - r.w) * 0.5f;
    r.y = (static_cast<float>(screenH) - r.h) * 0.5f;
    return r;
}

/** 滚动容器的第 i 张玻璃卡片（GL 坐标；scroll 单位 px）。 */
RectF ScrollCardRect(int index, int screenW, int screenH, float d, float scroll) {
    const float cardH = 160.0f * d;
    const float gap = 16.0f * d;
    const float pad = 16.0f * d;
    RectF r;
    r.x = pad;
    r.w = static_cast<float>(screenW) - pad * 2.0f;
    r.h = cardH;
    r.y = static_cast<float>(screenH) - (pad + static_cast<float>(index) * (cardH + gap) - scroll) -
          cardH;
    return r;
}

float ScrollContentHeight(int count, float d) {
    const float cardH = 160.0f * d;
    const float gap = 16.0f * d;
    return 16.0f * d * 2.0f + static_cast<float>(count) * cardH +
           static_cast<float>(count - 1) * gap;
}

RectF PgPanelRect(int screenW, int screenH, float d) {
    RectF r;
    r.w = 256.0f * d;
    r.h = 256.0f * d;
    r.x = (static_cast<float>(screenW) - r.w) * 0.5f;
    // 顶部留 96dp：参考实现是 48dp + statusBars；这里再让开 ArkUI 的返回按钮
    r.y = static_cast<float>(screenH) - 96.0f * d - r.h;
    return r;
}

RectF PgSheetRect(int screenW, int screenH, float d) {
    (void)screenW;
    (void)screenH;
    RectF r;
    r.x = 16.0f * d;
    r.w = (screenW > 0 ? static_cast<float>(screenW) : 0.0f) - 32.0f * d;
    r.h = 332.0f * d;
    r.y = 72.0f * d;
    return r;
}

/** 游乐场滑块的"整行"矩形（含标签下方 24dp 的交互高度）。 */
RectF PgSliderRowRect(int index, int screenW, int screenH, float d) {
    const RectF s = PgSheetRect(screenW, screenH, d);
    const float pad = 24.0f * d;
    const float labelH = 15.0f * d * 1.35f;
    const float rowH = 24.0f * d;
    const float gap = 16.0f * d;
    const float yTop = s.y + s.h - pad - static_cast<float>(index) * (labelH + rowH + gap);
    const float rowY = yTop - labelH - rowH;
    return RectF{s.x + pad, rowY, s.w - pad * 2.0f, rowH};
}

const char *PgButtonLabel(int index, bool sheetOpen) {
    if (index == 0) {
        return sheetOpen ? "Hide" : "Show";
    }
    return "Reset";
}

RectF PgButtonRect(int index, bool sheetOpen, int screenW, int screenH, float d) {
    const char *label = PgButtonLabel(index, sheetOpen);
    const float textW = MeasureTextWidth(label, 15.0f * d, false);
    RectF r;
    r.w = textW + 32.0f * d;
    r.h = 48.0f * d;
    r.y = 20.0f * d;
    r.x = (index == 0) ? 20.0f * d : static_cast<float>(screenW) - 20.0f * d - r.w;
    (void)screenH;
    return r;
}

}  // namespace

void GlesRenderer::UpdateCatalogAnim(float dt) {
    if (dt <= 0.0f) {
        return;
    }
    if (dt > 0.05f) {
        dt = 0.05f;
    }
    const int scene = scene_.load();
    if (scene == static_cast<int>(Scene::kTabs)) {
        return;
    }

    std::lock_guard<std::mutex> lock(animMutex_);
    switch (scene) {
        case static_cast<int>(Scene::kButtons): {
            // InteractiveHighlight 的按压弹簧：spring(0.5, 300)
            for (int i = 0; i < 4; i++) {
                SpringStep(scenePress_[i], scenePressVel_[i], (sceneDown_ == i) ? 1.0f : 0.0f,
                           300.0f, 0.5f, dt);
                if (scenePress_[i] < 0.0f) { scenePress_[i] = 0.0f; scenePressVel_[i] = 0.0f; }
                if (scenePress_[i] > 1.0f) { scenePress_[i] = 1.0f; scenePressVel_[i] = 0.0f; }
            }
            break;
        }
        case static_cast<int>(Scene::kToggle): {
            // fraction 始终走弹簧：拖动时追手指映射的目标（带一点延迟/阻尼），
            // 松手后追 0/1。刚度 450 + 阻尼 0.65：动画稍慢、收尾有回弹（q 弹）
            const float fractionTarget =
                toggleDragging_ ? toggleDragTarget_ : (toggleOn_ ? 1.0f : 0.0f);
            SpringStep(toggleFraction_, toggleFractionVel_, fractionTarget, 450.0f, 0.65f, dt);
            SpringStep(togglePress_, togglePressVel_, toggleDragging_ ? 1.0f : 0.0f, 1000.0f, 1.0f,
                       dt);
            if (togglePress_ < 0.0f) { togglePress_ = 0.0f; togglePressVel_ = 0.0f; }
            if (togglePress_ > 1.0f) { togglePress_ = 1.0f; togglePressVel_ = 0.0f; }
            // 旋钮缩放：比参考实现更软（刚度 200，阻尼 0.5 / 0.6）、按压缩放更大（1.7）
            const float knobTarget = toggleDragging_ ? 1.7f : 1.0f;
            SpringStep(toggleScaleX_, toggleScaleXVel_, knobTarget, 200.0f, 0.5f, dt);
            SpringStep(toggleScaleY_, toggleScaleYVel_, knobTarget, 200.0f, 0.6f, dt);
            toggleScaleX_ = std::max(0.1f, toggleScaleX_);
            toggleScaleY_ = std::max(0.1f, toggleScaleY_);
            // 拖动速度（velocityTracker 差分 -> spring(0.5, 300) 平滑）：驱动移动时的形变
            const float rawVelocity =
                toggleDragging_ ? (toggleFraction_ - toggleFractionLast_) / dt : 0.0f;
            SpringStep(toggleVelocity_, toggleVelocityVel_, rawVelocity, 300.0f, 0.5f, dt);
            toggleFractionLast_ = toggleFraction_;
            break;
        }
        case static_cast<int>(Scene::kSlider): {
            // 数值弹簧：手指写的是目标值，旋钮用弹簧追 —— 拖动时滞后一点点、停手时回弹（q 弹）。
            // 刚度/阻尼按"约 0.2s 收敛、过冲 ~6%"配：跟手但不粘手，收尾弹一下。
            SpringStep(sliderValueAnim_, sliderValueAnimVel_, sliderValue_, 700.0f, 0.68f, dt);
            // 动画值速度（%/s）再做一次弹簧平滑：直接差分噪声太大，形变会抖
            const float rawValueVelocity =
                sliderDragging_ ? (sliderValueAnim_ - sliderValueAnimLast_) / dt : 0.0f;
            SpringStep(sliderVelocity_, sliderVelocityVel_, rawValueVelocity, 300.0f, 0.5f, dt);
            sliderValueAnimLast_ = sliderValueAnim_;
            // 按压进度：刚度调软、阻尼 0.55 —— 按下/抬起都有轻微过冲，弹一下
            SpringStep(sliderPress_, sliderPressVel_, sliderDragging_ ? 1.0f : 0.0f, 700.0f, 0.55f,
                       dt);
            if (sliderPress_ < 0.0f) { sliderPress_ = 0.0f; sliderPressVel_ = 0.0f; }
            if (sliderPress_ > 1.0f) { sliderPress_ = 1.0f; sliderPressVel_ = 0.0f; }
            // 旋钮缩放：拖动目标 1.45、刚度 170 / 阻尼 0.45、0.55 —— 放大克制一点，但更弹
            const float knobTarget = sliderDragging_ ? 1.45f : 1.0f;
            SpringStep(sliderScaleX_, sliderScaleXVel_, knobTarget, 170.0f, 0.45f, dt);
            SpringStep(sliderScaleY_, sliderScaleYVel_, knobTarget, 170.0f, 0.55f, dt);
            sliderScaleX_ = std::max(0.1f, sliderScaleX_);
            sliderScaleY_ = std::max(0.1f, sliderScaleY_);
            break;
        }
        case static_cast<int>(Scene::kDialog): {
            for (int i = 0; i < 2; i++) {
                SpringStep(dialogPress_[i], dialogPressVel_[i], (dialogDown_ == i) ? 1.0f : 0.0f,
                           300.0f, 0.5f, dt);
                if (dialogPress_[i] < 0.0f) { dialogPress_[i] = 0.0f; dialogPressVel_[i] = 0.0f; }
                if (dialogPress_[i] > 1.0f) { dialogPress_[i] = 1.0f; dialogPressVel_[i] = 0.0f; }
            }
            break;
        }
        case static_cast<int>(Scene::kControlCenter): {
            if (!ccDragging_) {
                // 展开 spring(0.5, 300) / 收起 spring(1.0, 300)，与参考实现一致
                const float target = (ccProgress_ > 0.5f) ? 1.0f : 0.0f;
                const float damping = (target > 0.5f) ? 0.5f : 1.0f;
                SpringStep(ccProgress_, ccProgressVel_, target, 300.0f, damping, dt);
                if (std::fabs(ccProgress_ - target) < 0.005f && std::fabs(ccProgressVel_) < 0.01f) {
                    ccProgress_ = target;
                    ccProgressVel_ = 0.0f;
                }
            }
            break;
        }
        case static_cast<int>(Scene::kMagnifier):
        case static_cast<int>(Scene::kLockScreen):
        case static_cast<int>(Scene::kProgressiveBlur): {
            break;
        }
        case static_cast<int>(Scene::kAdaptiveLuminance): {
            // 参考实现是 1s tween；这里用时间常数为 0.35s 的指数趋近，观感接近
            const float lumDelta = (lumTarget_ - lumValue_) * std::min(1.0f, dt / 0.35f);
            lumValue_ += lumDelta;
            gSpringMotion += std::fabs(lumDelta);
            break;
        }
        case static_cast<int>(Scene::kScrollContainer):
        case static_cast<int>(Scene::kLazyScrollContainer): {
            if (!listDragging_) {
                const float scrollDelta = listScrollVel_ * dt;
                listScroll_ += scrollDelta;
                gSpringMotion += std::fabs(scrollDelta);
                listScrollVel_ *= std::pow(0.02f, dt);   // 快速衰减，避免长尾巴
                if (std::fabs(listScrollVel_) < 1.0f) {
                    listScrollVel_ = 0.0f;
                }
                const float contentH = ScrollContentHeight(
                    scene == static_cast<int>(Scene::kLazyScrollContainer) ? 100 : 20,
                    params_.Density());
                const float maxScroll = std::max(0.0f, contentH - static_cast<float>(height_.load()));
                if (listScroll_ < 0.0f) {
                    listScroll_ = 0.0f;
                    listScrollVel_ = 0.0f;
                }
                if (listScroll_ > maxScroll) {
                    listScroll_ = maxScroll;
                    listScrollVel_ = 0.0f;
                }
            }
            break;
        }
        case static_cast<int>(Scene::kPlayground): {
            for (int i = 0; i < 5; i++) {
                SpringStep(pgSliderPress_[i], pgSliderPressVel_[i],
                           (pgSliderDown_ == i) ? 1.0f : 0.0f, 1000.0f, 1.0f, dt);
                if (pgSliderPress_[i] < 0.0f) { pgSliderPress_[i] = 0.0f; pgSliderPressVel_[i] = 0.0f; }
                if (pgSliderPress_[i] > 1.0f) { pgSliderPress_[i] = 1.0f; pgSliderPressVel_[i] = 0.0f; }
            }
            for (int i = 0; i < 2; i++) {
                SpringStep(pgButtonPress_[i], pgButtonPressVel_[i], (pgButtonDown_ == i) ? 1.0f : 0.0f,
                           300.0f, 0.5f, dt);
                if (pgButtonPress_[i] < 0.0f) { pgButtonPress_[i] = 0.0f; pgButtonPressVel_[i] = 0.0f; }
                if (pgButtonPress_[i] > 1.0f) { pgButtonPress_[i] = 1.0f; pgButtonPressVel_[i] = 0.0f; }
            }
            break;
        }
        default:
            break;
    }
}

bool GlesRenderer::OnTouchCatalog(int action, float x, float y, int surfaceW, int surfaceH) {
    const float d = params_.Density();
    const int scene = scene_.load();
    const float glY = static_cast<float>(surfaceH) - y;   // 触摸 y（自上而下）-> GL y

    switch (scene) {
        case static_cast<int>(Scene::kButtons): {
            std::lock_guard<std::mutex> lock(animMutex_);
            int hit = -1;
            for (int i = 0; i < 4; i++) {
                if (RectHit(ButtonRectAt(surfaceW, surfaceH, d, i), x, glY)) {
                    hit = i;
                    break;
                }
            }
            if (action == 0) {
                sceneDown_ = hit;
            } else if (action == 2) {
                // 按在按钮上拖动：不响应（按钮没有拖动语义）
            } else if (action == 1 || action == 3) {
                sceneDown_ = -1;
            }
            return true;
        }
        case static_cast<int>(Scene::kToggle): {
            std::lock_guard<std::mutex> lock(animMutex_);
            const RectF track = ToggleTrackRect(surfaceW, surfaceH, d);
            // 与旋钮行程一致（轨道宽 - 2*2dp - 旋钮宽 = 24dp），拖动才跟手
            const float dragWidth = 24.0f * d;
            if (action == 0) {
                if (!RectHit(track, x, glY, 24.0f * d)) {
                    return true;
                }
                toggleDragging_ = true;
                toggleDidDrag_ = false;
                toggleDownX_ = x;
                toggleGrabStartFraction_ = toggleFraction_;
            } else if (action == 2) {
                if (!toggleDragging_) {
                    return true;
                }
                const float delta = x - toggleDownX_;
                if (std::fabs(delta) > 4.0f * d) {
                    toggleDidDrag_ = true;
                }
                // 相对按下点映射（避免累计漂移）；只更新目标，fraction 以弹簧追随 → 有一点延迟
                float next = toggleGrabStartFraction_ + delta / dragWidth;
                next = std::max(0.0f, std::min(1.0f, next));
                toggleDragTarget_ = next;
            } else if (action == 1 || action == 3) {
                if (!toggleDragging_) {
                    return true;
                }
                toggleDragging_ = false;
                if (toggleDidDrag_) {
                    toggleOn_ = toggleDragTarget_ >= 0.5f;
                    toggleDragTarget_ = toggleOn_ ? 1.0f : 0.0f;
                    GLOG("toggle drag -> %{public}d", toggleOn_ ? 1 : 0);
                } else {
                    toggleOn_ = !toggleOn_;
                    GLOG("toggle tap -> %{public}d", toggleOn_ ? 1 : 0);
                }
            }
            return true;
        }
        case static_cast<int>(Scene::kSlider): {
            std::lock_guard<std::mutex> lock(animMutex_);
            const RectF track = SliderTrackRect(surfaceW, surfaceH, d);
            if (action == 0) {
                if (!RectHit(track, x, glY, 30.0f * d)) {
                    return true;
                }
                sliderDragging_ = true;
                sliderDidDrag_ = false;
                sliderGrabValue_ = sliderValue_;
                sliderGrabX_ = x;
                // 点击轨道：直接跳到点击位置（参考实现的 detectTapGestures）
                const float next = (x - track.x) / std::max(track.w, 1.0f) * 100.0f;
                sliderValue_ = std::max(0.0f, std::min(100.0f, next));
            } else if (action == 2) {
                if (!sliderDragging_) {
                    return true;
                }
                const float delta = x - sliderGrabX_;
                if (std::fabs(delta) > 4.0f * d) {
                    sliderDidDrag_ = true;
                }
                float next = sliderGrabValue_ + delta / std::max(track.w, 1.0f) * 100.0f;
                next = std::max(0.0f, std::min(100.0f, next));
                sliderValue_ = next;
            } else if (action == 1 || action == 3) {
                if (!sliderDragging_) {
                    return true;
                }
                sliderDragging_ = false;
                GLOG("slider value=%{public}.1f", static_cast<double>(sliderValue_));
            }
            return true;
        }
        case static_cast<int>(Scene::kDialog): {
            std::lock_guard<std::mutex> lock(animMutex_);
            RectF buttons[2];
            DialogButtonRects(surfaceW, surfaceH, d, buttons);
            int hit = -1;
            for (int i = 0; i < 2; i++) {
                if (RectHit(buttons[i], x, glY)) {
                    hit = i;
                    break;
                }
            }
            if (action == 0) {
                dialogDown_ = hit;
            } else if (action == 1 || action == 3) {
                const bool tapped = (action == 1) && (dialogDown_ >= 0) && (dialogDown_ == hit);
                dialogDown_ = -1;
                if (tapped) {
                    GLOG("dialog button %{public}d tapped", hit);
                }
            }
            return true;
        }
        case static_cast<int>(Scene::kControlCenter): {
            std::lock_guard<std::mutex> lock(animMutex_);
            if (action == 0) {
                ccDragging_ = true;
                ccDragStartProgress_ = ccProgress_;
                ccDragStartY_ = y;
                ccProgressVel_ = 0.0f;
            } else if (action == 2 && ccDragging_) {
                // 参考实现：progress += delta / 1000（px）
                const float next = ccDragStartProgress_ + (y - ccDragStartY_) / 1000.0f;
                ccProgress_ = std::max(0.0f, std::min(1.0f, next));
            } else if (action == 1 || action == 3) {
                ccDragging_ = false;
            }
            return true;
        }
        case static_cast<int>(Scene::kLockScreen): {
            std::lock_guard<std::mutex> lock(animMutex_);
            if (action == 0) {
                lockDragging_ = true;
                lockGrabDX_ = x - lockOffsetX_;
                lockGrabDY_ = y - lockOffsetY_;
            } else if (action == 2 && lockDragging_) {
                lockOffsetX_ = x - lockGrabDX_;
                lockOffsetY_ = y - lockGrabDY_;
            } else if (action == 1 || action == 3) {
                lockDragging_ = false;
            }
            return true;
        }
        case static_cast<int>(Scene::kMagnifier): {
            std::lock_guard<std::mutex> lock(animMutex_);
            if (!magInited_) {
                magX_ = static_cast<float>(surfaceW) * 0.5f;
                magY_ = static_cast<float>(surfaceH) * 0.42f;
                magInited_ = true;
            }
            if (action == 0) {
                magDragging_ = true;
                magGrabDX_ = x - magX_;
                magGrabDY_ = glY - magY_;
            } else if (action == 2 && magDragging_) {
                magX_ = x - magGrabDX_;
                magY_ = glY - magGrabDY_;
            } else if (action == 1 || action == 3) {
                magDragging_ = false;
            }
            return true;
        }
        case static_cast<int>(Scene::kProgressiveBlur): {
            return true;
        }
        case static_cast<int>(Scene::kAdaptiveLuminance): {
            std::lock_guard<std::mutex> lock(animMutex_);
            if (action == 0) {
                const RectF panel = AdaptivePanelRect(surfaceW, surfaceH, d);
                if (!RectHit(panel, x, glY)) {
                    return true;
                }
                pgDragging_ = true;
                pgGrabDX_ = x - panel.x;
                pgGrabDY_ = glY - panel.y;
            } else if (action == 2 && pgDragging_) {
                // 用 pgX_/pgY_ 存自适应面板的位置偏移（仅此场景复用）
                pgX_ = x - pgGrabDX_ - AdaptivePanelRect(surfaceW, surfaceH, d).x;
                pgY_ = glY - pgGrabDY_ - AdaptivePanelRect(surfaceW, surfaceH, d).y;
            } else if (action == 1 || action == 3) {
                pgDragging_ = false;
            }
            return true;
        }
        case static_cast<int>(Scene::kScrollContainer):
        case static_cast<int>(Scene::kLazyScrollContainer): {
            std::lock_guard<std::mutex> lock(animMutex_);
            if (action == 0) {
                listDragging_ = true;
                listGrabScroll_ = listScroll_;
                listGrabY_ = y;
                listScrollVel_ = 0.0f;
            } else if (action == 2 && listDragging_) {
                // 手指上滑（y 变小）-> 内容上移 -> scroll 变大
                listScroll_ = listGrabScroll_ + (listGrabY_ - y);
                if (listScroll_ < -200.0f) {
                    listScroll_ = -200.0f;   // 顶部回弹余量
                }
            } else if (action == 1 || action == 3) {
                listDragging_ = false;
            }
            return true;
        }
        case static_cast<int>(Scene::kPlayground): {
            std::lock_guard<std::mutex> lock(animMutex_);
            const RectF panel = PgPanelRect(surfaceW, surfaceH, d);
            RectF button0 = PgButtonRect(0, pgSheetOpen_, surfaceW, surfaceH, d);
            RectF button1 = PgButtonRect(1, pgSheetOpen_, surfaceW, surfaceH, d);
            if (action == 0) {
                if (RectHit(panel, x, glY)) {
                    pgDragging_ = true;
                    pgGrabDX_ = x - panel.x;
                    pgGrabDY_ = glY - panel.y;
                    return true;
                }
                if (RectHit(button0, x, glY)) {
                    pgButtonDown_ = 0;
                    return true;
                }
                if (RectHit(button1, x, glY)) {
                    pgButtonDown_ = 1;
                    return true;
                }
                if (pgSheetOpen_) {
                    for (int i = 0; i < 5; i++) {
                        if (RectHit(PgSliderRowRect(i, surfaceW, surfaceH, d), x, glY)) {
                            pgSliderDown_ = i;
                            pgSliderGrabValue_[i] = pgSliderValue_[i];
                            pgSliderGrabX_[i] = x;
                            // 点击定位（与参考实现的 tap 行为一致）
                            const RectF row = PgSliderRowRect(i, surfaceW, surfaceH, d);
                            const float next = (x - row.x) / std::max(row.w, 1.0f);
                            pgSliderValue_[i] = std::max(0.0f, std::min(1.0f, next));
                            return true;
                        }
                    }
                }
            } else if (action == 2) {
                if (pgDragging_) {
                    pgX_ = x - pgGrabDX_ - panel.x;
                    pgY_ = glY - pgGrabDY_ - panel.y;
                    return true;
                }
                if (pgSliderDown_ >= 0) {
                    const RectF row = PgSliderRowRect(pgSliderDown_, surfaceW, surfaceH, d);
                    const float delta = x - pgSliderGrabX_[pgSliderDown_];
                    float next = pgSliderGrabValue_[pgSliderDown_] + delta / std::max(row.w, 1.0f);
                    next = std::max(0.0f, std::min(1.0f, next));
                    pgSliderValue_[pgSliderDown_] = next;
                    return true;
                }
            } else if (action == 1 || action == 3) {
                if (pgButtonDown_ == 0 && action == 1) {
                    pgSheetOpen_ = !pgSheetOpen_;
                    overlayTextDirty_.store(true);
                    GLOG("playground sheet %{public}s", pgSheetOpen_ ? "open" : "closed");
                } else if (pgButtonDown_ == 1 && action == 1) {
                    // Reset：与参考实现一致，回到初始值
                    pgX_ = 0.0f;
                    pgY_ = 0.0f;
                    const float sliderDefaults[5] = {0.5f, 0.0f, 0.2f, 0.2f, 0.0f};
                    for (int i = 0; i < 5; i++) {
                        pgSliderValue_[i] = sliderDefaults[i];
                    }
                    GLOG("playground reset");
                }
                pgButtonDown_ = -1;
                pgDragging_ = false;
                pgSliderDown_ = -1;
            }
            return true;
        }
        default:
            return false;
    }
}

void GlesRenderer::OnTouch(int action, float x, float y, int64_t /*timestampMs*/) {
    // 触摸是"必须立刻出帧"的信号，也是交互档帧率的触发点
    lastTouchMs_.store(NowMs());
    forceRedraw_.store(true);
    const int surfaceW = static_cast<int>(width_.load());
    const int surfaceH = static_cast<int>(height_.load());
    if (surfaceW <= 0 || surfaceH <= 0) return;

    // 目录场景不走页签场景的手势（顶部玻璃按钮 / 浮动件都没有）
    if (scene_.load() != static_cast<int>(Scene::kTabs)) {
        OnTouchCatalog(action, x, y, surfaceW, surfaceH);
        return;
    }

    // ---- 顶部三个玻璃按钮：最高优先级 ----
    {
        int hit = -1;
        const bool inBtn = HitButton(x, y, surfaceW, surfaceH, &hit);
        int tapped = -1;
        {
            std::lock_guard<std::mutex> lock(animMutex_);
            if (action == 0) {                       // DOWN
                btnDownIndex_ = inBtn ? hit : -1;
            } else if (action == 2) {                // MOVE
                if (btnDownIndex_ >= 0) return;      // 按在按钮上拖动：不参与滚动/页签拖动
            } else if (action == 1 || action == 3) { // UP / CANCEL
                if (btnDownIndex_ >= 0) {
                    const int down = btnDownIndex_;
                    btnDownIndex_ = -1;
                    if (action == 1 && inBtn && hit == down) {
                        tapped = down;
                    }
                }
            }
        }
        if (inBtn && action != 2) {
            if (tapped >= 0) {
                const int target = kIconButtonMenuTargets[tapped];
                GLOG("icon button %{public}d tap -> menu target %{public}d", tapped, target);
                if (MenuRequestFn fn = menuRequestFn_.load()) {
                    fn(target);   // 交给 ArkTS 开菜单（napi threadsafe function）
                }
            }
            return;   // 按钮区域内的事件一律不再往下走
        }
    }

    // ---- 浮动玻璃件：拖动（优先于底板与背景滚动）----
    {
        const float touchGlY = static_cast<float>(surfaceH) - y;
        const float fW = static_cast<float>(surfaceW) - params_.Px(pid::kOuterPaddingH) * 2.0f;
        const float fH = params_.Px(pid::kBarHeight);
        const float fx = floatX_.load();
        const float fy = floatY_.load();
        const bool inFloat = (x >= fx) && (x <= fx + fW) && (touchGlY >= fy) &&
                             (touchGlY <= fy + fH);

        if (draggingFloat_) {
            if (action == 2) {
                float nx = x - floatGrabDX_;
                float ny = touchGlY - floatGrabDY_;
                nx = std::max(0.0f, std::min(nx, static_cast<float>(surfaceW) - fW));
                ny = std::max(0.0f, std::min(ny, static_cast<float>(surfaceH) - fH));
                floatX_.store(nx);
                floatY_.store(ny);
            } else if (action == 1) {
                draggingFloat_ = false;
                GLOG("float drag end: x=%{public}.0f y=%{public}.0f", floatX_.load(),
                     floatY_.load());
            }
            return;
        }
        if (action == 0 && inFloat) {
            draggingFloat_ = true;
            floatGrabDX_ = x - fx;
            floatGrabDY_ = touchGlY - fy;
            GLOG("float drag start: x=%{public}.0f y=%{public}.0f", fx, fy);
            return;
        }
    }

    // 触摸 y 从顶部起算；GL 的 y 从底部起算
    const float glY = static_cast<float>(surfaceH) - y;

    // 只在底部玻璃条附近响应（条：[kMarginV, kMarginV + kBarH]，上下各放宽一点）
    const float barV = params_.Px(pid::kOuterPaddingV);
    const float barHpx = params_.Px(pid::kBarHeight);
    const bool inBar = glY > (barV - 160.0f) && glY < (barV + barHpx + 220.0f);

    const float value = TabValueFromX(x, surfaceW);
    // 页签格子宽度（视差累积要用；与 TabValueFromX / 指示器定位同一套几何，必须走 TabInnerPadPx）
    const float tabWidthPx_ = (static_cast<float>(surfaceW) -
                               params_.Px(pid::kOuterPaddingH) * 2.0f -
                               TabInnerPadPx() * 2.0f) / 3.0f;

    std::lock_guard<std::mutex> lock(animMutex_);

    // ---- 拖动中的事件优先于"手指在不在条上"：划出页签条再划回来也必须继续跟手 ----
    if (anim_.dragging) {
        if (action == 2) {  // MOVE：拖动目标 = 触摸位置（绝对跟手）
            static int moveCount = 0;
            static bool firstMove = true;
            if (firstMove || (moveCount % 30) == 0) {
                GLOG("touch MOVE#%{public}d x=%{public}.0f value=%{public}.2f", moveCount, x,
                     value);
                firstMove = false;
            }
            moveCount++;
            const float prev = anim_.targetValue;
            float next = value;
            if (next < 0.0f) next = 0.0f;
            if (next > 2.0f) next = 2.0f;
            anim_.targetValue = next;
            const float delta = anim_.targetValue - prev;
            // 速度不在事件里换算（事件率不一定是 60Hz）：只累积位移，UpdateAnim 里按 dt 换成"每秒"
            moveAccum_ += delta;
            // 底板视差：把本帧的横向位移累积起来（与参考实现的 offsetAnimation 同义）
            parallaxAccum_ += delta * tabWidthPx_;
            const float maxW = static_cast<float>(surfaceW);
            if (parallaxAccum_ > maxW) parallaxAccum_ = maxW;
            if (parallaxAccum_ < -maxW) parallaxAccum_ = -maxW;
            return;
        }
        if (action == 1 || action == 3) {  // UP / CANCEL
            anim_.dragging = false;
            // 目标取**手指位置**（不是被弹簧拖后的 value）：快速拖动松手也不会落错页签
            int target = static_cast<int>(std::lround(anim_.targetValue));
            if (target < 0) target = 0;
            if (target > 2) target = 2;
            anim_.targetIndex = target;
            if (selectedTab_ != target) {
                selectedTab_ = target;
                labelsDirty_.store(true);   // 选中色要重渲染文字纹理
            }
            GLOG("touch UP targetValue=%{public}.2f value=%{public}.2f -> target=%{public}d",
                 anim_.targetValue, anim_.value, target);
            return;
        }
    }

    // ---- 背景滚动：不在底部玻璃条上时，纵向拖动 = 滚照片 ----
    // （对齐参考实现的 PhotoBackground 的 verticalScroll）
    if (!inBar) {
        if (action == 0) {
            scrolling_ = true;
            scrollGrabStart_ = scroll_.load();
            scrollGrabY_ = y;
        } else if (action == 2 && scrolling_) {
            const float contentH = static_cast<float>(surfaceH) *
                                   std::max(1.0f, params_.N(pid::kBackgroundScrollRange));
            const float maxScrollPx = std::max(1.0f, contentH - static_cast<float>(surfaceH));
            // 手指上滑（y 变小）-> 内容往上走 -> scroll 变大
            float next = scrollGrabStart_ + (scrollGrabY_ - y) / maxScrollPx;
            next = std::max(0.0f, std::min(1.0f, next));
            scroll_.store(next);
        } else if (action == 1) {
            scrolling_ = false;
        }
        return;   // 背景区域不参与指示器拖动
    }

    if (action == 0) {  // DOWN：按下即开始位移（弹簧过渡到触摸位置，不瞬移）
        anim_.dragging = true;
        anim_.targetValue = std::max(0.0f, std::min(2.0f, value));
        moveAccum_ = 0.0f;
        GLOG("touch DOWN x=%{public}.0f value=%{public}.2f -> target=%{public}.2f", x, value,
             anim_.targetValue);
    }
}

bool GlesRenderer::InitEgl(void *window) {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        GERR("eglGetDisplay failed");
        return false;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display_, &major, &minor) != EGL_TRUE) {
        GERR("eglInitialize failed: 0x%{public}x", eglGetError());
        return false;
    }

    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE,
    };
    EGLint numConfigs = 0;
    if (eglChooseConfig(display_, attribs, &config_, 1, &numConfigs) != EGL_TRUE || numConfigs < 1) {
        GERR("eglChooseConfig failed: 0x%{public}x", eglGetError());
        return false;
    }

    surface_ = eglCreateWindowSurface(display_, config_,
                                      reinterpret_cast<EGLNativeWindowType>(window), nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        GERR("eglCreateWindowSurface failed: 0x%{public}x", eglGetError());
        return false;
    }

    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        GERR("eglCreateContext failed: 0x%{public}x", eglGetError());
        return false;
    }
    if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
        GERR("eglMakeCurrent failed: 0x%{public}x", eglGetError());
        return false;
    }
    // swap interval = 1 只是个提示：实测 XComponent 的 surface 上不保证按 vsync 阻塞，
    // 真正的限速在 RenderLoop 的 PaceFrame()（时间基准）。
    eglSwapInterval(display_, 1);

    GLOG("EGL ready: v%{public}d.%{public}d, renderer=%{public}s",
         major, minor, reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    return true;
}

bool GlesRenderer::InitGl() {
    progBg_ = LinkProgram(kVertexShader, kBackgroundShader);
    progBlur_ = LinkProgram(kVertexShader, kBlurShader);
    // 底板 / 指示器 / 光斑 **共用**前导 + SDF helper，只是 body 不同
    const std::string commonShader = std::string(kGlslPrelude) + kGlslSdfHelpers;
    progGlass_ = LinkProgram(kVertexShader, (commonShader + kGlassShaderBody).c_str());
    progIndicator_ = LinkProgram(kVertexShader, (commonShader + kIndicatorShaderBody).c_str());
    progGlow_ = LinkProgram(kVertexShader, (commonShader + kGlowShaderBody).c_str());
    progIcon_ = LinkProgram(kVertexShader, (std::string(kGlslPrelude) + kIconShaderBody).c_str());
    // 目录场景：纯色 / 拷贝 / 文字图标 / 渐进模糊
    progSolid_ = LinkProgram(kVertexShader, (commonShader + kSolidShaderBody).c_str());
    progCopy_ = LinkProgram(kVertexShader, (std::string(kGlslPrelude) + kCopyShaderBody).c_str());
    progSprite_ = LinkProgram(kVertexShader, (std::string(kGlslPrelude) + kSpriteShaderBody).c_str());
    progProgBlur_ =
        LinkProgram(kVertexShader, (commonShader + kProgressiveBlurShaderBody).c_str());
    progExtCopy_ = LinkProgram(kVertexShader, kExternalCopyShader);
    if (progBg_ == 0 || progBlur_ == 0 || progGlass_ == 0 || progIndicator_ == 0 ||
        progGlow_ == 0 || progIcon_ == 0 || progSolid_ == 0 || progCopy_ == 0 ||
        progSprite_ == 0 || progProgBlur_ == 0 || progExtCopy_ == 0) {
        return false;
    }
    glGenFramebuffers(2, blurFbo_);
    glGenTextures(2, blurTex_);
    glGenFramebuffers(2, blurFbo2_);
    glGenTextures(2, blurTex2_);
    glGenFramebuffers(4, &blurDownFbo_[0][0]);
    glGenTextures(4, &blurDownTex_[0][0]);
    glGenFramebuffers(1, &bgFbo_);
    glGenTextures(1, &bgTex_);
    glGenFramebuffers(1, &sceneFbo_);
    glGenTextures(1, &sceneTex_);
    glGenFramebuffers(1, &contentFbo_);
    glGenTextures(1, &contentBlitTex_);
    glGenTextures(1, &labelTex_);
    glGenTextures(1, &labelAccentTex_);
    glGenTextures(1, &iconTex_);
    glGenTextures(1, &photoTex_);
    glGenTextures(1, &contentTextTex_);
    glGenTextures(1, &overlayTextTex_);
    glGenTextures(1, &planeIconTex_);
    // 飞机图标（控制中心 / 游乐场）：24×24 视口的绝对 path，白色、预乘 alpha
    {
        const int iconPx = std::max(8, static_cast<int>(kPlaneIconDp * params_.Density() + 0.5f));
        LabelBitmap icon;
        if (RenderIconPath(kPlaneIconPath, 24.0f, iconPx, 0xFFFFFFFF, icon)) {
            glBindTexture(GL_TEXTURE_2D, planeIconTex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, icon.width, icon.height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, icon.pixels.data());
            SetupTexture(planeIconTex_);
            planeIconSizePx_ = iconPx;
        }
    }
    CreateContentImage();
    GLOG("GL programs ready (bg/blur/glass/indicator/glow/icon/solid/copy/sprite/progblur/extcopy)");
    return true;
}

void GlesRenderer::CreateContentImage() {
    if (contentImage_ != nullptr) {
        return;
    }
    glGenTextures(1, &contentTex_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, contentTex_);
    // 外纹理不能 glTexImage2D（内容由生产端写入），只需过滤/环绕参数
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    contentImage_ = OH_NativeImage_Create(contentTex_, GL_TEXTURE_EXTERNAL_OES);
    if (contentImage_ == nullptr) {
        GERR("OH_NativeImage_Create failed");
        return;
    }
    OH_NativeImage_AttachContext(contentImage_, contentTex_);
    OH_OnFrameAvailableListener listener{nullptr, OnContentFrameAvailableNoop};
    OH_NativeImage_SetOnFrameAvailableListener(contentImage_, listener);

    uint64_t surfaceId = 0;
    if (OH_NativeImage_GetSurfaceId(contentImage_, &surfaceId) != 0) {
        GERR("OH_NativeImage_GetSurfaceId failed");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(contentMutex_);
        contentSurfaceId_ = std::to_string(surfaceId);
    }
    GLOG("content image ready: surfaceId=%{public}s", std::to_string(surfaceId).c_str());
}

bool GlesRenderer::UpdateContentTexture(int w, int h) {
    if (contentImage_ == nullptr || contentBlitTex_ == 0 || w <= 0 || h <= 0) {
        return false;
    }
    // 没有新帧（ArkUI 那棵树这一帧没重绘）就沿用上一张 blit 结果，不重复拷贝。
    if (OH_NativeImage_UpdateSurfaceImage(contentImage_) != 0) {
        return false;
    }
    if (OH_NativeImage_GetTransformMatrixV2(contentImage_, contentMatrix_) != 0) {
        OH_NativeImage_GetTransformMatrix(contentImage_, contentMatrix_);
    }
    contentValid_ = true;
    if (!contentLogged_) {
        contentLogged_ = true;
        GLOG("content first frame: m0=%{public}.3f m5=%{public}.3f m12=%{public}.3f m13=%{public}.3f",
             contentMatrix_[0], contentMatrix_[5], contentMatrix_[12], contentMatrix_[13]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, contentFbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glUseProgram(progExtCopy_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, contentTex_);
    glUniform1i(glGetUniformLocation(progExtCopy_, "uExternal"), 0);
    glUniformMatrix4fv(glGetUniformLocation(progExtCopy_, "uTexMatrix"), 1, GL_FALSE,
                       contentMatrix_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void GlesRenderer::DestroyGl() {
    if (bgFbo_ != 0) { glDeleteFramebuffers(1, &bgFbo_); bgFbo_ = 0; }
    if (bgTex_ != 0) { glDeleteTextures(1, &bgTex_); bgTex_ = 0; }
    if (blurFbo_[0] != 0) { glDeleteFramebuffers(2, blurFbo_); blurFbo_[0] = 0; }
    if (blurTex_[0] != 0) { glDeleteTextures(2, blurTex_); blurTex_[0] = 0; }
    if (blurFbo2_[0] != 0) { glDeleteFramebuffers(2, blurFbo2_); blurFbo2_[0] = 0; }
    if (blurTex2_[0] != 0) { glDeleteTextures(2, blurTex2_); blurTex2_[0] = 0; }
    if (blurDownFbo_[0][0] != 0) {
        glDeleteFramebuffers(4, &blurDownFbo_[0][0]);
        blurDownFbo_[0][0] = 0;
    }
    if (blurDownTex_[0][0] != 0) {
        glDeleteTextures(4, &blurDownTex_[0][0]);
        blurDownTex_[0][0] = 0;
    }
    if (progBg_ != 0) { glDeleteProgram(progBg_); progBg_ = 0; }
    if (progBlur_ != 0) { glDeleteProgram(progBlur_); progBlur_ = 0; }
    if (progGlass_ != 0) { glDeleteProgram(progGlass_); progGlass_ = 0; }
    if (progIndicator_ != 0) { glDeleteProgram(progIndicator_); progIndicator_ = 0; }
    if (progGlow_ != 0) { glDeleteProgram(progGlow_); progGlow_ = 0; }
    if (progIcon_ != 0) { glDeleteProgram(progIcon_); progIcon_ = 0; }
    if (progSolid_ != 0) { glDeleteProgram(progSolid_); progSolid_ = 0; }
    if (progCopy_ != 0) { glDeleteProgram(progCopy_); progCopy_ = 0; }
    if (progSprite_ != 0) { glDeleteProgram(progSprite_); progSprite_ = 0; }
    if (progProgBlur_ != 0) { glDeleteProgram(progProgBlur_); progProgBlur_ = 0; }
    if (progExtCopy_ != 0) { glDeleteProgram(progExtCopy_); progExtCopy_ = 0; }
    if (contentImage_ != nullptr) {
        OH_NativeImage_DetachContext(contentImage_);
        OH_NativeImage_Destroy(&contentImage_);
        contentImage_ = nullptr;
    }
    if (contentTex_ != 0) { glDeleteTextures(1, &contentTex_); contentTex_ = 0; }
    if (contentBlitTex_ != 0) { glDeleteTextures(1, &contentBlitTex_); contentBlitTex_ = 0; }
    if (contentFbo_ != 0) { glDeleteFramebuffers(1, &contentFbo_); contentFbo_ = 0; }
    {
        std::lock_guard<std::mutex> lock(contentMutex_);
        contentSurfaceId_.clear();
    }
    contentValid_ = false;
    contentLogged_ = false;
    if (sceneFbo_ != 0) { glDeleteFramebuffers(1, &sceneFbo_); sceneFbo_ = 0; }
    if (sceneTex_ != 0) { glDeleteTextures(1, &sceneTex_); sceneTex_ = 0; }
    if (labelTex_ != 0) { glDeleteTextures(1, &labelTex_); labelTex_ = 0; }
    if (labelAccentTex_ != 0) { glDeleteTextures(1, &labelAccentTex_); labelAccentTex_ = 0; }
    if (iconTex_ != 0) { glDeleteTextures(1, &iconTex_); iconTex_ = 0; }
    if (photoTex_ != 0) { glDeleteTextures(1, &photoTex_); photoTex_ = 0; }
    if (contentTextTex_ != 0) { glDeleteTextures(1, &contentTextTex_); contentTextTex_ = 0; }
    if (overlayTextTex_ != 0) { glDeleteTextures(1, &overlayTextTex_); overlayTextTex_ = 0; }
    if (planeIconTex_ != 0) { glDeleteTextures(1, &planeIconTex_); planeIconTex_ = 0; }
    planeIconSizePx_ = 0;
    contentTextW_ = 0;
    contentTextH_ = 0;
    overlayTextW_ = 0;
    overlayTextH_ = 0;
    iconTexSizePx_ = 0;
    targetW_ = 0;
    targetH_ = 0;
}

void GlesRenderer::DestroyEgl() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) { eglDestroyContext(display_, context_); }
        if (surface_ != EGL_NO_SURFACE) { eglDestroySurface(display_, surface_); }
        eglTerminate(display_);
    }
    display_ = EGL_NO_DISPLAY;
    surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
    config_ = nullptr;
}

bool GlesRenderer::UploadLabels(int w, int h) {
    // 每次都按当前参数重算尺寸：底板高 / 水平外边距一变，文字位图的**尺寸**也要变。
    // 旧写法只在首次（或 resize）算一次，之后滑这两个滑杆时文字位图仍是旧尺寸，
    // 再被拉伸到新的底板矩形上（文字被拉长）。
    labelW_ = w - static_cast<int>(params_.Px(pid::kOuterPaddingH) * 2.0f);
    labelH_ = static_cast<int>(params_.Px(pid::kBarHeight));
    if (labelW_ <= 0 || labelH_ <= 0 || labelTex_ == 0 || labelAccentTex_ == 0) {
        GERR("UploadLabels: bad label size %{public}dx%{public}d tex=%{public}u",
             labelW_, labelH_, labelTex_);
        return false;
    }
    static const std::vector<std::string> kLabels{"首页", "搜索", "我的"};
    const auto alphaByte = [](float a) {
        int v = static_cast<int>(a * 255.0f + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return static_cast<uint32_t>(v) << 24;
    };
    const uint32_t selArgb = alphaByte(params_.N(pid::kLabelSelectedAlpha)) | kTabSelectedRgb;
    const uint32_t unselArgb = alphaByte(params_.N(pid::kLabelUnselectedAlpha)) | kTabUnselectedRgb;
    const float iconSizePx = kTabIconSizeDp * params_.Density();
    const float iconGapPx = kTabIconGapDp * params_.Density();
    const float fontPx = params_.Px(pid::kLabelFontSize);
    const float innerPad = TabInnerPadPx();
    // selectedIndex = -1：整条只用一种颜色。两张位图除颜色外参数完全相同，
    // 保证叠在一起时字形/图标逐像素对齐（差一个参数就会错位成"重影"）。
    const auto rasterize = [&](uint32_t argb, LabelBitmap &out) {
        return RenderTabStrip(labelW_, labelH_, kLabels, kTabIconPaths, -1, iconSizePx, iconGapPx,
                              kTabIconBoldStroke, fontPx, innerPad, argb, argb, out);
    };

    // ① 常态色：底板里所有页签都是常态色（凸显色不在这里，见 ②）
    LabelBitmap labels;
    if (!rasterize(unselArgb, labels)) {
        GERR("RenderTabStrip failed");
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, labelTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, labels.width, labels.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, labels.pixels.data());
    SetupTexture(labelTex_);

    // ② 强调色：整条都是凸显色，只有指示器透镜里才会采到它
    LabelBitmap accent;
    if (!rasterize(selArgb, accent)) {
        GERR("RenderTabStrip (accent) failed");
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, labelAccentTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, accent.width, accent.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, accent.pixels.data());
    SetupTexture(labelAccentTex_);

    GLOG("label textures %{public}dx%{public}d uploaded, selected=%{public}d",
         labels.width, labels.height, selectedTab_);
    return true;
}

bool GlesRenderer::UploadIcons(int iconSizePx) {
    if (iconSizePx <= 0 || iconTex_ == 0) {
        return false;
    }
    LabelBitmap icons;
    if (!RenderGlassIcons(iconSizePx, icons)) {
        GERR("RenderGlassIcons failed (size=%{public}d)", iconSizePx);
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, iconTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, icons.width, icons.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, icons.pixels.data());
    SetupTexture(iconTex_);
    iconTexSizePx_ = iconSizePx;
    GLOG("icon texture %{public}dx%{public}d uploaded", icons.width, icons.height);
    return true;
}

bool GlesRenderer::EnsureTargets(int w, int h) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (w == targetW_ && h == targetH_) {
        return true;
    }
    targetW_ = w;
    targetH_ = h;
    forceRedraw_.store(true);   // 尺寸变了必须重画

    glBindTexture(GL_TEXTURE_2D, bgTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    SetupTexture(bgTex_);
    glBindFramebuffer(GL_FRAMEBUFFER, bgFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bgTex_, 0);

    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, blurTex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        SetupTexture(blurTex_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, blurFbo_[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blurTex_[i], 0);
        // 按钮专用模糊链（半径与底座不同，见 DrawFrame 的 pass 2b）
        glBindTexture(GL_TEXTURE_2D, blurTex2_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        SetupTexture(blurTex2_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, blurFbo2_[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blurTex2_[i],
                               0);
    }
    // 主模糊链的降采样金字塔：level 0 = 1/2 分辨率，level 1 = 1/4（见 RenderBlur）
    for (int level = 0; level < 2; level++) {
        const int lw = std::max(1, w >> (level + 1));
        const int lh = std::max(1, h >> (level + 1));
        blurDownW_[level] = lw;
        blurDownH_[level] = lh;
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, blurDownTex_[level][i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         nullptr);
            SetupTexture(blurDownTex_[level][i]);
            glBindFramebuffer(GL_FRAMEBUFFER, blurDownFbo_[level][i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   blurDownTex_[level][i], 0);
        }
    }
    glBindTexture(GL_TEXTURE_2D, sceneTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    SetupTexture(sceneTex_);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneTex_, 0);

    // ArkTS 内容外纹理的 2D 落点（大小与 surface 一致；导出节点也铺满同一矩形）
    glBindTexture(GL_TEXTURE_2D, contentBlitTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    SetupTexture(contentBlitTex_);
    glBindFramebuffer(GL_FRAMEBUFFER, contentFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, contentBlitTex_,
                           0);
    contentValid_ = false;   // 新尺寸下旧帧作废，等下一帧外纹理

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        GERR("FBO incomplete: 0x%{public}x", status);
        return false;
    }

    // ---- 文字纹理：底板区域，用 native_drawing 栅格化 ----
    labelW_ = w - static_cast<int>(params_.Px(pid::kOuterPaddingH) * 2.0f);
    labelH_ = static_cast<int>(params_.Px(pid::kBarHeight));
    UploadLabels(w, h);
    // 目录场景的文字位图与屏幕尺寸相关，尺寸变了要重传
    contentTextDirty_.store(true);
    overlayTextDirty_.store(true);

    GLOG("targets resized: %{public}d x %{public}d", w, h);
    return true;
}

// ---------------------------------------------------------------------------
// 高斯模糊链
// ---------------------------------------------------------------------------

GLuint GlesRenderer::BlurIterations(GLuint sourceTex, float sigmaPx, int w, int h,
                                    GLuint fboPair[2], GLuint texPair[2]) {
    // 9-tap 核（见 kBlurShader）的方差：Σ w·x² ≈ 2.854，等效 σ ≈ 1.69 × 采样间距。
    // 多趟叠加的总方差 = Σ(2.854·间距²)，所以按"间距 ~2× 递增"把方差凑到 σ²：
    //   σ=32 → 2, 4, 8, 2.4（4 趟）——最后一趟的间距与前面已经糊出来的尺度相当，
    //   核的空洞被填掉，不会像"单趟间距=半径"那样在 30px 尺度上留下鬼影/色带。
    constexpr float kKernelVariance = 2.854f;
    constexpr int kMaxIterations = 8;
    // sourceTex 落在 ping-pong 里时，第一趟必须写另一张，避免读写同一纹理。
    const int writeIdx = (sourceTex == texPair[0]) ? 1 : 0;
    GLuint src = sourceTex;
    float remaining = sigmaPx * sigmaPx;
    float spacing = std::max(1.0f, sigmaPx * 0.125f);
    for (int iter = 0; iter < kMaxIterations && remaining > 0.05f; iter++) {
        const float step = std::min(spacing, std::sqrt(remaining / kKernelVariance));
        for (int axis = 0; axis < 2; axis++) {
            const GLuint dst = texPair[axis == 0 ? writeIdx : 1 - writeIdx];
            glBindFramebuffer(GL_FRAMEBUFFER, fboPair[axis == 0 ? writeIdx : 1 - writeIdx]);
            glViewport(0, 0, w, h);
            glDisable(GL_BLEND);
            glUseProgram(progBlur_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src);
            glUniform1i(glGetUniformLocation(progBlur_, "uSource"), 0);
            glUniform2f(glGetUniformLocation(progBlur_, "uTexel"), 1.0f / static_cast<float>(w),
                        1.0f / static_cast<float>(h));
            glUniform2f(glGetUniformLocation(progBlur_, "uDir"), axis == 0 ? 1.0f : 0.0f,
                        axis == 0 ? 0.0f : 1.0f);
            glUniform1f(glGetUniformLocation(progBlur_, "uRadius"), step);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            src = dst;
        }
        remaining -= kKernelVariance * step * step;
        spacing = std::max(step * 2.0f, 1.0f);
    }
    return src;
}

GLuint GlesRenderer::RenderButtonBlur(GLuint sourceTex, float radiusPx) {
    // 这张结果只有按钮会采样，所以整条模糊链只算按钮那一块
    //（外扩 4×半径覆盖最大采样间距的 9-tap 支撑域）
    float btnRowLeft = 0.0f;
    float btnRowBottom = 0.0f;
    ButtonRectPx(0, targetW_, targetH_, &btnRowLeft, &btnRowBottom);
    const float btnRowW = static_cast<float>(kIconButtonCount) * ButtonSizePx() +
                          static_cast<float>(kIconButtonCount - 1) * ButtonSpacingPx();
    glEnable(GL_SCISSOR_TEST);
    SetScissorRect(btnRowLeft, btnRowBottom, btnRowW, ButtonSizePx(), radiusPx * 4.0f + 8.0f,
                   targetW_, targetH_);
    const GLuint result =
        BlurIterations(sourceTex, radiusPx, targetW_, targetH_, blurFbo2_, blurTex2_);
    glDisable(GL_SCISSOR_TEST);
    return result;
}

GLuint GlesRenderer::RenderBlur(GLuint sourceTex, float radiusPx) {
    // radiusPx = 高斯 σ（px）。与上游 Compose `blur(radius)` 的语义一致：
    // 之前的实现把 radius 当成单趟 9-tap 的采样间距，等效 σ ≈ 1.69×radius，整体糊过头。
    //
    // ⚠️ 整条链必须**关掉 scissor**：DrawGlass 为省填充率开着 scissor 调进来，但
    //    金字塔的目标分辨率只有 1/2、1/4，同一组 scissor 坐标在低分辨率上圈的是
    //    另一块区域 —— 形状内的纹素根本没被写，采样到的是未初始化的黑（面板会发死）。
    //    而且折射会把形状外最多 refractionAmount 的像素采进来，贴着形状的 scissor
    //    本来也覆盖不到。按钮链（RenderButtonBlur）是另一套：全分辨率 + 显式 scissor。
    const GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);

    GLuint result = sourceTex;
    int level = -1;   // -1 = 全分辨率
    if (radiusPx >= kBlurPyramidQuarterMinPx && blurDownW_[1] > 0 && blurDownH_[1] > 0) {
        level = 1;
    } else if (radiusPx >= kBlurPyramidHalfMinPx && blurDownW_[0] > 0 && blurDownH_[0] > 0) {
        level = 0;
    }
    if (level < 0) {
        result = BlurIterations(sourceTex, radiusPx, targetW_, targetH_, blurFbo_, blurTex_);
    } else {
        // 大 σ：先逐级降采样，再在低分辨率上叠同样的多趟核。
        // 降采样用 radius=0 的 9-tap：半分辨率下输出像素中心恰好落在源 texel 角上，
        // 一次双线性采样就是 2×2 box 平均（天然抗锯齿），不产生额外模糊。
        GLuint src = sourceTex;
        int srcW = targetW_;
        int srcH = targetH_;
        for (int l = 0; l <= level; l++) {
            const int lw = blurDownW_[l];
            const int lh = blurDownH_[l];
            glBindFramebuffer(GL_FRAMEBUFFER, blurDownFbo_[l][0]);
            glViewport(0, 0, lw, lh);
            glDisable(GL_BLEND);
            glUseProgram(progBlur_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src);
            glUniform1i(glGetUniformLocation(progBlur_, "uSource"), 0);
            glUniform2f(glGetUniformLocation(progBlur_, "uTexel"),
                        1.0f / static_cast<float>(srcW), 1.0f / static_cast<float>(srcH));
            glUniform2f(glGetUniformLocation(progBlur_, "uDir"), 1.0f, 0.0f);
            glUniform1f(glGetUniformLocation(progBlur_, "uRadius"), 0.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            src = blurDownTex_[l][0];
            srcW = lw;
            srcH = lh;
        }
        // 低分辨率下 1 texel = 2^(level+1) 屏幕 px，σ 要按同一比例换算
        const float sigmaAtLevel = radiusPx / static_cast<float>(1 << (level + 1));
        result = BlurIterations(src, sigmaAtLevel, blurDownW_[level], blurDownH_[level],
                                blurDownFbo_[level], blurDownTex_[level]);
    }

    if (scissorOn) {
        glEnable(GL_SCISSOR_TEST);
    }
    return result;
}

// ===========================================================================
// 目录场景：通用绘制原语
// ===========================================================================

void GlesRenderer::BlitTexture(GLuint tex, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glUseProgram(progCopy_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(progCopy_, "uSource"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void GlesRenderer::CopyBackgroundToScene(int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glUseProgram(progCopy_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bgTex_);
    glUniform1i(glGetUniformLocation(progCopy_, "uSource"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

GlesRenderer::GlassSpec GlesRenderer::DefaultGlass(float x, float y, float w, float h, float radius,
                                                   float d) const {
    GlassSpec s;
    s.x = x;
    s.y = y;
    s.w = w;
    s.h = h;
    s.rTL = radius;
    s.rTR = radius;
    s.rBR = radius;
    s.rBL = radius;
    s.highlight = 0.5f;   // Highlight.Default（白 alpha 0.5，加法混合）
    s.highlightWidthPx = std::max(1.0f, std::ceil(0.5f * d));
    s.highlightBlurPx = s.highlightWidthPx * 0.5f;
    s.hasShadow = true;
    s.shadowBlurPx = 24.0f * d;   // Shadow.Default：radius 24dp / offset radius/6 / 黑 0.1
    s.shadowOffsetPx = 4.0f * d;
    s.shadowAlpha = 0.1f;
    return s;
}

GlesRenderer::GlassSpec GlesRenderer::KnobGlass(float x, float y, float w, float h, float d,
                                                float press) const {
    // 开关（Toggle）与滑块（Slider）的旋钮**共用**这一套材质：两个场景长得一样，
    // 参数分头改很容易漂移，统一放这里（要调就一起调）。
    // - 不要任何阴影：外阴影会在按压时围出一圈灰边，内阴影会在边缘压暗
    // - 静止时是纯白旋钮；按下后白面**很快**褪干净（不留选中底色 / 表面白），只留纯玻璃
    // - 折射半径稍小、强度按需：边缘的"液态"弯折
    GlassSpec s = DefaultGlass(x, y, w, h, h * 0.5f, d);
    s.hasShadow = false;
    s.shadowAlpha = 0.0f;
    s.innerAlpha = 0.0f;
    s.surfaceAlpha = std::max(0.0f, 1.0f - 1.5f * press);
    s.blurRadiusPx = 8.0f * d * (1.0f - press);
    s.refractionHeight = 4.0f * d * press;
    s.refractionAmount = 15.0f * d * press;
    s.dispersion = true;
    s.highlight = 0.0f;
    s.ambient = press > 0.001f;
    s.ambientIntensity = 0.38f * press;
    s.highlightWidthPx = std::max(1.0f, std::ceil(0.333f * d));
    s.highlightBlurPx = s.highlightWidthPx * 0.5f;
    // 按压采样缩小：绕旋钮中心（形状的按压缩放也绕中心，两者同心）
    const float sampleScale = 1.0f - kKnobPressSampleShrink * press;
    s.sampleScaleX = sampleScale;
    s.sampleScaleY = sampleScale;
    s.samplePivotX = x + w * 0.5f;
    s.samplePivotY = y + h * 0.5f;
    return s;
}

void GlesRenderer::DrawSolid(const SolidSpec &spec, int w, int h) {
    GLuint prog = progSolid_;
    if (prog == 0) return;
    // 混合状态由本函数自己保证：调用方可能刚画过文字（那边会关混合），
    // 若混着用，未乘覆盖率的 RGB 会整屏写出去（放大镜光标就踩过这个坑）。
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    float rTL = spec.r;
    float rTR = spec.r;
    float rBR = spec.r;
    float rBL = spec.r;
    if (spec.r <= 0.0f) {
        rTL = spec.rTL;
        rTR = spec.rTR;
        rBR = spec.rBR;
        rBL = spec.rBL;
    }
    glUniform2f(glGetUniformLocation(prog, "uResolution"), static_cast<float>(w),
                static_cast<float>(h));
    glUniform4f(glGetUniformLocation(prog, "uRect"), spec.x, spec.y, spec.w, spec.h);
    glUniform4f(glGetUniformLocation(prog, "uRadii"), rTL, rTR, rBR, rBL);
    glUniform4f(glGetUniformLocation(prog, "uColor"), spec.colorR, spec.colorG, spec.colorB,
                spec.a);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void GlesRenderer::DrawSprite(GLuint tex, float x, float y, float w, float h, int screenW,
                              int screenH, float alpha) {
    if (tex == 0 || w <= 0.0f || h <= 0.0f) {
        return;
    }
    // 位图是**预乘 alpha**：混合函数固定 (ONE, ONE_MINUS_SRC_ALPHA)，由本函数保证
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(progSprite_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(progSprite_, "uSprite"), 0);
    glUniform2f(glGetUniformLocation(progSprite_, "uResolution"), static_cast<float>(screenW),
                static_cast<float>(screenH));
    glUniform4f(glGetUniformLocation(progSprite_, "uRect"), x, y, w, h);
    glUniform1f(glGetUniformLocation(progSprite_, "uAlpha"), alpha);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

GLuint GlesRenderer::BlurFor(GLuint sourceTex, float radiusPx, int w, int h) {
    if (radiusPx <= 0.01f) {
        return sourceTex;
    }
    if (blurCacheTex_ != 0 && blurCacheSrc_ == sourceTex &&
        std::fabs(radiusPx - blurCacheRadius_) < 0.01f) {
        return blurCacheTex_;
    }
    blurCacheTex_ = RenderBlur(sourceTex, radiusPx);
    blurCacheRadius_ = radiusPx;
    blurCacheSrc_ = sourceTex;
    return blurCacheTex_;
}

void GlesRenderer::DrawGlass(const GlassSpec &s, GLuint targetFbo, int w, int h) {
    // 形状被 scaleX/scaleY 放大时，笔刷范围要跟着外扩，否则旋钮上下会被 scissor 切平。
    const float scalePad = 0.5f * std::max(s.w * std::max(0.0f, s.scaleX - 1.0f),
                                           s.h * std::max(0.0f, s.scaleY - 1.0f));
    const float shadowPad =
        std::max(2.0f, s.shadowBlurPx + std::fabs(s.shadowOffsetPx) + 4.0f) + scalePad;
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    SetScissorRect(s.x, s.y, s.w, s.h, shadowPad, w, h);
    // ⚠️ 顺序：BlurFor 会切到模糊 program，必须先把模糊算出来再 useProgram(progGlass_)。
    // 反过来的话 glUniform* 会按不同 program 的 location 提交（GL_INVALID_OPERATION），
    // 而且 glDrawArrays 用的是模糊 program —— 按钮/旋钮会直接画不出来。
    const GLuint backdropTex = s.useBackground ? bgTex_ : sceneTex_;
    const GLuint blurredTex = BlurFor(backdropTex, s.blurRadiusPx, w, h);
    // ⚠️ BlurFor（RenderBlur）会把 framebuffer / viewport 留在模糊链的目标上，混合也被关掉，
    // 所以这里必须把状态**整套重新设一遍**：否则这一笔玻璃会画进模糊 FBO（屏幕上什么都不出现）。
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(progGlass_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, backdropTex);
    glUniform1i(glGetUniformLocation(progGlass_, "uBackdrop"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blurredTex);
    glUniform1i(glGetUniformLocation(progGlass_, "uBlurred"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, labelTex_);
    glUniform1i(glGetUniformLocation(progGlass_, "uLabels"), 2);
    glUniform2f(glGetUniformLocation(progGlass_, "uLabelSize"), 1.0f, 1.0f);
    glUniform2f(glGetUniformLocation(progGlass_, "uResolution"), static_cast<float>(w),
                static_cast<float>(h));
    glUniform2f(glGetUniformLocation(progGlass_, "uTexel"), 1.0f / static_cast<float>(w),
                1.0f / static_cast<float>(h));
    glUniform4f(glGetUniformLocation(progGlass_, "uGlassRect"), s.x, s.y, s.w, s.h);
    glUniform4f(glGetUniformLocation(progGlass_, "uRadii"), s.rTL, s.rTR, s.rBR, s.rBL);
    glUniform1f(glGetUniformLocation(progGlass_, "uScaleX"), s.scaleX);
    glUniform1f(glGetUniformLocation(progGlass_, "uScaleY"), s.scaleY);
    glUniform1f(glGetUniformLocation(progGlass_, "uRefractionHeight"), s.refractionHeight);
    glUniform1f(glGetUniformLocation(progGlass_, "uRefractionAmount"), -s.refractionAmount);
    glUniform1f(glGetUniformLocation(progGlass_, "uDispersion"), s.dispersion ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uDepthEffect"), s.depthEffect ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uSurfaceAlpha"), s.surfaceAlpha);
    glUniform3f(glGetUniformLocation(progGlass_, "uSurfaceColor"), s.surfaceR, s.surfaceG,
                s.surfaceB);
    glUniform1f(glGetUniformLocation(progGlass_, "uBlurRadius"), s.blurRadiusPx);
    glUniform1f(glGetUniformLocation(progGlass_, "uVibrancy"), s.vibrancy);
    glUniform1f(glGetUniformLocation(progGlass_, "uBrightness"), s.brightness);
    glUniform1f(glGetUniformLocation(progGlass_, "uContrast"), s.contrast);
    glUniform1f(glGetUniformLocation(progGlass_, "uSaturation"), s.saturation);
    glUniform1f(glGetUniformLocation(progGlass_, "uEffectOpacity"), s.effectOpacity);
    glUniform1f(glGetUniformLocation(progGlass_, "uPlainHighlight"), s.plainHighlight ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uHighlight"), s.highlight);
    glUniform1f(glGetUniformLocation(progGlass_, "uHighlightWidth"), s.highlightWidthPx);
    glUniform1f(glGetUniformLocation(progGlass_, "uHighlightBlur"), s.highlightBlurPx);
    glUniform1f(glGetUniformLocation(progGlass_, "uAmbient"), s.ambient ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uAmbientIntensity"), s.ambientIntensity);
    glUniform1f(glGetUniformLocation(progGlass_, "uShadowBlur"), s.shadowBlurPx);
    glUniform1f(glGetUniformLocation(progGlass_, "uShadowOffset"), s.shadowOffsetPx);
    glUniform1f(glGetUniformLocation(progGlass_, "uShadowAlpha"), s.shadowAlpha);
    glUniform1f(glGetUniformLocation(progGlass_, "uShadow"), s.hasShadow ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uInnerRadius"), s.innerRadiusPx);
    glUniform1f(glGetUniformLocation(progGlass_, "uInnerAlpha"), s.innerAlpha);
    glUniform2f(glGetUniformLocation(progGlass_, "uSampleScale"), s.sampleScaleX, s.sampleScaleY);
    glUniform2f(glGetUniformLocation(progGlass_, "uSamplePivot"), s.samplePivotX, s.samplePivotY);
    glUniform1f(glGetUniformLocation(progGlass_, "uShowLabels"), 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uOverlayMode"), 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
}

// ===========================================================================
// 目录场景：文字层
// ===========================================================================

bool GlesRenderer::UploadContentText(int w, int h) {
    std::vector<TextRun> runs;
    if (!BuildContentText(scene_.load(), w, h, runs) || runs.empty()) {
        contentTextW_ = 0;
        contentTextH_ = 0;
        return false;
    }
    LabelBitmap bitmap;
    if (!RenderTextOverlay(contentTextW_, contentTextH_, runs, bitmap)) {
        contentTextW_ = 0;
        contentTextH_ = 0;
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, contentTextTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bitmap.width, bitmap.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, bitmap.pixels.data());
    SetupTexture(contentTextTex_);
    GLOG("content text texture %{public}dx%{public}d uploaded", bitmap.width, bitmap.height);
    return true;
}

bool GlesRenderer::UploadOverlayText(int w, int h) {
    std::vector<TextRun> runs;
    if (!BuildOverlayText(scene_.load(), w, h, runs) || runs.empty()) {
        overlayTextW_ = 0;
        overlayTextH_ = 0;
        return false;
    }
    LabelBitmap bitmap;
    if (!RenderTextOverlay(overlayTextW_, overlayTextH_, runs, bitmap)) {
        overlayTextW_ = 0;
        overlayTextH_ = 0;
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, overlayTextTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bitmap.width, bitmap.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, bitmap.pixels.data());
    SetupTexture(overlayTextTex_);
    GLOG("overlay text texture %{public}dx%{public}d uploaded", bitmap.width, bitmap.height);
    return true;
}

void GlesRenderer::DrawContentText(int w, int h) {
    if (contentTextTex_ == 0 || contentTextW_ <= 0 || contentTextH_ <= 0) {
        return;
    }
    DrawSprite(contentTextTex_, contentTextX_, contentTextY_, static_cast<float>(contentTextW_),
               static_cast<float>(contentTextH_), w, h);
}

void GlesRenderer::DrawOverlayText(int w, int h) {
    if (overlayTextTex_ == 0 || overlayTextW_ <= 0 || overlayTextH_ <= 0) {
        return;
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    DrawSprite(overlayTextTex_, overlayTextX_, overlayTextY_, static_cast<float>(overlayTextW_),
               static_cast<float>(overlayTextH_), w, h);
    glDisable(GL_BLEND);
}

bool GlesRenderer::BuildContentText(int scene, int w, int h, std::vector<TextRun> &runs) {
    const float d = params_.Density();
    contentTextX_ = 0.0f;
    contentTextY_ = 0.0f;
    contentTextW_ = w;
    contentTextH_ = h;
    const float sh = static_cast<float>(h);

    if (scene == static_cast<int>(Scene::kMagnifier)) {
        // 卡片上的正文（放大镜要折射它）
        const RectF card = MagCardRect(w, h, d);
        TextRun run;
        run.text = kLoremIpsum;
        run.x = card.x + 24.0f * d;
        run.y = sh - (card.y + card.h) + 24.0f * d;
        run.wrapWidth = card.w - 48.0f * d;
        run.fontSizePx = 16.0f * d;
        run.argb = 0xFFFFFFFF;
        run.align = 0;
        runs.push_back(run);
        return true;
    }
    return false;
}

bool GlesRenderer::BuildOverlayText(int scene, int w, int h, std::vector<TextRun> &runs) {
    const float d = params_.Density();
    overlayTextX_ = 0.0f;
    overlayTextY_ = 0.0f;
    overlayTextW_ = w;
    overlayTextH_ = h;
    const float sh = static_cast<float>(h);

    switch (scene) {
        case static_cast<int>(Scene::kButtons): {
            for (int i = 0; i < 4; i++) {
                const RectF r = ButtonRectAt(w, h, d, i);
                TextRun run;
                run.text = kButtonLabels[i];
                run.x = r.x + r.w * 0.5f;
                run.y = sh - (r.y + r.h) + (r.h - 15.0f * d * 1.35f) * 0.5f;
                run.fontSizePx = 15.0f * d;
                run.argb = 0xFFFFFFFF;
                run.align = 1;
                runs.push_back(run);
            }
            return true;
        }
        case static_cast<int>(Scene::kDialog): {
            const RectF p = DialogPanelRect(w, h, d);
            const float panelTop = sh - (p.y + p.h);
            const float titleBlock = 24.0f * d * 1.35f;
            TextRun title;
            title.text = "Dialog Title";
            title.x = p.x + 28.0f * d;
            title.y = panelTop + 24.0f * d;
            title.fontSizePx = 24.0f * d;
            title.argb = 0xFFFFFFFF;
            title.align = 0;
            runs.push_back(title);

            TextRun body;
            body.text = kLoremIpsum;
            body.x = p.x + 24.0f * d;
            body.y = panelTop + 24.0f * d + titleBlock + 12.0f * d;
            body.wrapWidth = p.w - 48.0f * d;
            body.fontSizePx = 15.0f * d;
            body.argb = 0xADFFFFFF;   // 白 alpha 0.68
            body.align = 0;
            body.maxLines = 5;
            runs.push_back(body);

            RectF buttons[2];
            DialogButtonRects(w, h, d, buttons);
            const char *labels[2] = {"Cancel", "Okay"};
            for (int i = 0; i < 2; i++) {
                TextRun label;
                label.text = labels[i];
                label.x = buttons[i].x + buttons[i].w * 0.5f;
                label.y = sh - (buttons[i].y + buttons[i].h) +
                          (buttons[i].h - 16.0f * d * 1.35f) * 0.5f;
                label.fontSizePx = 16.0f * d;
                label.argb = 0xFFFFFFFF;
                label.align = 1;
                runs.push_back(label);
            }
            return true;
        }
        case static_cast<int>(Scene::kLockScreen): {
            const RectF p = LockPanelRect(w, h, d);
            const float panelTop = sh - (p.y + p.h);
            TextRun clock;
            clock.text = "09:41";
            clock.x = p.x + p.w * 0.5f;
            clock.y = panelTop + 12.0f * d;
            clock.fontSizePx = 88.0f * d;
            clock.argb = 0xFFFFFFFF;
            clock.align = 1;
            clock.bold = true;
            runs.push_back(clock);

            TextRun date;
            date.text = "Monday, January 1";
            date.x = p.x + p.w * 0.5f;
            date.y = clock.y + 88.0f * d * 1.35f;
            date.fontSizePx = 20.0f * d;
            date.argb = 0xCCFFFFFF;
            date.align = 1;
            runs.push_back(date);
            return true;
        }
        case static_cast<int>(Scene::kProgressiveBlur): {
            const RectF band = ProgBlurBandRect(w, h, d);
            TextRun run;
            run.text = "alpha-masked progressive blur";
            run.x = static_cast<float>(w) * 0.5f;
            run.y = sh - (band.y + band.h) + (band.h - 16.0f * d * 1.35f) * 0.5f;
            run.fontSizePx = 16.0f * d;
            run.argb = 0xFFFFFFFF;
            run.align = 1;
            runs.push_back(run);
            return true;
        }
        case static_cast<int>(Scene::kAdaptiveLuminance): {
            const RectF p = AdaptivePanelRect(w, h, d);
            overlayTextX_ = p.x;
            overlayTextY_ = p.y;
            overlayTextW_ = static_cast<int>(p.w);
            overlayTextH_ = static_cast<int>(p.h);
            const uint32_t argb = (lumValue_ > 0.5f) ? 0xFF000000 : 0xFFFFFFFF;
            TextRun first;
            first.text = "luminance:";
            first.x = p.w * 0.5f;
            first.y = p.h * 0.5f - 22.0f * d;
            first.fontSizePx = 16.0f * d;
            first.argb = argb;
            first.align = 1;
            runs.push_back(first);

            char buffer[32] = {0};
            snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(lumValue_));
            TextRun second;
            second.text = buffer;
            second.x = p.w * 0.5f;
            second.y = p.h * 0.5f + 2.0f * d;
            second.fontSizePx = 16.0f * d;
            second.argb = argb;
            second.align = 1;
            runs.push_back(second);
            return true;
        }
        case static_cast<int>(Scene::kPlayground): {
            // 薄片收起时只画按钮文字（滑块整组都不画）
            if (pgSheetOpen_) {
                static const char *kLabels[5] = {"Corner radius", "Blur radius",
                                                 "Refraction height", "Refraction amount",
                                                 "Chromatic aberration"};
                for (int i = 0; i < 5; i++) {
                    const RectF row = PgSliderRowRect(i, w, h, d);
                    TextRun run;
                    run.text = kLabels[i];
                    run.x = row.x;
                    run.y = sh - (row.y + row.h + 15.0f * d * 1.35f);
                    run.fontSizePx = 15.0f * d;
                    // 薄片表面是 White 0.5（浅色），文字用深色才看得见
                    run.argb = 0xFF1C1C1E;
                    run.align = 0;
                    runs.push_back(run);
                }
            }
            for (int i = 0; i < 2; i++) {
                const RectF button = PgButtonRect(i, pgSheetOpen_, w, h, d);
                TextRun run;
                run.text = PgButtonLabel(i, pgSheetOpen_);
                run.x = button.x + button.w * 0.5f;
                run.y = sh - (button.y + button.h) + (button.h - 15.0f * d * 1.35f) * 0.5f;
                run.fontSizePx = 15.0f * d;
                run.argb = 0xFFFFFFFF;
                run.align = 1;
                runs.push_back(run);
            }
            return true;
        }
        default:
            return false;
    }
}

// ===========================================================================
// 目录场景：各场景绘制
// ===========================================================================

void GlesRenderer::DrawButtonsScene(int w, int h) {
    const float d = params_.Density();
    float press[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        for (int i = 0; i < 4; i++) {
            press[i] = scenePress_[i];
        }
    }
    BlitTexture(bgTex_, w, h);
    for (int i = 0; i < 4; i++) {
        const RectF r = ButtonRectAt(w, h, d, i);
        GlassSpec s = DefaultGlass(r.x, r.y, r.w, r.h, r.h * 0.5f, d);
        // LiquidButton：vibrancy + blur(2dp) + lens(12dp, 24dp)
        s.vibrancy = 1.5f;
        s.blurRadiusPx = 2.0f * d;
        s.refractionHeight = 12.0f * d;
        s.refractionAmount = 24.0f * d;
        if (i == 1) {
            s.surfaceAlpha = 0.3f;   // surfaceColor = White 0.3
        } else if (i == 2) {
            s.surfaceAlpha = 0.8f;   // tint = #0088FF
            s.surfaceR = kAccentR;
            s.surfaceG = kAccentG;
            s.surfaceB = kAccentB;
        } else if (i == 3) {
            s.surfaceAlpha = 0.8f;   // tint = #FF8D28
            s.surfaceR = 1.0f;
            s.surfaceG = 0.553f;
            s.surfaceB = 0.157f;
        }
        // 按压：scale = lerp(1, 1 + 4dp / height, progress)
        const float scale = 1.0f + (4.0f * d) / r.h * press[i];
        s.scaleX = scale;
        s.scaleY = scale;
        // 按压采样缩小：形状放大 + 内容回缩，绕按钮中心（与顶部按钮 / 旋钮同量级）
        const float btnSampleScale = 1.0f - kButtonPressSampleShrink * press[i];
        s.sampleScaleX = btnSampleScale;
        s.sampleScaleY = btnSampleScale;
        s.samplePivotX = r.x + r.w * 0.5f;
        s.samplePivotY = r.y + r.h * 0.5f;
        DrawGlass(s, 0, w, h);
    }
    DrawOverlayText(w, h);
}

void GlesRenderer::DrawToggleScene(int w, int h) {
    const float d = params_.Density();
    float fraction = 0.0f;
    float press = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float velocity = 0.0f;
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        fraction = toggleFraction_;
        press = togglePress_;
        scaleX = toggleScaleX_;
        scaleY = toggleScaleY_;
        velocity = toggleVelocity_;
    }
    const RectF track = ToggleTrackRect(w, h, d);
    const RectF knob = ToggleKnobRect(w, h, d, fraction);

    // ---- 内容层：轨道底色（选中进度插值）----
    // 参考实现 `lerp(trackColor, accentColor, fraction)` 把 **alpha 也一起插值**
    // （未选中 #787880@0.36 -> 选中 #30D158@1）。之前固定 0.36，打开后轨道一直是
    // 半透明灰绿，出不来参考图那种实心亮绿。
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SolidSpec trackSolid;
    trackSolid.x = track.x;
    trackSolid.y = track.y;
    trackSolid.w = track.w;
    trackSolid.h = track.h;
    trackSolid.r = track.h * 0.5f;
    trackSolid.a = kTrackAlpha + (1.0f - kTrackAlpha) * fraction;
    trackSolid.colorR = kTrackR + (0.188f - kTrackR) * fraction;   // #30D158
    trackSolid.colorG = kTrackG + (0.820f - kTrackG) * fraction;
    trackSolid.colorB = kTrackB + (0.345f - kTrackB) * fraction;
    DrawSolid(trackSolid, w, h);
    glDisable(GL_BLEND);
    BlitTexture(sceneTex_, w, h);

    // ---- 玻璃旋钮：材质与 Toggle 旋钮共用（见 KnobGlass），按压/拖动表现单独调 ----
    GlassSpec s = KnobGlass(knob.x, knob.y, knob.w, knob.h, d, press);
    // 移动时的拉长/压扁（比参考实现更明显：系数 1.0 / 0.4，限幅 ±0.3）
    const float vel = velocity / 50.0f;
    const float deformX =
        std::max(0.1f, scaleX / (1.0f - std::max(-0.3f, std::min(0.3f, vel * 1.0f))));
    const float deformY =
        std::max(0.1f, scaleY * (1.0f - std::max(-0.3f, std::min(0.3f, vel * 0.4f))));
    s.scaleX = deformX;
    s.scaleY = deformY;
    DrawGlass(s, 0, w, h);
}

void GlesRenderer::DrawSliderScene(int w, int h) {
    const float d = params_.Density();
    float value = 50.0f;
    float animValue = 50.0f;
    float press = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float velocity = 0.0f;
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        value = sliderValue_;
        animValue = sliderValueAnim_;
        press = sliderPress_;
        scaleX = sliderScaleX_;
        scaleY = sliderScaleY_;
        velocity = sliderVelocity_;
    }
    const float progress = std::max(0.0f, std::min(1.0f, value / 100.0f));
    // 旋钮位置用**动画值**（滞后 + 回弹），轨道填充用目标值（读数不滞后）
    const float knobProgress = std::max(0.0f, std::min(1.0f, animValue / 100.0f));
    const RectF track = SliderTrackRect(w, h, d);
    const RectF knob = SliderKnobRect(w, h, d, knobProgress);

    // ---- 内容层：轨道 + 已完成填充 ----
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SolidSpec trackSolid;
    trackSolid.x = track.x;
    trackSolid.y = track.y;
    trackSolid.w = track.w;
    trackSolid.h = track.h;
    trackSolid.r = track.h * 0.5f;
    trackSolid.a = kTrackAlpha;
    trackSolid.colorR = kTrackR;
    trackSolid.colorG = kTrackG;
    trackSolid.colorB = kTrackB;
    DrawSolid(trackSolid, w, h);

    SolidSpec fillSolid = trackSolid;
    fillSolid.w = track.w * progress;
    fillSolid.a = 1.0f;
    fillSolid.colorR = kAccentR;
    fillSolid.colorG = kAccentG;
    fillSolid.colorB = kAccentB;
    DrawSolid(fillSolid, w, h);
    glDisable(GL_BLEND);
    BlitTexture(sceneTex_, w, h);

    // ---- 玻璃旋钮：材质与 Toggle 旋钮共用（见 KnobGlass），缩放/形变单独调 ----
    GlassSpec s = KnobGlass(knob.x, knob.y, knob.w, knob.h, d, press);
    // 折射比 Toggle 强：滑块旋钮更宽（40dp），边缘有足够的行程做"液态"弯折。
    // 高度 10dp 已接近旋钮半高（12dp）—— 拖动时几乎整块都在折射带里，透镜感拉满。
    s.refractionHeight = 10.0f * d * press;
    s.refractionAmount = 24.0f * d * press;
    // 拖动时的拉长/压扁：速度越大越"流"，横向拉长、纵向压扁（限幅 ±0.3）
    const float vel = velocity / 450.0f;
    const float stretch = std::max(-0.3f, std::min(0.3f, vel));
    s.scaleX = std::max(0.1f, scaleX * (1.0f + stretch));
    s.scaleY = std::max(0.1f, scaleY * (1.0f - stretch * 0.4f));
    DrawGlass(s, 0, w, h);
}

void GlesRenderer::DrawDialogScene(int w, int h) {
    const float d = params_.Density();
    float press[2] = {0.0f, 0.0f};
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        press[0] = dialogPress_[0];
        press[1] = dialogPress_[1];
    }
    const RectF panel = DialogPanelRect(w, h, d);

    // ---- 内容层：暗化（dimColor = #121212 alpha 0.56）----
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SolidSpec dim;
    dim.x = 0.0f;
    dim.y = 0.0f;
    dim.w = static_cast<float>(w);
    dim.h = static_cast<float>(h);
    dim.a = 0.56f;
    dim.colorR = 0.0706f;
    dim.colorG = 0.0706f;
    dim.colorB = 0.0706f;
    DrawSolid(dim, w, h);
    glDisable(GL_BLEND);
    BlitTexture(sceneTex_, w, h);

    // ---- 玻璃面板（drawBackdrop: colorControls + blur(8dp) + lens(24dp, 48dp, depth)）----
    GlassSpec s = DefaultGlass(panel.x, panel.y, panel.w, panel.h, 48.0f * d, d);
    s.saturation = 1.5f;
    s.blurRadiusPx = 8.0f * d;
    s.refractionHeight = 24.0f * d;
    s.refractionAmount = 48.0f * d;
    s.depthEffect = true;
    s.plainHighlight = true;
    s.highlight = kPlainHighlight;   // Highlight.Plain（见 kPlainHighlight 的配平说明）
    s.surfaceAlpha = 0.4f;
    s.surfaceR = 0.0706f;  // containerColor = #121212 alpha 0.4
    s.surfaceG = 0.0706f;
    s.surfaceB = 0.0706f;
    DrawGlass(s, 0, w, h);

    // ---- 按钮（纯色胶囊，参考实现里不是玻璃）----
    RectF buttons[2];
    DialogButtonRects(w, h, d, buttons);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int i = 0; i < 2; i++) {
        SolidSpec button;
        const float scale = 1.0f + 0.02f * press[i];
        const float bw = buttons[i].w * scale;
        const float bh = buttons[i].h * scale;
        button.x = buttons[i].x + (buttons[i].w - bw) * 0.5f;
        button.y = buttons[i].y + (buttons[i].h - bh) * 0.5f;
        button.w = bw;
        button.h = bh;
        button.r = bh * 0.5f;
        if (i == 0) {
            // containerColor.copy(alpha = 0.2f) → #121212 @ 0.4 × 0.2 = 0.08
            button.a = 0.08f;
            button.colorR = 0.0706f;
            button.colorG = 0.0706f;
            button.colorB = 0.0706f;
        } else {
            button.a = 1.0f;
            button.colorR = kAccentR;
            button.colorG = kAccentG;
            button.colorB = kAccentB;
        }
        DrawSolid(button, w, h);
    }
    glDisable(GL_BLEND);
    DrawOverlayText(w, h);
}

void GlesRenderer::DrawControlCenterScene(int w, int h) {
    const float d = params_.Density();
    float progress = 1.0f;
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        progress = ccProgress_;
    }
    const float sp = 16.0f * d;
    const float item = 68.0f * d;
    const float two = 152.0f * d;
    const float gridW = two * 2.0f + sp;
    const float x0 = (static_cast<float>(w) - gridW) * 0.5f;
    const float gridTopDown = 140.0f * d;
    const float yTop = static_cast<float>(h) - gridTopDown;
    const float yShift = 48.0f * d * (1.0f - progress);

    // ---- 内容层：背景模糊 + 暗化（对齐参考实现的 renderEffect + dim）----
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const float bgBlur = 4.0f * d * progress;
    if (bgBlur > 0.01f) {
        const GLuint blurred = RenderBlur(bgTex_, bgBlur);
        DrawSprite(blurred, 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), w, h,
                   progress);
    }
    SolidSpec dim;
    dim.x = 0.0f;
    dim.y = 0.0f;
    dim.w = static_cast<float>(w);
    dim.h = static_cast<float>(h);
    dim.a = 0.4f * progress;
    dim.colorR = 0.0f;
    dim.colorG = 0.0f;
    dim.colorB = 0.0f;
    DrawSolid(dim, w, h);
    glDisable(GL_BLEND);
    BlitTexture(sceneTex_, w, h);

    // ---- 玻璃圆角方块（cap 形状 = 34dp 半径；无投影，Default 高光）----
    const float radius = item * 0.5f;
    auto drawTile = [&](float gx, float gy, float tw, float th) {
        GlassSpec s = DefaultGlass(x0 + gx, yTop - gy - th + yShift, tw, th, radius, d);
        s.hasShadow = false;
        s.vibrancy = 1.5f;
        s.refractionHeight = 24.0f * d * progress;
        s.refractionAmount = 48.0f * d * progress;
        s.depthEffect = true;
        // 收起进度同时驱动"材质淡出"（对齐参考实现的 alpha = EaseIn(progress)）
        s.effectOpacity = progress;
        // 参考实现的控制中心没有底色填充
        DrawGlass(s, 0, w, h);
    };
    const float row2 = two + sp;
    const float row3 = row2 + two + sp;
    // 第一行
    drawTile(0.0f, 0.0f, two, two);
    drawTile(two + sp, 0.0f, two, two);
    // 第二行
    drawTile(0.0f, row2, item, item);
    drawTile(item + sp, row2, item, item);
    drawTile(0.0f, row2 + item + sp, two, item);
    drawTile(two + sp, row2, item, two);
    drawTile(two + sp + item + sp, row2, item, two);
    // 第三行
    drawTile(0.0f, row3, two, two);
    drawTile(two + sp, row3, item, item);
    drawTile(two + sp + item + sp, row3, item, item);
    drawTile(two + sp, row3 + item + sp, item, item);

    // ---- 图标：大块内的三个内嵌胶囊 + 几个独立方块上的飞机 ----
    if (planeIconTex_ != 0 && planeIconSizePx_ > 0 && progress > 0.01f) {
        const float innerSize = 56.0f * d;
        const float innerIcon = innerSize * 0.8f;
        const float tileIcon = 40.0f * d;
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // 位图是预乘 alpha
        auto drawIconAt = [&](float cx, float cy, float size) {
            DrawSprite(planeIconTex_, cx - size * 0.5f, cy - size * 0.5f, size, size, w, h,
                       progress);
        };
        // 大块内的三个胶囊（白 / 蓝 / 蓝）
        const float innerPad = 16.0f * d;
        const float capsuleCenters[3][2] = {
            {x0 + innerPad + innerSize * 0.5f, yTop - innerPad - innerSize * 0.5f + yShift},
            {x0 + two - innerPad - innerSize * 0.5f, yTop - innerPad - innerSize * 0.5f + yShift},
            {x0 + innerPad + innerSize * 0.5f,
             yTop - two + innerPad + innerSize * 0.5f + yShift},
        };
        const float capsuleColors[3][3] = {
            {1.0f, 1.0f, 1.0f},   // inactive White 0.2
            {kAccentR, kAccentG, kAccentB},
            {kAccentR, kAccentG, kAccentB},
        };
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (int i = 0; i < 3; i++) {
            SolidSpec capsule;
            capsule.x = capsuleCenters[i][0] - innerSize * 0.5f;
            capsule.y = capsuleCenters[i][1] - innerSize * 0.5f;
            capsule.w = innerSize;
            capsule.h = innerSize;
            capsule.r = innerSize * 0.5f;
            // 内嵌胶囊也随收起进度淡出（否则玻璃没了、胶囊还挂在半空）
            capsule.a = ((i == 0) ? 0.2f : 1.0f) * progress;
            capsule.colorR = capsuleColors[i][0];
            capsule.colorG = capsuleColors[i][1];
            capsule.colorB = capsuleColors[i][2];
            DrawSolid(capsule, w, h);
        }
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        for (int i = 0; i < 3; i++) {
            drawIconAt(capsuleCenters[i][0], capsuleCenters[i][1], innerIcon);
        }
        // 独立方块上的飞机
        const float tileCenters[5][2] = {
            {x0 + item * 0.5f, yTop - row2 - item * 0.5f + yShift},
            {x0 + item + sp + item * 0.5f, yTop - row2 - item * 0.5f + yShift},
            {x0 + two + sp + item * 0.5f, yTop - row3 - item * 0.5f + yShift},
            {x0 + two + sp + item + sp + item * 0.5f, yTop - row3 - item * 0.5f + yShift},
            {x0 + two + sp + item * 0.5f, yTop - row3 - item - sp - item * 0.5f + yShift},
        };
        for (int i = 0; i < 5; i++) {
            drawIconAt(tileCenters[i][0], tileCenters[i][1], tileIcon);
        }
        glDisable(GL_BLEND);
    }
}

void GlesRenderer::DrawLockScreenScene(int w, int h) {
    const float d = params_.Density();
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        offsetX = lockOffsetX_;
        offsetY = lockOffsetY_;
    }
    // ---- 内容层：黑 0.3 暗化 ----
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SolidSpec dim;
    dim.x = 0.0f;
    dim.y = 0.0f;
    dim.w = static_cast<float>(w);
    dim.h = static_cast<float>(h);
    dim.a = 0.3f;
    dim.colorR = 0.0f;
    dim.colorG = 0.0f;
    dim.colorB = 0.0f;
    DrawSolid(dim, w, h);
    glDisable(GL_BLEND);
    BlitTexture(sceneTex_, w, h);

    // ---- 玻璃钟表底板（colorControls + blur(2dp)，SDF 时钟的等价物）----
    const RectF base = LockPanelRect(w, h, d);
    const float glOffsetY = -offsetY;   // 屏幕向下拖动 = GL 坐标减
    GlassSpec s = DefaultGlass(base.x + offsetX, base.y + glOffsetY, base.w, base.h,
                               base.h * 0.35f, d);
    s.hasShadow = false;
    s.highlight = 0.0f;
    s.brightness = -0.1f;
    s.contrast = 0.75f;
    s.saturation = 1.5f;
    s.blurRadiusPx = 2.0f * d;
    DrawGlass(s, 0, w, h);

    // ---- 钟表文字（跟着拖动位移）----
    if (overlayTextTex_ != 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        DrawSprite(overlayTextTex_, overlayTextX_ + offsetX, overlayTextY_ + glOffsetY,
                   static_cast<float>(overlayTextW_), static_cast<float>(overlayTextH_), w, h);
        glDisable(GL_BLEND);
    }
}

void GlesRenderer::DrawMagnifierScene(int w, int h) {
    const float d = params_.Density();
    float magX = static_cast<float>(w) * 0.5f;
    float magY = static_cast<float>(h) * 0.42f;
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        if (!magInited_) {
            magX_ = magX;
            magY_ = magY;
            magInited_ = true;
        }
        magX = magX_;
        magY = magY_;
    }
    const RectF card = MagCardRect(w, h, d);
    const RectF lens = MagLensRect(magX, magY, d);
    const RectF cursor = MagCursorRect(magX, magY, d);

    // ---- 内容层：卡片 + 正文 + 光标（透镜要放大它们）----
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SolidSpec cardSolid;
    cardSolid.x = card.x;
    cardSolid.y = card.y;
    cardSolid.w = card.w;
    cardSolid.h = card.h;
    cardSolid.r = 32.0f * d;
    cardSolid.a = 0.9f;
    cardSolid.colorR = 0.0706f;
    cardSolid.colorG = 0.0706f;
    cardSolid.colorB = 0.0706f;
    DrawSolid(cardSolid, w, h);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    DrawContentText(w, h);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SolidSpec cursorSolid;
    cursorSolid.x = cursor.x;
    cursorSolid.y = cursor.y;
    cursorSolid.w = cursor.w;
    cursorSolid.h = cursor.h;
    cursorSolid.r = cursor.w * 0.5f;
    cursorSolid.a = 1.0f;
    cursorSolid.colorR = kAccentR;
    cursorSolid.colorG = kAccentG;
    cursorSolid.colorB = kAccentB;
    DrawSolid(cursorSolid, w, h);
    glDisable(GL_BLEND);
    BlitTexture(sceneTex_, w, h);

    // ---- 放大镜（lens(8dp, 24dp, depth, dispersion) + 1.5× 采样 + 内阴影）----
    GlassSpec s = DefaultGlass(lens.x, lens.y, lens.w, lens.h, lens.h * 0.5f, d);
    s.hasShadow = false;
    s.highlight = 0.0f;
    s.refractionHeight = 8.0f * d;
    s.refractionAmount = 24.0f * d;
    s.depthEffect = true;
    s.dispersion = true;
    s.innerRadiusPx = 16.0f * d;
    s.innerAlpha = 0.15f;
    s.sampleScaleX = 1.5f;
    s.sampleScaleY = 1.5f;
    s.samplePivotX = lens.x + lens.w * 0.5f;
    s.samplePivotY = lens.y + lens.h * 0.5f;
    DrawGlass(s, 0, w, h);
}

void GlesRenderer::DrawProgressiveBlurScene(int w, int h) {
    const float d = params_.Density();
    const RectF band = ProgBlurBandRect(w, h, d);
    BlitTexture(bgTex_, w, h);

    const float radius = 4.0f * d;
    const GLuint blurred = (radius > 0.01f) ? RenderBlur(bgTex_, radius) : bgTex_;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    SetScissorRect(band.x, band.y, band.w, band.h, 1.0f, w, h);
    glUseProgram(progProgBlur_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bgTex_);
    glUniform1i(glGetUniformLocation(progProgBlur_, "uSharp"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blurred);
    glUniform1i(glGetUniformLocation(progProgBlur_, "uBlurred"), 1);
    glUniform2f(glGetUniformLocation(progProgBlur_, "uResolution"), static_cast<float>(w),
                static_cast<float>(h));
    glUniform4f(glGetUniformLocation(progProgBlur_, "uRect"), band.x, band.y, band.w, band.h);
    glUniform3f(glGetUniformLocation(progProgBlur_, "uTint"), 0.502f, 0.502f, 0.502f);
    glUniform1f(glGetUniformLocation(progProgBlur_, "uTintIntensity"), 0.8f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SCISSOR_TEST);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    DrawOverlayText(w, h);
    glDisable(GL_BLEND);
}

void GlesRenderer::DrawAdaptiveScene(int w, int h) {
    const float d = params_.Density();
    float lum = 0.25f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        lum = lumValue_;
        offsetX = pgX_;
        offsetY = pgY_;
    }
    const RectF base = AdaptivePanelRect(w, h, d);

    // ---- 亮度采样：每 250ms 读一次背景（16×16 缩略图求平均）----
    const int64_t now = NowMs();
    if (now - lumSampleMs_ > 250) {
        lumSampleMs_ = now;
        const int sampleW = 16;
        const int sampleH = 16;
        int sx = static_cast<int>(base.x + offsetX + base.w * 0.5f) - sampleW / 2;
        int sy = static_cast<int>(base.y + offsetY + base.h * 0.5f) - sampleH / 2;
        sx = std::max(0, std::min(sx, w - sampleW));
        sy = std::max(0, std::min(sy, h - sampleH));
        unsigned char pixels[sampleW * sampleH * 4] = {0};
        glBindFramebuffer(GL_FRAMEBUFFER, bgFbo_);
        glReadPixels(sx, sy, sampleW, sampleH, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        float sum = 0.0f;
        for (int i = 0; i < sampleW * sampleH; i++) {
            const float r = static_cast<float>(pixels[i * 4 + 0]) / 255.0f;
            const float g = static_cast<float>(pixels[i * 4 + 1]) / 255.0f;
            const float b = static_cast<float>(pixels[i * 4 + 2]) / 255.0f;
            sum += 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }
        std::lock_guard<std::mutex> lock(animMutex_);
        lumTarget_ = sum / static_cast<float>(sampleW * sampleH);
    }

    // 文字（数值）最多 10Hz 更新，避免整屏位图重传
    const int pct = static_cast<int>(std::lround(lum * 100.0f));
    if (pct != lumShown_ && now - lumTextMs_ > 100) {
        lumShown_ = pct;
        lumTextMs_ = now;
        overlayTextDirty_.store(true);
    }

    BlitTexture(bgTex_, w, h);

    // ---- 玻璃面板：亮度驱动的 colorControls / blur + lens(24dp, minDim/2, depth) ----
    const float v = lum * 2.0f - 1.0f;
    const float l = (v >= 0.0f ? 1.0f : -1.0f) * v * v;
    GlassSpec s = DefaultGlass(base.x + offsetX, base.y + offsetY, base.w, base.h, 24.0f * d, d);
    s.hasShadow = false;
    s.plainHighlight = true;
    s.highlight = kPlainHighlight;
    s.saturation = 1.5f;
    if (l > 0.0f) {
        s.brightness = 0.1f + (0.5f - 0.1f) * l;
        s.contrast = 1.0f + (0.0f - 1.0f) * l;
        s.blurRadiusPx = (8.0f + (16.0f - 8.0f) * l) * d;
    } else {
        s.brightness = 0.1f + (-0.2f - 0.1f) * (-l);
        s.contrast = 1.0f;
        s.blurRadiusPx = (8.0f + (2.0f - 8.0f) * (-l)) * d;
    }
    s.refractionHeight = 24.0f * d;
    s.refractionAmount = base.w * 0.5f;
    s.depthEffect = true;
    DrawGlass(s, 0, w, h);

    if (overlayTextTex_ != 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        DrawSprite(overlayTextTex_, overlayTextX_ + offsetX, overlayTextY_ + offsetY,
                   static_cast<float>(overlayTextW_), static_cast<float>(overlayTextH_), w, h);
        glDisable(GL_BLEND);
    }
}

void GlesRenderer::DrawScrollScene(int w, int h, bool lazy) {
    const float d = params_.Density();
    float scroll = 0.0f;
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        scroll = listScroll_;
    }
    BlitTexture(bgTex_, w, h);
    const int count = lazy ? 100 : 20;
    for (int i = 0; i < count; i++) {
        const RectF r = ScrollCardRect(i, w, h, d, scroll);
        if (r.y + r.h < -8.0f || r.y > static_cast<float>(h) + 8.0f) {
            continue;
        }
        GlassSpec s = DefaultGlass(r.x, r.y, r.w, r.h, 32.0f * d, d);
        s.vibrancy = 1.5f;
        s.refractionHeight = 16.0f * d;
        s.refractionAmount = 32.0f * d;
        DrawGlass(s, 0, w, h);
    }
}

void GlesRenderer::DrawPlaygroundScene(int w, int h) {
    const float d = params_.Density();
    float px = 0.0f;
    float py = 0.0f;
    bool sheetOpen = true;
    float cornerFrac = 0.5f;
    float blurDp = 0.0f;
    float heightFrac = 0.2f;
    float amountFrac = 0.2f;
    float dispersion = 0.0f;
    float values[5] = {0.5f, 0.0f, 0.2f, 0.2f, 0.0f};
    float sliderPress[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float buttonPress[2] = {0.0f, 0.0f};
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        px = pgX_;
        py = pgY_;
        sheetOpen = pgSheetOpen_;
        for (int i = 0; i < 5; i++) {
            values[i] = pgSliderValue_[i];
            sliderPress[i] = pgSliderPress_[i];
        }
        buttonPress[0] = pgButtonPress_[0];
        buttonPress[1] = pgButtonPress_[1];
    }

    const RectF panelBase = PgPanelRect(w, h, d);
    const RectF sheet = PgSheetRect(w, h, d);
    const float panelW = panelBase.w;
    const float panelH = panelBase.h;
    const float minDim = std::min(panelW, panelH);

    // 滑块值 -> 面板参数（参考实现的 5 个参数：圆角 / 模糊 / 折射高 / 折射强 / 色散）
    cornerFrac = values[0];
    blurDp = values[1] * 32.0f;
    heightFrac = values[2];
    amountFrac = values[3];
    dispersion = values[4];


    // ---- 底部薄片：画进 sceneTex_（采样 bg，避免读写同一纹理），
    //      这样滑块旋钮与玻璃板都能采样到它 ----
    if (sheetOpen) {
        GlassSpec ss = DefaultGlass(sheet.x, sheet.y, sheet.w, sheet.h, 32.0f * d, d);
        ss.useBackground = true;
        ss.plainHighlight = true;
        ss.highlight = kPlainHighlight;
        ss.vibrancy = 1.5f;
        ss.blurRadiusPx = 4.0f * d;
        ss.refractionHeight = 16.0f * d;
        ss.refractionAmount = 32.0f * d;
        ss.surfaceAlpha = 0.5f;
        DrawGlass(ss, sceneFbo_, w, h);
    }
    BlitTexture(sceneTex_, w, h);

    // ---- 玻璃板（drawBackdrop: vibrancy + blur(0..32dp) + lens(..., depth[, dispersion])）----
    {
        const float radius = 128.0f * d * cornerFrac;
        GlassSpec s = DefaultGlass(panelBase.x + px, panelBase.y + py, panelW, panelH,
                                   std::min(radius, panelH * 0.5f), d);
        s.hasShadow = false;
        s.plainHighlight = true;
        s.highlight = kPlainHighlight;
        s.vibrancy = 1.5f;
        s.blurRadiusPx = blurDp * d;
        s.refractionHeight = heightFrac * minDim * 0.5f;
        s.refractionAmount = amountFrac * minDim;
        s.depthEffect = true;
        s.dispersion = dispersion > 0.0f;
        DrawGlass(s, 0, w, h);
    }

    // ---- 薄片上的滑块（轨道纯色 + 玻璃旋钮）----
    if (sheetOpen) {
        for (int i = 0; i < 5; i++) {
            const RectF row = PgSliderRowRect(i, w, h, d);
            const float trackH = 6.0f * d;
            const float knobW = 40.0f * d;
            const float knobH = 24.0f * d;
            const float progress = std::max(0.0f, std::min(1.0f, values[i]));
            // 轨道
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            SolidSpec trackSolid;
            trackSolid.x = row.x;
            trackSolid.y = row.y + (row.h - trackH) * 0.5f;
            trackSolid.w = row.w;
            trackSolid.h = trackH;
            trackSolid.r = trackH * 0.5f;
            trackSolid.a = kTrackAlpha;
            trackSolid.colorR = kTrackR;
            trackSolid.colorG = kTrackG;
            trackSolid.colorB = kTrackB;
            DrawSolid(trackSolid, w, h);
            SolidSpec fillSolid = trackSolid;
            fillSolid.w = row.w * progress;
            fillSolid.a = 1.0f;
            fillSolid.colorR = kAccentR;
            fillSolid.colorG = kAccentG;
            fillSolid.colorB = kAccentB;
            DrawSolid(fillSolid, w, h);
            glDisable(GL_BLEND);

            // 玻璃旋钮（40×24，与 LiquidSlider 一致）
            float knobX = row.x + row.w * progress - knobW * 0.5f;
            knobX = std::max(row.x - knobW * 0.25f,
                             std::min(row.x + row.w - knobW * 0.75f, knobX));
            const float knobY = row.y + (row.h - knobH) * 0.5f;
            const float press = sliderPress[i];
            GlassSpec knob = DefaultGlass(knobX, knobY, knobW, knobH, knobH * 0.5f, d);
            knob.hasShadow = true;
            knob.shadowBlurPx = 4.0f * d;
            knob.shadowOffsetPx = 4.0f * d / 6.0f;
            knob.shadowAlpha = 0.05f;
            knob.surfaceAlpha = 1.0f - press;
            knob.blurRadiusPx = 8.0f * d * (1.0f - press);
            knob.refractionHeight = 10.0f * d * press;
            knob.refractionAmount = 14.0f * d * press;
            knob.dispersion = true;
            knob.highlight = 0.0f;
            knob.ambient = press > 0.001f;
            knob.ambientIntensity = 0.38f * press;
            knob.highlightWidthPx = std::max(1.0f, std::ceil(0.333f * d));
            knob.highlightBlurPx = knob.highlightWidthPx * 0.5f;
            knob.innerRadiusPx = 4.0f * d * press;
            knob.innerAlpha = 0.15f * press;
            const float knobScale = 1.0f + 0.5f * press;
            knob.scaleX = knobScale;
            knob.scaleY = knobScale;
            DrawGlass(knob, 0, w, h);
        }
    }

    // ---- 两个橙色 tinted 按钮 ----
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int i = 0; i < 2; i++) {
        const RectF base = PgButtonRect(i, sheetOpen, w, h, d);
        const float scale = 1.0f + (4.0f * d) / base.h * buttonPress[i];
        GlassSpec s = DefaultGlass(base.x, base.y, base.w, base.h, base.h * 0.5f, d);
        s.vibrancy = 1.5f;
        s.blurRadiusPx = 2.0f * d;
        s.refractionHeight = 12.0f * d;
        s.refractionAmount = 24.0f * d;
        s.surfaceAlpha = 0.8f;
        s.surfaceR = 1.0f;
        s.surfaceG = 0.553f;
        s.surfaceB = 0.157f;
        s.scaleX = scale;
        s.scaleY = scale;
        DrawGlass(s, 0, w, h);
    }
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    DrawOverlayText(w, h);
    glDisable(GL_BLEND);
}

void GlesRenderer::DrawArkTsGlassScene(int w, int h) {
    const float d = params_.Density();
    // 背景（ArkTS 内容纹理，未就绪时退回照片）已由 DrawCatalogScene 拷进 sceneTex_，
    // 玻璃采样 sceneTex_ 即可拿到实时画面；这里先铺屏，再叠两块玻璃。
    BlitTexture(bgTex_, w, h);

    // 几何与 `ArkTsGlassContent` 的 vp 布局对应（见 demo 侧注释）：
    // 大卡片压在中间的三张卡片上，胶囊压在滑块那一行上。
    const float marginH = 20.0f * d;
    const float cardW = static_cast<float>(w) - marginH * 2.0f;
    const float cardTop = 224.0f * d;
    const float cardH = 150.0f * d;
    GlassSpec card = DefaultGlass(marginH, static_cast<float>(h) - cardTop - cardH, cardW, cardH,
                                  28.0f * d, d);
    card.vibrancy = 1.6f;
    card.blurRadiusPx = 1.0f * d;
    card.refractionHeight = 24.0f * d;
    card.refractionAmount = 40.0f * d;
    card.dispersion = true;
    card.surfaceAlpha = 0.06f;
    DrawGlass(card, 0, w, h);

    const float pillTop = 442.0f * d;
    const float pillH = 60.0f * d;
    GlassSpec pill = DefaultGlass(marginH, static_cast<float>(h) - pillTop - pillH, cardW, pillH,
                                  pillH * 0.5f, d);
    pill.vibrancy = 1.4f;
    pill.blurRadiusPx = 0.5f * d;
    pill.refractionHeight = 14.0f * d;
    pill.refractionAmount = 22.0f * d;
    pill.dispersion = true;
    pill.surfaceAlpha = 0.08f;
    DrawGlass(pill, 0, w, h);
}

void GlesRenderer::DrawCatalogScene(int w, int h) {
    blurCacheRadius_ = -1.0f;
    blurCacheSrc_ = 0;
    // 所有目录场景的玻璃都采样 sceneTex_：这里统一先把背景拷进去，
    // 各场景再往上叠自己的内容（暗化 / 卡片 / 轨道）。
    // 之前是各场景自己拷，漏掉的场景（Buttons / Scroll / Adaptive）会采样到**上一帧/上一场景**
    // 残留的 sceneTex_ —— 滚动卡片整块发黑就是这个原因。
    CopyBackgroundToScene(w, h);
    switch (scene_.load()) {
        case static_cast<int>(Scene::kButtons):
            DrawButtonsScene(w, h);
            break;
        case static_cast<int>(Scene::kToggle):
            DrawToggleScene(w, h);
            break;
        case static_cast<int>(Scene::kSlider):
            DrawSliderScene(w, h);
            break;
        case static_cast<int>(Scene::kDialog):
            DrawDialogScene(w, h);
            break;
        case static_cast<int>(Scene::kControlCenter):
            DrawControlCenterScene(w, h);
            break;
        case static_cast<int>(Scene::kLockScreen):
            DrawLockScreenScene(w, h);
            break;
        case static_cast<int>(Scene::kMagnifier):
            DrawMagnifierScene(w, h);
            break;
        case static_cast<int>(Scene::kProgressiveBlur):
            DrawProgressiveBlurScene(w, h);
            break;
        case static_cast<int>(Scene::kAdaptiveLuminance):
            DrawAdaptiveScene(w, h);
            break;
        case static_cast<int>(Scene::kScrollContainer):
            DrawScrollScene(w, h, false);
            break;
        case static_cast<int>(Scene::kLazyScrollContainer):
            DrawScrollScene(w, h, true);
            break;
        case static_cast<int>(Scene::kPlayground):
            DrawPlaygroundScene(w, h);
            break;
        case static_cast<int>(Scene::kArkTsGlass):
            DrawArkTsGlassScene(w, h);
            break;
        default:
            break;
    }
}

void GlesRenderer::RenderBackground(int w, int h) {
    // ---- pass 1: 背景 -> bgTex_ ----
    glBindFramebuffer(GL_FRAMEBUFFER, bgFbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glUseProgram(progBg_);
    glUniform2f(glGetUniformLocation(progBg_, "uResolution"), static_cast<float>(w),
                static_cast<float>(h));
    // kArkTsGlass 场景：背景直接用 ArkTS 内容纹理（还没收到帧时退回照片）
    const bool useContent =
        scene_.load() == static_cast<int>(Scene::kArkTsGlass) && contentValid_;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, useContent ? contentBlitTex_ : photoTex_);
    glUniform1i(glGetUniformLocation(progBg_, "uPhoto"), 0);
    glUniform1f(glGetUniformLocation(progBg_, "uContentMode"), useContent ? 1.0f : 0.0f);
    glUniform2f(glGetUniformLocation(progBg_, "uPhotoSize"), static_cast<float>(photoW_),
                static_cast<float>(photoH_));
    glUniform1f(glGetUniformLocation(progBg_, "uScrollRange"),
                params_.N(pid::kBackgroundScrollRange));
    glUniform1f(glGetUniformLocation(progBg_, "uScroll"), scroll_.load());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void GlesRenderer::DrawFrame(float seconds) {
    const int w = targetW_;
    const int h = targetH_;
    if (w <= 0 || h <= 0) {
        return;
    }
    // 背景两种场景都要：页签场景后续用它的模糊链；目录场景直接铺/采样它。
    RenderBackground(w, h);
    if (scene_.load() == static_cast<int>(Scene::kTabs)) {
        DrawTabsScene(w, h, debugMode_.load());
    } else {
        DrawCatalogScene(w, h);
    }
    glFlush();
}

void GlesRenderer::DrawTabsScene(int w, int h, int mode) {
    const float texelX = 1.0f / static_cast<float>(w);
    const float texelY = 1.0f / static_cast<float>(h);

    // ---- 动画状态快照：后面的 pass 都用这一份（避免各 pass 各自加锁）----
    float animValue = 0.0f;
    float animPress = 0.0f;
    float animSpeed = 0.0f;
    float animScaleX = 1.0f;
    float animScaleY = 1.0f;
    float animGlowPress = 0.0f;
    float btnPress[kIconButtonCount] = {0.0f, 0.0f, 0.0f};
    {
        std::lock_guard<std::mutex> lock(animMutex_);
        animValue = anim_.value;
        animPress = anim_.press;
        animSpeed = anim_.speed;
        animScaleX = anim_.scaleX;
        animScaleY = anim_.scaleY;
        animGlowPress = glowPress_;
        for (int i = 0; i < kIconButtonCount; i++) {
            btnPress[i] = btnPress_[i];
        }
    }

    // ---- pass 2/3: 可分离高斯模糊 ----
    // 小半径走全分辨率两趟；大半径自动改用降采样金字塔（见 RenderBlur）。
    const float blurRadius = (mode == 4) ? 0.0f : params_.Px(pid::kBlurRadius);
    const GLuint blurResult =
        (blurRadius > 0.01f) ? RenderBlur(bgTex_, blurRadius) : static_cast<GLuint>(bgTex_);

    // ---- pass 2b: 按钮专用模糊 ----
    // 上游 GlassIconButton 的模糊是 `blur(iconBlurBase + 底板模糊 × iconBlurScale)`
    // （id 10 + id 1 × id 11），与底板的 0.7dp 差一个量级，共用一张结果会让按钮"过清"。
    // 按钮半径通常较小、且只覆盖按钮带，所以保持全分辨率 + scissor，不用金字塔。
    const float btnBlurRadius =
        (mode == 4) ? 0.0f
                    : (params_.Px(pid::kIconBlurBase) +
                       params_.Px(pid::kBlurRadius) * params_.N(pid::kIconBlurScale));
    const GLuint btnBlurResult =
        (btnBlurRadius > 0.01f) ? RenderButtonBlur(bgTex_, btnBlurRadius)
                                : static_cast<GLuint>(bgTex_);

    // ---- pass 4: 合成（背景 + 底板玻璃 + 文字）-> sceneTex_ ----
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glUseProgram(progGlass_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bgTex_);
    glUniform1i(glGetUniformLocation(progGlass_, "uBackdrop"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blurResult);
    glUniform1i(glGetUniformLocation(progGlass_, "uBlurred"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, labelTex_);
    glUniform1i(glGetUniformLocation(progGlass_, "uLabels"), 2);
    glUniform2f(glGetUniformLocation(progGlass_, "uLabelSize"), static_cast<float>(labelW_),
                static_cast<float>(labelH_));
    glUniform2f(glGetUniformLocation(progGlass_, "uResolution"), static_cast<float>(w),
                static_cast<float>(h));
    glUniform2f(glGetUniformLocation(progGlass_, "uTexel"), texelX, texelY);

    const float marginH = params_.Px(pid::kOuterPaddingH);
    const float marginV = params_.Px(pid::kOuterPaddingV);
    const float barH = params_.Px(pid::kBarHeight);
    if (!floatInited_) {
        // 初始位置：与底板同宽、水平对齐，放在底板**上方**
        floatX_.store(marginH);
        floatY_.store(marginV + barH + 180.0f);
        floatInited_ = true;
    }
    const float innerPad = TabInnerPadPx();
    const float barW = static_cast<float>(w) - marginH * 2.0f;
    const float capInsetPx = IndicatorEndInsetPx();

    const float panelOffset = panelOffsetPx_.load();
    const float tabW = (barW - innerPad * 2.0f) / 3.0f;
    // 底板圆角半径（参数 68）：钳到"高度一半"上限，超过就是胶囊
    const float panelRadius = std::min(params_.Px(pid::kPanelCornerRadius), barH * 0.5f);

    // 底板按压缩放（参数 5，对齐参考实现的 GlassPanel 的 layerBlock）：
    //   scale = lerp(1, 1 + pressGrowth.px / 底板宽, press)
    // 以底板中心为锚点。参考实现里这个缩放只作用于**玻璃层**（文字与指示器不跟着缩放），
    // 但缩量只有百分之几（3dp / 896px ≈ 1.3%），这里让底板矩形整体缩放、文字跟着被拉
    // 1.3% —— 视觉上等价，而且不需要给文字单独做逆变换。
    // 指示范围：极值 20dp / 896px ≈ 2.2%，仍远小于文字距底板边缘的内缩（14dp）。
    const float panelScale =
        1.0f + (barW > 1.0f ? params_.Px(pid::kPressGrowth) / barW : 0.0f) * animPress;
    const float panelW = barW * panelScale;
    const float panelH = barH * panelScale;
    const float panelCx = marginH + panelOffset + barW * 0.5f;
    const float panelCy = marginV + barH * 0.5f;
    glUniform4f(glGetUniformLocation(progGlass_, "uGlassRect"), panelCx - panelW * 0.5f,
                panelCy - panelH * 0.5f, panelW, panelH);
    // 底板四角半径（等值）：参数 68，钳到高度一半；按压时随底板一起放大
    const float panelRadiusPx = panelRadius * panelScale;
    glUniform4f(glGetUniformLocation(progGlass_, "uRadii"), panelRadiusPx, panelRadiusPx,
                panelRadiusPx, panelRadiusPx);
    glUniform1f(glGetUniformLocation(progGlass_, "uRefractionHeight"),
                mode == 2 ? 0.0f : params_.Px(pid::kRefractionHeight));
    // 上游 Lens.kt 送的是 **-refractionAmount**：位移因此指向形状内侧，边缘是"压缩/内折"
    // 的液态玻璃感。直接送正值 = 采样朝形状外 → 边缘外凸成凸透镜。必须同样取负。
    glUniform1f(glGetUniformLocation(progGlass_, "uRefractionAmount"),
                -params_.Px(pid::kRefractionAmount));
    glUniform1f(glGetUniformLocation(progGlass_, "uDispersion"),
                (params_.B(0) && mode != 1) ? 1.0f : 0.0f);   // bool 0 = 底座色散
    // depthEffect（bool 6）：折射方向叠一层径向分量
    glUniform1f(glGetUniformLocation(progGlass_, "uDepthEffect"),
                params_.B(bid::kDepthEffect) ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uSurfaceAlpha"),
                mode == 3 ? 0.0f : params_.N(pid::kContainerAlpha));
    // 目录场景新增的 uniform 在底板 pass 上保持"恒等"值，外观与旧版逐像素一致
    glUniform1f(glGetUniformLocation(progGlass_, "uScaleX"), 1.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uScaleY"), 1.0f);
    glUniform3f(glGetUniformLocation(progGlass_, "uSurfaceColor"), 1.0f, 1.0f, 1.0f);
    glUniform2f(glGetUniformLocation(progGlass_, "uSampleScale"), 1.0f, 1.0f);
    glUniform2f(glGetUniformLocation(progGlass_, "uSamplePivot"), 0.0f, 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uAmbientIntensity"), 0.38f);
    glUniform1f(glGetUniformLocation(progGlass_, "uBlurRadius"), blurRadius);
    glUniform1f(glGetUniformLocation(progGlass_, "uVibrancy"), params_.B(1) ? 1.5f : 1.0f);  // 提饱和
    // colorControls（亮度/对比度/饱和度）+ 效果不透明度（参数 77..80）
    glUniform1f(glGetUniformLocation(progGlass_, "uBrightness"), params_.N(pid::kColorBrightness));
    glUniform1f(glGetUniformLocation(progGlass_, "uContrast"), params_.N(pid::kColorContrast));
    glUniform1f(glGetUniformLocation(progGlass_, "uSaturation"), params_.N(pid::kColorSaturation));
    glUniform1f(glGetUniformLocation(progGlass_, "uEffectOpacity"),
                params_.N(pid::kEffectOpacity));
    glUniform1f(glGetUniformLocation(progGlass_, "uPlainHighlight"),
                params_.B(bid::kPlainHighlight) ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uHighlight"), 0.5f);   // Highlight.Default alpha
    // 棱边高光：环宽 = ceil(0.5dp*4) = 2px，外缘柔化 = blurRadius 0.25dp*4 = 1px
    const float hlWidthPx = std::max(1.0f, std::ceil(params_.Px(pid::kHighlightWidth)));
    glUniform1f(glGetUniformLocation(progGlass_, "uHighlightWidth"), hlWidthPx);
    glUniform1f(glGetUniformLocation(progGlass_, "uHighlightBlur"), hlWidthPx * 0.5f);
    glUniform1f(glGetUniformLocation(progGlass_, "uAmbient"), params_.B(5) ? 1.0f : 0.0f);
    // 外投影：模糊半径 / 向下偏移 / 有效不透明度（参数 70/71/72）
    glUniform1f(glGetUniformLocation(progGlass_, "uShadowBlur"), params_.Px(pid::kShadowBlur));
    glUniform1f(glGetUniformLocation(progGlass_, "uShadowOffset"), params_.Px(pid::kShadowOffset));
    glUniform1f(glGetUniformLocation(progGlass_, "uShadowAlpha"), params_.N(pid::kShadowAlpha));
    // 页签文字**不**进场景纹理：它由 pass 5 叠加（形状外常态色 / 形状内凸显色）。
    // 若在这里画进场景，指示器的透镜会先采到常态字形，再叠凸显色字形 —— 两套重影，
    // 而且拖动时常态字形会被透镜拖着一起动（参考实现里文字是不动的）。
    glUniform1f(glGetUniformLocation(progGlass_, "uShowLabels"), 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uOverlayMode"), 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uShadow"), 1.0f);
    // 内阴影是顶部按钮才有的（参数 18）；底板与浮动件传 0 = 关
    glUniform1f(glGetUniformLocation(progGlass_, "uInnerRadius"), 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uInnerAlpha"), 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ---- pass 4b: 跟手光斑（对齐参考实现的 InteractiveHighlight）----
    //   - 位置与上游的 position lambda 同式：(value + 0.5) × 页签格宽 + 底板视差，
    //     **不含**页签格内缩（上游就是这么取的），y = 底板高的一半；再钳在底板范围内
    //   - 加性混合（BlendMode.Plus），乘自己的按压进度弹簧（参数 31/32）
    //   - 写回 sceneFbo_：这样指示器的透镜会把光斑一起折射（上游也是这个图层顺序）
    if (animGlowPress > 0.001f) {
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
        glViewport(0, 0, w, h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);   // BlendMode.Plus
        glEnable(GL_SCISSOR_TEST);
        SetScissorRect(panelCx - panelW * 0.5f, panelCy - panelH * 0.5f, panelW, panelH, 0.0f, w, h);
        glUseProgram(progGlow_);
        const float panelLeft = marginH + panelOffset;
        float glowX = panelLeft + (animValue + 0.5f) * tabW;
        glowX = std::max(panelLeft, std::min(panelLeft + barW, glowX));
        // 光斑跟底板一起缩放（绕底板中心）：形状 / 圆角 / 中心都乘 panelScale。
        // 否则底板放大后光斑仍裁在未缩放矩形里，端头会露出没被加白的月牙。
        glowX = panelCx + (glowX - panelCx) * panelScale;
        glUniform2f(glGetUniformLocation(progGlow_, "uResolution"), static_cast<float>(w),
                    static_cast<float>(h));
        glUniform4f(glGetUniformLocation(progGlow_, "uGlassRect"), panelCx - panelW * 0.5f,
                    panelCy - panelH * 0.5f, panelW, panelH);
        glUniform4f(glGetUniformLocation(progGlow_, "uRadii"), panelRadiusPx, panelRadiusPx,
                    panelRadiusPx, panelRadiusPx);
        glUniform1f(glGetUniformLocation(progGlow_, "uProgress"), animGlowPress);
        glUniform1f(glGetUniformLocation(progGlow_, "uFlat"), params_.N(pid::kGlowFlat));
        glUniform1f(glGetUniformLocation(progGlow_, "uRadial"), params_.N(pid::kGlowRadial));
        glUniform1f(glGetUniformLocation(progGlow_, "uRadiusFactor"),
                    params_.N(pid::kGlowRadiusFactor));
        glUniform2f(glGetUniformLocation(progGlow_, "uCenter"), glowX, panelCy);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
    }

    // ---- pass 5: 指示器（采样 sceneTex_，同一套透镜）-> 屏幕 ----
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glUseProgram(progIndicator_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneTex_);
    glUniform1i(glGetUniformLocation(progIndicator_, "uScene"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, labelAccentTex_);
    glUniform1i(glGetUniformLocation(progIndicator_, "uLabelsAccent"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, labelTex_);
    glUniform1i(glGetUniformLocation(progIndicator_, "uLabels"), 2);
    glUniform2f(glGetUniformLocation(progIndicator_, "uResolution"), static_cast<float>(w),
                static_cast<float>(h));

    // 指示器几何由动画状态驱动（跟手 / 弹簧 / 按压缩放 / 速度形变）
    //   高度恒为 id 41；按压缩放走 scaleX/scaleY 的**独立弹簧**（参数 60..63），
    //   与参考实现的 layerBlock（固定 52dp 的胶囊 + scale）一致 —— 之前是把 press
    //   直接乘进高度，少了弹簧那一档。
    const float indH = params_.Px(pid::kIndicatorHeight);
    // 胶囊宽度 = 页签格 + 2 × (外扩 - 内缩)。
    // 对齐参考实现的 growthPx = (indicatorExpand - indicatorPadding)：id 42（胶囊内缩）
    // 以前在 GL 侧完全没读，滑杆是个死参数。
    const float growthPx = params_.Px(pid::kIndicatorExpand) - params_.Px(pid::kIndicatorPadding);
    const float indW = tabW + growthPx * 2.0f;
    float indX = marginH + panelOffset + innerPad + (animValue + 0.5f) * tabW - indW * 0.5f;
    // 端头钳位（对齐参考实现的 fastCoerceIn(minLeftPx, maxLeftPx)）：
    // 胶囊端头圆弧必须和底板端头圆弧同心 → 两侧至少留「同心留白」。
    // 默认参数下 indX 正好落在 minLeft 上（内缩 14 = 同心留白 4 + 外扩 10），所以是恒等变换；
    // 只有把内缩滑到 0、外扩拉满这类组合才会真正夹住 —— 那时不夹胶囊会探出底板。
    {
        const float panelLeft = marginH + panelOffset;
        const float minLeft = panelLeft + capInsetPx;
        const float maxLeft = std::max(minLeft, panelLeft + barW - indW - capInsetPx);
        indX = std::max(minLeft, std::min(maxLeft, indX));
    }
    const float indY = marginV + (barH - indH) * 0.5f;

    // 速度形变（拖动才非 1）：拉长/压扁，走非等比缩放。
    // 速度口径：animSpeed 已经是参考实现的 dragVelocity（value/秒 ÷ 2），这里 /10 同上游。
    const float v = animSpeed / 10.0f;
    const float stretch = std::max(-0.2f, std::min(0.2f, v * params_.N(pid::kVelocityStretch)));
    const float squash = std::max(-0.2f, std::min(0.2f, v * params_.N(pid::kVelocitySquash)));
    const float scaleX = 1.0f / (1.0f - stretch);
    const float scaleY = 1.0f * (1.0f - squash);
    // 按压 = **等边距膨胀**（不再是绕中心等比缩放）：
    // 竖向每边增长 = indH*(按压缩放弹簧-1)/2，横向同样多，所以四周超出量完全相等。
    // 旧写法 scaleX/scaleY 同时用按压缩放，因胶囊"宽 ≫ 高"，横向每边长的量会大一倍多。
    const float inflate = std::max(0.0f, indH * 0.5f * (animScaleY - 1.0f));

    glUniform4f(glGetUniformLocation(progIndicator_, "uRect"), indX, indY, indW, indH);
    // 胶囊 = 四角等半径
    const float indRadius = indH * 0.5f;
    glUniform4f(glGetUniformLocation(progIndicator_, "uRadii"), indRadius, indRadius, indRadius,
                indRadius);
    // 页签文字的映射：位图贴在底板矩形上（底板视差偏移也算进去）。
    // 形状外按屏幕坐标采常态色位图，形状内按折射坐标采凸显色位图。
    glUniform4f(glGetUniformLocation(progIndicator_, "uLabelRect"), marginH + panelOffset,
                marginV, barW, barH);
    glUniform2f(glGetUniformLocation(progIndicator_, "uLabelSize"), static_cast<float>(labelW_),
                static_cast<float>(labelH_));
    glUniform1f(glGetUniformLocation(progIndicator_, "uLabelSlots"), 3.0f);   // 首页 / 搜索 / 我的
    glUniform1f(glGetUniformLocation(progIndicator_, "uShowTabLabels"), 1.0f);
    // 按压时只有凸显色文字放大（参考实现的 LocalLiquidBottomTabScale = lerp(1, 1.2, press)）：
    // 放大比例跟着 press 弹簧走，松手自动回到 1
    glUniform1f(glGetUniformLocation(progIndicator_, "uLabelPressScale"),
                params_.N(pid::kPressLabelScale) * animPress);
    glUniform1f(glGetUniformLocation(progIndicator_, "uScaleX"), scaleX);
    glUniform1f(glGetUniformLocation(progIndicator_, "uScaleY"), scaleY);
    glUniform1f(glGetUniformLocation(progIndicator_, "uInflate"), inflate);
    // 按压采样缩小（参数 81）：形状向外膨胀的同时，玻璃里看到的底座/文字绕胶囊中心缩微 ——
    // 两者方向相反，合起来才是"液态透镜被按下去"的观感。下限 0.5 是防御：
    // 参数走 SetNumber 进来不设边界，极端值下别把采样缩到 0 / 负数。
    const float indSampleScale =
        std::max(0.5f, 1.0f - params_.N(pid::kPressSampleShrink) * animPress);
    glUniform2f(glGetUniformLocation(progIndicator_, "uSampleScale"), indSampleScale,
                indSampleScale);
    glUniform2f(glGetUniformLocation(progIndicator_, "uSamplePivot"), indX + indW * 0.5f,
                indY + indH * 0.5f);
    // 指示器透镜：静止/拖动两组值按 press 线性插值（与参考实现的 lerp(rest, drag, press) 一致）
    const auto lerpN = [this](int restId, int dragId, float t) {
        return params_.N(restId) + (params_.N(dragId) - params_.N(restId)) * t;
    };
    const float lensH = lerpN(pid::kRefractionHeightRest, pid::kRefractionHeightDrag, animPress);
    const float lensA = lerpN(pid::kRefractionAmountRest, pid::kRefractionAmountDrag, animPress);
    glUniform1f(glGetUniformLocation(progIndicator_, "uRefractionHeight"),
                mode == 2 ? 0.0f : lensH * params_.Density());
    // 同底板：上游对 amount 取负（-refractionAmount），位移朝内才是液态玻璃的边缘压缩
    glUniform1f(glGetUniformLocation(progIndicator_, "uRefractionAmount"),
                -lensA * params_.Density());
    glUniform1f(glGetUniformLocation(progIndicator_, "uDispersion"),
                (params_.B(2) && mode != 1) ? 1.0f : 0.0f);   // bool 2 = 指示器色散
    // depthEffect（bool 6）与底板共用同一个开关（上游页签指示器不用，默认关）
    glUniform1f(glGetUniformLocation(progIndicator_, "uDepthEffect"),
                params_.B(bid::kDepthEffect) ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(progIndicator_, "uTint"),
                mode == 3 ? 0.0f : lerpN(pid::kTintRest, pid::kTintDrag, animPress));
    // 表面白（参数 36 / 46）：同一层 onDrawSurface 上叠一层白，默认两端都是 0（等价关闭）
    glUniform1f(glGetUniformLocation(progIndicator_, "uSurfaceWhite"),
                mode == 3 ? 0.0f : lerpN(pid::kSurfaceWhiteRest, pid::kSurfaceWhiteDrag, animPress));
    glUniform1f(glGetUniformLocation(progIndicator_, "uHighlight"),
                lerpN(pid::kHighlightRest, pid::kHighlightDrag, animPress));
    glUniform1f(glGetUniformLocation(progIndicator_, "uHighlightWidth"), hlWidthPx);
    glUniform1f(glGetUniformLocation(progIndicator_, "uHighlightBlur"), hlWidthPx * 0.5f);
    // 指示器投影：几何与底板相同，强度 = 基础 alpha × lerp(静止 0.22, 拖动 1.0, press)
    glUniform1f(glGetUniformLocation(progIndicator_, "uShadowBlur"), params_.Px(pid::kShadowBlur));
    glUniform1f(glGetUniformLocation(progIndicator_, "uShadowOffset"),
                params_.Px(pid::kShadowOffset));
    // 指示器投影：上游 Shadow 的黑 alpha 上限只有 0.1，乘 kShadowAlpha 后按压最多 0.09，
    // 不足以在底座上形成"断开"的暗圈。这里额外 ×3（按压 ≈0.27）让鼓出部分与底座分离，
    // 做出 iOS 那种空隙感。衰减半径仍用底座的 uShadowBlur。
    glUniform1f(glGetUniformLocation(progIndicator_, "uShadowAlpha"),
                params_.N(pid::kShadowAlpha) * 3.0f *
                    lerpN(pid::kShadowRest, pid::kShadowDrag, animPress));
    // 内阴影：半径/强度两组值按 press 插值；强度再乘上游 InnerShadow 的颜色黑 alpha 0.15
    glUniform1f(glGetUniformLocation(progIndicator_, "uInnerRadius"),
                lerpN(pid::kInnerShadowRadiusRest, pid::kInnerShadowRadiusDrag, animPress) *
                    params_.Density());
    glUniform1f(glGetUniformLocation(progIndicator_, "uInnerAlpha"),
                lerpN(pid::kInnerShadowAlphaRest, pid::kInnerShadowAlphaDrag, animPress) * 0.15f);
    // 指示器模糊（参数 65/66，默认 0）
    glUniform1f(glGetUniformLocation(progIndicator_, "uSceneBlur"),
                lerpN(pid::kIndicatorBlurRest, pid::kIndicatorBlurDrag, animPress) *
                    params_.Density());
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ---- pass 6: 浮动玻璃件（与底板**同一个 program**，换材质输入 + 叠加混合）----
    //   - uBackdrop 绑已合成的 sceneTex_：折射采样的是"背景 + 底板 + 文字"
    //   - uBlurred  仍绑模糊背景（在底板之外的位置是对的）
    //   - uOverlayMode=1：形状外只输出自己的投影，形状内输出覆盖率 alpha
    glUseProgram(progGlass_);          // ★ pass 5 用的是指示器 program，这里必须切回来
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // 浮动件同样只占一小块：裁剪到"自己的矩形 + 外投影范围"（等价，省掉整屏的玻璃计算）
    glEnable(GL_SCISSOR_TEST);
    SetScissorRect(floatX_.load(), floatY_.load(), barW, barH,
                   params_.Px(pid::kShadowBlur) + params_.Px(pid::kShadowOffset) + 4.0f, w, h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneTex_);
    glUniform1i(glGetUniformLocation(progGlass_, "uBackdrop"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blurResult);
    glUniform1i(glGetUniformLocation(progGlass_, "uBlurred"), 1);
    glUniform1f(glGetUniformLocation(progGlass_, "uShowLabels"), 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uOverlayMode"), 1.0f);
    glUniform4f(glGetUniformLocation(progGlass_, "uGlassRect"), floatX_.load(), floatY_.load(),
                barW, barH);
    // 浮动件是四角独立圆角的展示面（参数 73..76）：每角钳到高度一半，避免圆角互相吃掉。
    const float floatRadiusMax = barH * 0.5f;
    glUniform4f(glGetUniformLocation(progGlass_, "uRadii"),
                std::min(params_.Px(pid::kFloatCornerTL), floatRadiusMax),
                std::min(params_.Px(pid::kFloatCornerTR), floatRadiusMax),
                std::min(params_.Px(pid::kFloatCornerBR), floatRadiusMax),
                std::min(params_.Px(pid::kFloatCornerBL), floatRadiusMax));
    // 材质与底板一致（同一个 program 且共享参数）：这里显式再写一遍，避免残留上一 pass 的状态
    glUniform1f(glGetUniformLocation(progGlass_, "uDepthEffect"),
                params_.B(bid::kDepthEffect) ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uBrightness"), params_.N(pid::kColorBrightness));
    glUniform1f(glGetUniformLocation(progGlass_, "uContrast"), params_.N(pid::kColorContrast));
    glUniform1f(glGetUniformLocation(progGlass_, "uSaturation"), params_.N(pid::kColorSaturation));
    glUniform1f(glGetUniformLocation(progGlass_, "uEffectOpacity"),
                params_.N(pid::kEffectOpacity));
    glUniform1f(glGetUniformLocation(progGlass_, "uPlainHighlight"),
                params_.B(bid::kPlainHighlight) ? 1.0f : 0.0f);
    // 浮动件同样把新增 uniform 复位（同一个 program，避免上一 pass 的残留）
    glUniform1f(glGetUniformLocation(progGlass_, "uScaleX"), 1.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uScaleY"), 1.0f);
    glUniform3f(glGetUniformLocation(progGlass_, "uSurfaceColor"), 1.0f, 1.0f, 1.0f);
    glUniform2f(glGetUniformLocation(progGlass_, "uSampleScale"), 1.0f, 1.0f);
    glUniform2f(glGetUniformLocation(progGlass_, "uSamplePivot"), 0.0f, 0.0f);
    glUniform1f(glGetUniformLocation(progGlass_, "uAmbientIntensity"), 0.38f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    // ---- pass 7: 顶部三个玻璃按钮（同一个 progGlass_，圆形 = 半径取半边长）----
    //   - uBackdrop 绑**清晰背景**、uBlurred 绑按钮自己的模糊链（pass 2b）
    //   - uOverlayMode=1：形状外只输出自己的投影、形状内输出覆盖率 alpha（叠加混合）
    //   - 材质读按钮自己的参数：模糊 10/11、折射 12/13、高光 14/15、阴影 16/17、内阴影 18、
    //     按压缩放 19、按压缩放弹簧 29/30；底色填充仍用 containerAlpha（id 4，上游同式）
    //   - 放在最后画：它们在屏幕顶部，与底板 / 浮动件无重叠，画在最上层最省事
    const float btnSize = ButtonSizePx();
    if (btnSize > 1.0f) {
        const float iconLensRest = params_.Px(pid::kIconRefractionRest);
        const float iconLensPress = params_.Px(pid::kIconRefractionPress);
        const float iconHlRest = params_.N(pid::kIconHighlightRest);
        const float iconHlPress = params_.N(pid::kIconHighlightPress);
        const float iconShRest = params_.N(pid::kIconShadowRest);
        const float iconShPress = params_.N(pid::kIconShadowPress);
        const float iconInnerPx = params_.Px(pid::kIconInnerShadowRadius);
        const float iconPressScale = params_.N(pid::kIconPressScale);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(progGlass_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bgTex_);
        glUniform1i(glGetUniformLocation(progGlass_, "uBackdrop"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, btnBlurResult);
        glUniform1i(glGetUniformLocation(progGlass_, "uBlurred"), 1);
        glUniform2f(glGetUniformLocation(progGlass_, "uResolution"), static_cast<float>(w),
                    static_cast<float>(h));
        glUniform2f(glGetUniformLocation(progGlass_, "uTexel"), texelX, texelY);
        glUniform1f(glGetUniformLocation(progGlass_, "uShowLabels"), 0.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uOverlayMode"), 1.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uVibrancy"), params_.B(1) ? 1.5f : 1.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uSurfaceAlpha"),
                    mode == 3 ? 0.0f : params_.N(pid::kContainerAlpha));
        glUniform1f(glGetUniformLocation(progGlass_, "uDispersion"),
                    (params_.B(0) && mode != 1) ? 1.0f : 0.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uBlurRadius"), btnBlurRadius);
        glUniform1f(glGetUniformLocation(progGlass_, "uHighlightWidth"), hlWidthPx);
        glUniform1f(glGetUniformLocation(progGlass_, "uHighlightBlur"), hlWidthPx * 0.5f);
        // 上游按钮只有 DefaultHighlight，不叠环境光 / 不用 depthEffect / 不做色彩控制
        glUniform1f(glGetUniformLocation(progGlass_, "uAmbient"), 0.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uDepthEffect"), 0.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uPlainHighlight"), 0.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uBrightness"), 0.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uContrast"), 1.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uSaturation"), 1.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uEffectOpacity"), 1.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uShadowBlur"), params_.Px(pid::kShadowBlur));
        glUniform1f(glGetUniformLocation(progGlass_, "uShadowOffset"),
                    params_.Px(pid::kShadowOffset));
        glUniform1f(glGetUniformLocation(progGlass_, "uShadow"), 1.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uScaleX"), 1.0f);
        glUniform1f(glGetUniformLocation(progGlass_, "uScaleY"), 1.0f);
        glUniform3f(glGetUniformLocation(progGlass_, "uSurfaceColor"), 1.0f, 1.0f, 1.0f);
        // uSampleScale / uSamplePivot 是**逐按钮**的（见下面循环），这里不再设恒等值
        glUniform1f(glGetUniformLocation(progGlass_, "uAmbientIntensity"), 0.38f);

        // 每个按钮只算自己那一块（外扩覆盖它的外投影）
        const float btnShadowPad = params_.Px(pid::kShadowBlur) + params_.Px(pid::kShadowOffset) + 4.0f;
        glEnable(GL_SCISSOR_TEST);
        for (int i = 0; i < kIconButtonCount; i++) {
            const float press = btnPress[i];
            float left = 0.0f;
            float bottom = 0.0f;
            ButtonRectPx(i, w, h, &left, &bottom);
            const float scale = 1.0f + (iconPressScale - 1.0f) * press;
            const float size = btnSize * scale;
            const float cx = left + btnSize * 0.5f;
            const float cy = bottom + btnSize * 0.5f;
            SetScissorRect(cx - size * 0.5f, cy - size * 0.5f, size, size, btnShadowPad, w, h);
            glUniform4f(glGetUniformLocation(progGlass_, "uGlassRect"), cx - size * 0.5f,
                        cy - size * 0.5f, size, size);
            const float btnRadius = size * 0.5f;
            glUniform4f(glGetUniformLocation(progGlass_, "uRadii"), btnRadius, btnRadius,
                        btnRadius, btnRadius);
            // 按压采样缩小：与旋钮同量级，绕按钮中心（形状的按压缩放也绕中心）
            const float btnSampleScale = 1.0f - kButtonPressSampleShrink * press;
            glUniform2f(glGetUniformLocation(progGlass_, "uSampleScale"), btnSampleScale,
                        btnSampleScale);
            glUniform2f(glGetUniformLocation(progGlass_, "uSamplePivot"), cx, cy);
            // 上游按钮的透镜高度与强度同值：refractionRest + refractionPress × press
            const float lensPx = iconLensRest + iconLensPress * press;
            glUniform1f(glGetUniformLocation(progGlass_, "uRefractionHeight"), lensPx);
            // 同底板：上游对 amount 取负（-refractionAmount）
            glUniform1f(glGetUniformLocation(progGlass_, "uRefractionAmount"), -lensPx);
            glUniform1f(glGetUniformLocation(progGlass_, "uHighlight"),
                        iconHlRest + iconHlPress * press);
            // 阴影 alpha = Shadow.Default 的黑 alpha 0.1 × (rest + press × press)
            glUniform1f(glGetUniformLocation(progGlass_, "uShadowAlpha"),
                        (iconShRest + iconShPress * press) * 0.1f);
            // 内阴影：半径 = id 18 × press，alpha = press × InnerShadow 的黑 alpha 0.15
            glUniform1f(glGetUniformLocation(progGlass_, "uInnerRadius"), iconInnerPx * press);
            glUniform1f(glGetUniformLocation(progGlass_, "uInnerAlpha"), press * 0.15f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
    }

    // ---- pass 8: 按钮图标（叠在按钮之上；图标不参与按压缩放，上游也是兄弟节点）----
    {
        const float iconPx = static_cast<float>(iconTexSizePx_);
        if (btnSize > 1.0f && iconPx > 0.0f && iconTex_ != 0) {
            float left = 0.0f;
            float bottom = 0.0f;
            ButtonRectPx(0, w, h, &left, &bottom);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // 图标位图是**预乘 alpha**
            glEnable(GL_SCISSOR_TEST);
            // 三个按钮的并集（图标只画在按钮里）
            SetScissorRect(left, bottom,
                           static_cast<float>(kIconButtonCount) * btnSize +
                               static_cast<float>(kIconButtonCount - 1) * ButtonSpacingPx(),
                           btnSize, 2.0f, w, h);
            glUseProgram(progIcon_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, iconTex_);
            glUniform1i(glGetUniformLocation(progIcon_, "uIcons"), 0);
            glUniform2f(glGetUniformLocation(progIcon_, "uResolution"), static_cast<float>(w),
                        static_cast<float>(h));
            glUniform2f(glGetUniformLocation(progIcon_, "uFirstRect"), left, bottom);
            glUniform1f(glGetUniformLocation(progIcon_, "uBtnSize"), btnSize);
            glUniform1f(glGetUniformLocation(progIcon_, "uSpacing"), ButtonSpacingPx());
            glUniform1f(glGetUniformLocation(progIcon_, "uIconSize"), iconPx);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
        }
    }

    glFlush();
}

void GlesRenderer::LogFps() {
    // 每次循环都调用（含空闲跳过的分支）：frameCount_ 只统计真正绘制的帧，
    // skippedFrames_ 统计被空闲检测跳过的次数。
    int64_t now = NowMs();
    if (fpsWindowStartMs_ == 0) {
        fpsWindowStartMs_ = now;
        return;
    }
    int64_t elapsed = now - fpsWindowStartMs_;
    if (elapsed >= 1000) {
        const float fps = frameCount_ * 1000.0f / static_cast<float>(elapsed);
        // idle = 这一秒里被空闲检测跳过的帧数：静止时应接近满值（功耗接近 0）
        GLOG("PERF_GL fps=%{public}.1f frames=%{public}d idle=%{public}d in %{public}lldms "
             "mode=%{public}d",
             fps, frameCount_, skippedFrames_, static_cast<long long>(elapsed),
             debugMode_.load());
        frameCount_ = 0;
        skippedFrames_ = 0;
        fpsWindowStartMs_ = now;
    }
}

void GlesRenderer::RenderLoop(void *window) {
    if (!InitEgl(window)) {
        GERR("EGL init failed, render loop abort");
        DestroyEgl();
        running_.store(false);
        return;
    }
    if (!InitGl()) {
        GERR("GL init failed, render loop abort");
        DestroyGl();
        DestroyEgl();
        running_.store(false);
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    int lastW = 0;
    int lastH = 0;
    int64_t lastSizeCheck = 0;

    while (!stopRequested_.load()) {
        int64_t now = NowMs();
        if (now - lastSizeCheck >= 100) {
            lastSizeCheck = now;
            lastW = static_cast<int>(width_.load());
            lastH = static_cast<int>(height_.load());
        }
        if (lastW <= 0 || lastH <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        if (!EnsureTargets(lastW, lastH)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        const float seconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();

        // 暂停（页面不可见）：不出帧、也不步进动画。仍要消费内容纹理的帧，
        // 否则 ArkUI 的 BuilderNode 生产端会被 buffer 队列堵住。
        if (paused_.load()) {
            UpdateContentTexture(targetW_, targetH_);
            skippedFrames_++;
            std::this_thread::sleep_for(std::chrono::milliseconds(kNormalFrameMs));
            LogFps();
            continue;
        }

        const int64_t animNow = NowMs();
        const float dt = (lastAnimMs_ == 0) ? 0.0f
                                            : static_cast<float>(animNow - lastAnimMs_) / 1000.0f;
        lastAnimMs_ = animNow;
        gSpringMotion = 0.0f;
        UpdateAnim(dt);
        UpdateCatalogAnim(dt);

        // 纹理重传（各自的事件驱动，静止时全是空检查）
        bool uploads = false;
        if (labelsDirty_.exchange(false)) {
            UploadLabels(targetW_, targetH_);
            uploads = true;
        }
        // 目录场景的文字层：场景/尺寸/数值变化时重传（见各自的 Dirty 标记）
        if (contentTextDirty_.exchange(false)) {
            UploadContentText(targetW_, targetH_);
            uploads = true;
        }
        if (overlayTextDirty_.exchange(false)) {
            UploadOverlayText(targetW_, targetH_);
            uploads = true;
        }
        // 顶部按钮的图标位图：尺寸（19dp × density）变了才需要重传
        const int wantIconPx =
            std::max(8, static_cast<int>(kIconButtonIconDp * params_.Density() + 0.5f));
        if (wantIconPx != iconTexSizePx_) {
            UploadIcons(wantIconPx);
            uploads = true;
        }
        if (photoDirty_.exchange(false)) {
            std::lock_guard<std::mutex> lock(photoMutex_);
            if (!photoPixels_.empty()) {
                glBindTexture(GL_TEXTURE_2D, photoTex_);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, photoW_, photoH_, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, photoPixels_.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                uploads = true;
                GLOG("photo texture uploaded %{public}dx%{public}d", photoW_, photoH_);
            }
        }

        // ArkTS 内容纹理：每帧取最新帧（没有新帧就沿用上一张），必须在画之前。
        // 有内容更新（BuilderNode 在动）也算"要画"。
        const bool contentFrame = UpdateContentTexture(targetW_, targetH_);

        // ---- 空闲检测：没有任何"会改变画面"的输入就不画 ----
        // 判定依据（任一成立就要画）：
        //   ① 输入/参数/场景/尺寸变化（forceRedraw_）
        //   ② 纹理重传（uploads）
        //   ③ 弹簧还在动（gSpringMotion 超过阈值）
        //   ④ 交互窗口内（触摸后 400ms，保证跟手流畅）
        //   ⑤ ArkTS 内容纹理有新帧
        // 不画时 surface 保留最后一帧，屏幕内容不变；渲染线程只睡 16ms 醒来再查。
        const bool interacting = (animNow - lastTouchMs_.load()) < kInteractiveWindowMs;
        const bool active = forceRedraw_.exchange(false) || uploads ||
                            gSpringMotion > kIdleMotionEps || interacting || contentFrame;
        if (!active) {
            skippedFrames_++;
            std::this_thread::sleep_for(std::chrono::milliseconds(kNormalFrameMs));
            LogFps();
            continue;
        }

        PaceFrame();
        DrawFrame(seconds);
        eglSwapBuffers(display_, surface_);
        frameCount_++;
        LogFps();
    }

    DestroyGl();
    DestroyEgl();
    running_.store(false);
    GLOG("render loop exited");
}

}  // namespace glass
