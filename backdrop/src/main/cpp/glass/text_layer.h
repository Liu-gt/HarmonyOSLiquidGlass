#ifndef GLASS_TEXT_LAYER_H
#define GLASS_TEXT_LAYER_H

#include <cstdint>
#include <string>
#include <vector>

namespace glass {

/**
 * 页签条位图（RGBA8888，预乘 alpha，行 0 = 位图顶部）。
 *
 * 图标与文字栅格化为纹理输入至 GL 场景，以便材质着色器（如指示器透镜）
 * 对文字内容执行物理折射、色散与形变采样。
 */
struct LabelBitmap {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
};

/**
 * 绘制顶部三个玻璃按钮的图标（白色，预乘 alpha），横向排布生成位图。
 *
 * 图标图形对齐参考实现的 DragIcon / NotificationIcon / MoreIcon。
 * 顺序依次为：0 = 拖动图标（左），1 = 铃铛（中），2 = 更多选项（右）。
 *
 * @param iconSizePx 单个图标的边长（px）；输出位图尺寸 = (3 × iconSizePx) × iconSizePx。
 */
bool RenderGlassIcons(int iconSizePx, LabelBitmap &out);

/**
 * 画一条"图标 + 文字"的页签条。
 *
 * - [iconPaths] 是 24×24 视口的 SVG path（只含绝对 M/L/Q/Z），与参考实现的
 *   `TabIconPaths` 相同；用 [iconBoldStroke] 加粗描边。
 * - 每个标签居中在自己的格子里，图标在上、文字在下。
 * - [selectedArgb] / [unselectedArgb] 已经包含 alpha 通道。
 */
bool RenderTabStrip(int width, int height, const std::vector<std::string> &labels,
                    const std::vector<std::string> &iconPaths, int selectedIndex,
                    float iconSizePx, float iconGapPx, float iconBoldStroke, float fontSizePx,
                    float innerPaddingPx, uint32_t selectedArgb, uint32_t unselectedArgb,
                    LabelBitmap &out);

// ---------------------------------------------------------------------------
// 目录（catalog）场景用的通用文字 / 图标栅格化
// ---------------------------------------------------------------------------

/** 一段文字。坐标以位图**左上角**为原点（与 native_drawing 的行 0 一致）。 */
struct TextRun {
    std::string text;
    float x = 0.0f;           // 左对齐时是左边缘；居中对齐时是中心；右对齐时是右边缘
    float y = 0.0f;           // 文本块顶部
    float wrapWidth = 0.0f;   // > 0 时按词换行，块宽不超过它
    float fontSizePx = 16.0f;
    uint32_t argb = 0xFFFFFFFF;
    /** 0 = 左对齐，1 = 居中，2 = 右对齐（相对 wrapWidth；无 wrap 时居中/右对齐以 x 为基准） */
    int align = 0;
    bool bold = false;
    int maxLines = 0;         // 0 = 不限
};

/** 量一段文字在当前字号下的宽度（px）。 */
float MeasureTextWidth(const std::string &text, float fontSizePx, bool bold);

/**
 * 将多段文本批量栅格化至指定尺寸的位图（透明背景，预乘 alpha）。
 *
 * 采用批量预烘焙纹理方案，通过单次贴图绘制提升静态文本布局的渲染性能，
 * 避免逐帧排版带来的 CPU 开销。
 */
bool RenderTextOverlay(int width, int height, const std::vector<TextRun> &runs, LabelBitmap &out);

/**
 * 把一条 24×24 视口的 SVG path（只含绝对 M/L/Q/Z）栅格化成正方形图标位图。
 *
 * 用途：控制中心的飞机图标等（参考实现里是 ImageVector，这里等价成同视口的 path）。
 */
bool RenderIconPath(const std::string &svgPath, float viewport, int sizePx, uint32_t argb,
                    LabelBitmap &out);

}  // namespace glass

#endif  // GLASS_TEXT_LAYER_H
