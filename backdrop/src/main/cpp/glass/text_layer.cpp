#include "text_layer.h"

#include <hilog/log.h>
#include <native_drawing/drawing_bitmap.h>
#include <native_drawing/drawing_brush.h>
#include <native_drawing/drawing_canvas.h>
#include <native_drawing/drawing_font.h>
#include <native_drawing/drawing_path.h>
#include <native_drawing/drawing_pen.h>
#include <native_drawing/drawing_point.h>
#include <native_drawing/drawing_text_blob.h>
#include <native_drawing/drawing_typeface.h>
#include <native_drawing/drawing_types.h>

#include <cctype>
#include <cstdlib>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "GLASS"
#define LOG_DOMAIN 0x0000

#define GLOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)
#define GERR(...) OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)

namespace glass {

namespace {

/** 图标 SVG 视口边长（与参考实现一致：24×24） */
constexpr float kIconViewport = 24.0f;

/** 读一个浮点数；跳过逗号/空白。支持负号与小数。 */
bool ReadFloat(const std::string &s, size_t &i, float &out) {
    while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) {
        i++;
    }
    if (i >= s.size()) {
        return false;
    }
    size_t start = i;
    if (s[i] == '-' || s[i] == '+') {
        i++;
    }
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) {
        i++;
    }
    if (i == start) {
        return false;
    }
    out = std::strtof(s.substr(start, i - start).c_str(), nullptr);
    return true;
}

/** 24×24 视口的 SVG path（绝对 M/L/Q/Z）-> OH_Drawing_Path。 */
OH_Drawing_Path *ParseSvgPath(const std::string &d) {
    OH_Drawing_Path *path = OH_Drawing_PathCreate();
    size_t i = 0;
    char lastCmd = 0;
    while (i < d.size()) {
        while (i < d.size() && std::isspace(static_cast<unsigned char>(d[i]))) {
            i++;
        }
        if (i >= d.size()) {
            break;
        }
        char c = d[i];
        if (std::isalpha(static_cast<unsigned char>(c))) {
            i++;
            lastCmd = c;
        } else if (lastCmd == 0) {
            break;  // 非法：还没有命令就有数字
        } else {
            c = lastCmd;  // 隐式重复上一个命令
        }

        if (c == 'Z' || c == 'z') {
            OH_Drawing_PathClose(path);
            continue;
        }
        float v[4] = {0, 0, 0, 0};
        const int need = (c == 'Q' || c == 'q') ? 4 : 2;
        bool ok = true;
        for (int k = 0; k < need; k++) {
            if (!ReadFloat(d, i, v[k])) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            break;
        }
        switch (c) {
            case 'M':
                OH_Drawing_PathMoveTo(path, v[0], v[1]);
                lastCmd = 'L';  // SVG 规则：M 之后的隐式命令是 L
                break;
            case 'L':
                OH_Drawing_PathLineTo(path, v[0], v[1]);
                break;
            case 'Q':
                OH_Drawing_PathQuadTo(path, v[0], v[1], v[2], v[3]);
                break;
            default:
                break;
        }
    }
    return path;
}

/**
 * 顶部按钮的三个图标：形状与参考实现的 DragIcon / NotificationIcon /
 * MoreIcon 逐点一致（那里用 Canvas 手绘，归一化到 0..1 的边长内）。
 *
 * 全部用白色不透明画；预乘 alpha 的位图里"白色 × 覆盖"就是我们要的图标像素。
 */
void DrawGlassIcons(OH_Drawing_Canvas *canvas, OH_Drawing_Brush *brush, OH_Drawing_Pen *pen,
                    float size, int index) {
    const float w = size;
    const float h = size;

    // 图标之间的横向偏移：每个图标占一格
    OH_Drawing_CanvasSave(canvas);
    OH_Drawing_CanvasTranslate(canvas, w * static_cast<float>(index), 0.0f);

    if (index == 0) {
        // ---- DragIcon：水平双向箭头（描边） ----
        OH_Drawing_Path *path = OH_Drawing_PathCreate();
        OH_Drawing_PathMoveTo(path, w * 0.12f, h * 0.50f);
        OH_Drawing_PathLineTo(path, w * 0.88f, h * 0.50f);
        OH_Drawing_PathMoveTo(path, w * 0.31f, h * 0.26f);
        OH_Drawing_PathLineTo(path, w * 0.12f, h * 0.50f);
        OH_Drawing_PathLineTo(path, w * 0.31f, h * 0.74f);
        OH_Drawing_PathMoveTo(path, w * 0.69f, h * 0.26f);
        OH_Drawing_PathLineTo(path, w * 0.88f, h * 0.50f);
        OH_Drawing_PathLineTo(path, w * 0.69f, h * 0.74f);

        OH_Drawing_PenSetColor(pen, 0xFFFFFFFF);
        OH_Drawing_PenSetWidth(pen, w * 0.11f);
        OH_Drawing_PenSetCap(pen, LINE_ROUND_CAP);
        OH_Drawing_PenSetJoin(pen, LINE_ROUND_JOIN);
        OH_Drawing_CanvasAttachPen(canvas, pen);
        OH_Drawing_CanvasDrawPath(canvas, path);
        OH_Drawing_CanvasDetachPen(canvas);
        OH_Drawing_PathDestroy(path);
    } else if (index == 1) {
        // ---- NotificationIcon：铃铛（填充）+ 底部小圆点 ----
        OH_Drawing_Path *path = OH_Drawing_PathCreate();
        OH_Drawing_PathMoveTo(path, w * 0.50f, h * 0.06f);
        OH_Drawing_PathCubicTo(path, w * 0.22f, h * 0.08f, w * 0.20f, h * 0.42f, w * 0.19f,
                               h * 0.64f);
        OH_Drawing_PathLineTo(path, w * 0.08f, h * 0.76f);
        OH_Drawing_PathCubicTo(path, w * 0.32f, h * 0.86f, w * 0.68f, h * 0.86f, w * 0.92f,
                               h * 0.76f);
        OH_Drawing_PathLineTo(path, w * 0.81f, h * 0.64f);
        OH_Drawing_PathCubicTo(path, w * 0.80f, h * 0.42f, w * 0.78f, h * 0.08f, w * 0.50f,
                               h * 0.06f);
        OH_Drawing_PathClose(path);

        OH_Drawing_BrushSetColor(brush, 0xFFFFFFFF);
        OH_Drawing_CanvasAttachBrush(canvas, brush);
        OH_Drawing_CanvasDrawPath(canvas, path);
        OH_Drawing_CanvasDetachBrush(canvas);
        OH_Drawing_PathDestroy(path);

        OH_Drawing_Point *center = OH_Drawing_PointCreate(w * 0.50f, h * 0.94f);
        OH_Drawing_CanvasAttachBrush(canvas, brush);
        OH_Drawing_CanvasDrawCircle(canvas, center, h * 0.075f);
        OH_Drawing_CanvasDetachBrush(canvas);
        OH_Drawing_PointDestroy(center);
    } else {
        // ---- MoreIcon：三个竖排圆点 ----
        const float r = w * 0.10f;
        const float ys[3] = {0.20f, 0.50f, 0.80f};
        OH_Drawing_BrushSetColor(brush, 0xFFFFFFFF);
        for (int i = 0; i < 3; i++) {
            OH_Drawing_Point *center = OH_Drawing_PointCreate(w * 0.50f, h * ys[i]);
            OH_Drawing_CanvasAttachBrush(canvas, brush);
            OH_Drawing_CanvasDrawCircle(canvas, center, r);
            OH_Drawing_CanvasDetachBrush(canvas);
            OH_Drawing_PointDestroy(center);
        }
    }

    OH_Drawing_CanvasRestore(canvas);
}

OH_Drawing_Font *CreateFont(float fontSizePx, bool bold) {
    OH_Drawing_Font *font = OH_Drawing_FontCreate();
    OH_Drawing_Typeface *typeface = OH_Drawing_TypefaceCreateDefault();
    OH_Drawing_FontSetTypeface(font, typeface);
    OH_Drawing_TypefaceDestroy(typeface);
    OH_Drawing_FontSetTextSize(font, fontSizePx);
    if (bold) {
        OH_Drawing_FontSetFakeBoldText(font, true);
    }
    return font;
}

float MeasureWithFont(OH_Drawing_Font *font, const std::string &text) {
    if (text.empty()) {
        return 0.0f;
    }
    float width = 0.0f;
    OH_Drawing_FontMeasureText(font, text.c_str(), text.size(), TEXT_ENCODING_UTF8, nullptr,
                               &width);
    return width;
}

/** 基线以上的高度（ascent）；native_drawing 返回正数，异常时按 0.88 × 字号兜底。 */
float AscentOf(OH_Drawing_Font *font, float fontSizePx) {
    OH_Drawing_Font_Metrics metrics{};
    float ascent = OH_Drawing_FontGetMetrics(font, &metrics);
    if (ascent == 0.0f) {
        ascent = fontSizePx * 0.88f;
    }
    if (ascent < 0.0f) {
        ascent = -ascent;
    }
    return ascent;
}

/**
 * 按空格拆词换行；单个词超过行宽时再按字符硬断。
 * 目录场景的文案都是英文/数字，词级换行已足够（中文按字符断由硬断兜底）。
 */
std::vector<std::string> WrapText(OH_Drawing_Font *font, const std::string &text, float maxWidth) {
    std::vector<std::string> lines;
    if (text.empty()) {
        lines.push_back("");
        return lines;
    }
    if (maxWidth <= 1.0f) {
        lines.push_back(text);
        return lines;
    }
    size_t i = 0;
    while (i < text.size()) {
        size_t lineStart = i;
        size_t lastSpace = std::string::npos;
        float width = 0.0f;
        // 逐词前进：词 + 词间空格
        while (i < text.size()) {
            size_t wordStart = i;
            size_t wordEnd = text.find(' ', wordStart);
            if (wordEnd == std::string::npos) {
                wordEnd = text.size();
            }
            std::string candidate = text.substr(lineStart, wordEnd - lineStart);
            if (MeasureWithFont(font, candidate) > maxWidth && wordEnd > wordStart) {
                if (lineStart == wordStart) {
                    // 首词就超宽：按字符硬断
                    size_t cut = wordStart;
                    while (cut < wordEnd &&
                           MeasureWithFont(font, text.substr(lineStart, cut - lineStart + 1)) <=
                               maxWidth) {
                        cut++;
                    }
                    if (cut == lineStart) {
                        cut = lineStart + 1;
                    }
                    lines.push_back(text.substr(lineStart, cut - lineStart));
                    i = cut;
                    lastSpace = std::string::npos;
                    break;
                }
                // 回退到上一处可断行（去掉行尾空格）
                size_t end = lastSpace != std::string::npos ? lastSpace : wordStart;
                while (end > lineStart && text[end - 1] == ' ') {
                    end--;
                }
                lines.push_back(text.substr(lineStart, end - lineStart));
                i = end;
                while (i < text.size() && text[i] == ' ') {
                    i++;
                }
                lastSpace = std::string::npos;
                break;
            }
            width = MeasureWithFont(font, candidate);
            (void)width;
            if (wordEnd >= text.size()) {
                lines.push_back(text.substr(lineStart));
                i = text.size();
                break;
            }
            lastSpace = wordEnd;
            i = wordEnd + 1;
        }
    }
    if (lines.empty()) {
        lines.push_back(text);
    }
    return lines;
}

}  // namespace

float MeasureTextWidth(const std::string &text, float fontSizePx, bool bold) {
    OH_Drawing_Font *font = CreateFont(fontSizePx, bold);
    const float width = MeasureWithFont(font, text);
    OH_Drawing_FontDestroy(font);
    return width;
}

bool RenderTextOverlay(int width, int height, const std::vector<TextRun> &runs, LabelBitmap &out) {
    if (width <= 0 || height <= 0 || runs.empty()) {
        return false;
    }

    OH_Drawing_Bitmap *bitmap = OH_Drawing_BitmapCreate();
    OH_Drawing_BitmapFormat format{COLOR_FORMAT_RGBA_8888, ALPHA_FORMAT_PREMUL};
    OH_Drawing_BitmapBuild(bitmap, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                           &format);

    OH_Drawing_Canvas *canvas = OH_Drawing_CanvasCreate();
    OH_Drawing_CanvasBind(canvas, bitmap);
    OH_Drawing_CanvasClear(canvas, 0x00000000);

    OH_Drawing_Brush *brush = OH_Drawing_BrushCreate();
    OH_Drawing_BrushSetAntiAlias(brush, true);

    const float lineHeightFactor = 1.35f;
    for (const TextRun &run : runs) {
        if (run.text.empty()) {
            continue;
        }
        OH_Drawing_Font *font = CreateFont(run.fontSizePx, run.bold);
        const float ascent = AscentOf(font, run.fontSizePx);
        const float lineHeight = run.fontSizePx * lineHeightFactor;
        const std::vector<std::string> lines =
            (run.wrapWidth > 1.0f) ? WrapText(font, run.text, run.wrapWidth)
                                   : std::vector<std::string>{run.text};

        float baseline = run.y + ascent;
        int drawn = 0;
        for (const std::string &line : lines) {
            if (run.maxLines > 0 && drawn >= run.maxLines) {
                break;
            }
            const float textWidth = MeasureWithFont(font, line);
            float x = run.x;
            if (run.align == 1) {
                x = (run.wrapWidth > 1.0f) ? run.x + (run.wrapWidth - textWidth) * 0.5f
                                           : run.x - textWidth * 0.5f;
            } else if (run.align == 2) {
                x = (run.wrapWidth > 1.0f) ? run.x + run.wrapWidth - textWidth
                                           : run.x - textWidth;
            }
            OH_Drawing_TextBlob *blob = OH_Drawing_TextBlobCreateFromString(
                line.c_str(), font, TEXT_ENCODING_UTF8);
            if (blob != nullptr) {
                OH_Drawing_BrushSetColor(brush, run.argb);
                OH_Drawing_CanvasAttachBrush(canvas, brush);
                OH_Drawing_CanvasDrawTextBlob(canvas, blob, x, baseline);
                OH_Drawing_CanvasDetachBrush(canvas);
                OH_Drawing_TextBlobDestroy(blob);
            }
            baseline += lineHeight;
            drawn++;
        }
        OH_Drawing_FontDestroy(font);
    }

    auto *pixels = static_cast<uint32_t *>(OH_Drawing_BitmapGetPixels(bitmap));
    if (pixels == nullptr) {
        GERR("RenderTextOverlay: BitmapGetPixels returned null");
    } else {
        out.width = width;
        out.height = height;
        out.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height);
    }

    OH_Drawing_BrushDestroy(brush);
    OH_Drawing_CanvasDestroy(canvas);
    OH_Drawing_BitmapDestroy(bitmap);
    return !out.pixels.empty();
}

bool RenderIconPath(const std::string &svgPath, float viewport, int sizePx, uint32_t argb,
                    LabelBitmap &out) {
    if (svgPath.empty() || viewport <= 0.0f || sizePx <= 0) {
        return false;
    }

    OH_Drawing_Bitmap *bitmap = OH_Drawing_BitmapCreate();
    OH_Drawing_BitmapFormat format{COLOR_FORMAT_RGBA_8888, ALPHA_FORMAT_PREMUL};
    OH_Drawing_BitmapBuild(bitmap, static_cast<uint32_t>(sizePx), static_cast<uint32_t>(sizePx),
                           &format);

    OH_Drawing_Canvas *canvas = OH_Drawing_CanvasCreate();
    OH_Drawing_CanvasBind(canvas, bitmap);
    OH_Drawing_CanvasClear(canvas, 0x00000000);

    const float scale = static_cast<float>(sizePx) / viewport;
    OH_Drawing_CanvasScale(canvas, scale, scale);

    OH_Drawing_Path *path = ParseSvgPath(svgPath);
    OH_Drawing_Brush *brush = OH_Drawing_BrushCreate();
    OH_Drawing_BrushSetAntiAlias(brush, true);
    OH_Drawing_BrushSetColor(brush, argb);
    OH_Drawing_CanvasAttachBrush(canvas, brush);
    OH_Drawing_CanvasDrawPath(canvas, path);
    OH_Drawing_CanvasDetachBrush(canvas);

    auto *pixels = static_cast<uint32_t *>(OH_Drawing_BitmapGetPixels(bitmap));
    if (pixels == nullptr) {
        GERR("RenderIconPath: BitmapGetPixels returned null");
    } else {
        out.width = sizePx;
        out.height = sizePx;
        out.pixels.assign(pixels, pixels + static_cast<size_t>(sizePx) * sizePx);
    }

    OH_Drawing_PathDestroy(path);
    OH_Drawing_BrushDestroy(brush);
    OH_Drawing_CanvasDestroy(canvas);
    OH_Drawing_BitmapDestroy(bitmap);
    return !out.pixels.empty();
}

bool RenderGlassIcons(int iconSizePx, LabelBitmap &out) {
    if (iconSizePx <= 0) {
        return false;
    }
    const int width = iconSizePx * 3;
    const int height = iconSizePx;

    OH_Drawing_Bitmap *bitmap = OH_Drawing_BitmapCreate();
    OH_Drawing_BitmapFormat format{COLOR_FORMAT_RGBA_8888, ALPHA_FORMAT_PREMUL};
    OH_Drawing_BitmapBuild(bitmap, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                           &format);

    OH_Drawing_Canvas *canvas = OH_Drawing_CanvasCreate();
    OH_Drawing_CanvasBind(canvas, bitmap);
    OH_Drawing_CanvasClear(canvas, 0x00000000);

    OH_Drawing_Brush *brush = OH_Drawing_BrushCreate();
    OH_Drawing_BrushSetAntiAlias(brush, true);
    OH_Drawing_Pen *pen = OH_Drawing_PenCreate();
    OH_Drawing_PenSetAntiAlias(pen, true);

    for (int i = 0; i < 3; i++) {
        DrawGlassIcons(canvas, brush, pen, static_cast<float>(iconSizePx), i);
    }

    auto *pixels = static_cast<uint32_t *>(OH_Drawing_BitmapGetPixels(bitmap));
    if (pixels == nullptr) {
        GERR("RenderGlassIcons: BitmapGetPixels returned null");
    } else {
        out.width = width;
        out.height = height;
        out.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height);
    }

    OH_Drawing_BrushDestroy(brush);
    OH_Drawing_PenDestroy(pen);
    OH_Drawing_CanvasDestroy(canvas);
    OH_Drawing_BitmapDestroy(bitmap);

    return !out.pixels.empty();
}

bool RenderTabStrip(int width, int height, const std::vector<std::string> &labels,
                    const std::vector<std::string> &iconPaths, int selectedIndex,
                    float iconSizePx, float iconGapPx, float iconBoldStroke, float fontSizePx,
                    float innerPaddingPx, uint32_t selectedArgb, uint32_t unselectedArgb,
                    LabelBitmap &out) {
    if (width <= 0 || height <= 0 || labels.empty()) {
        return false;
    }

    OH_Drawing_Bitmap *bitmap = OH_Drawing_BitmapCreate();
    OH_Drawing_BitmapFormat format{COLOR_FORMAT_RGBA_8888, ALPHA_FORMAT_PREMUL};
    OH_Drawing_BitmapBuild(bitmap, static_cast<uint32_t>(width),
                           static_cast<uint32_t>(height), &format);

    OH_Drawing_Canvas *canvas = OH_Drawing_CanvasCreate();
    OH_Drawing_CanvasBind(canvas, bitmap);
    OH_Drawing_CanvasClear(canvas, 0x00000000);

    OH_Drawing_Brush *brush = OH_Drawing_BrushCreate();
    OH_Drawing_BrushSetAntiAlias(brush, true);
    OH_Drawing_Pen *pen = OH_Drawing_PenCreate();
    OH_Drawing_PenSetAntiAlias(pen, true);

    OH_Drawing_Font *font = OH_Drawing_FontCreate();
    OH_Drawing_Typeface *typeface = OH_Drawing_TypefaceCreateDefault();
    OH_Drawing_FontSetTypeface(font, typeface);
    OH_Drawing_FontSetTextSize(font, fontSizePx);

    // 实测：OH_Drawing_FontGetMetrics 返回的 ascent 是**正数**（基线以上的高度）
    OH_Drawing_Font_Metrics metrics{};
    float ascent = OH_Drawing_FontGetMetrics(font, &metrics);
    if (ascent == 0.0f) {
        ascent = fontSizePx * 0.88f;
    }
    if (ascent < 0.0f) {
        ascent = -ascent;
    }

    const float inner = innerPaddingPx;
    const float usable = static_cast<float>(width) - inner * 2.0f;
    const float slot = usable / static_cast<float>(labels.size());

    // 整列（图标 + 间距 + 文字）垂直居中
    const float textBlock = fontSizePx * 1.35f;
    const float columnH = iconSizePx + iconGapPx + textBlock;
    const float columnTop = (static_cast<float>(height) - columnH) * 0.5f;
    const float iconTop = columnTop;
    const float baseline = columnTop + iconSizePx + iconGapPx + ascent;

    const float iconScale = iconSizePx / kIconViewport;

    for (size_t i = 0; i < labels.size(); i++) {
        const bool selected = (static_cast<int>(i) == selectedIndex);
        const uint32_t argb = selected ? selectedArgb : unselectedArgb;
        const float centerX = inner + slot * (static_cast<float>(i) + 0.5f);

        // ---- 图标 ----
        if (i < iconPaths.size() && !iconPaths[i].empty()) {
            OH_Drawing_Path *path = ParseSvgPath(iconPaths[i]);
            OH_Drawing_CanvasSave(canvas);
            OH_Drawing_CanvasTranslate(canvas, centerX - iconSizePx * 0.5f, iconTop);
            OH_Drawing_CanvasScale(canvas, iconScale, iconScale);

            OH_Drawing_BrushSetColor(brush, argb);
            OH_Drawing_CanvasAttachBrush(canvas, brush);
            OH_Drawing_CanvasDrawPath(canvas, path);
            OH_Drawing_CanvasDetachBrush(canvas);

            // 加粗：填充之上再描一圈圆角描边（对齐参考实现的 TabIconBoldStroke）
            if (iconBoldStroke > 0.0f) {
                OH_Drawing_PenSetColor(pen, argb);
                OH_Drawing_PenSetWidth(pen, iconBoldStroke);
                OH_Drawing_PenSetAntiAlias(pen, true);
                OH_Drawing_CanvasAttachPen(canvas, pen);
                OH_Drawing_CanvasDrawPath(canvas, path);
                OH_Drawing_CanvasDetachPen(canvas);
            }

            OH_Drawing_CanvasRestore(canvas);
            OH_Drawing_PathDestroy(path);
        }

        // ---- 文字 ----
        OH_Drawing_TextBlob *blob =
            OH_Drawing_TextBlobCreateFromString(labels[i].c_str(), font, TEXT_ENCODING_UTF8);
        if (blob != nullptr) {
            float textWidth = 0.0f;
            OH_Drawing_FontMeasureText(font, labels[i].c_str(), labels[i].size(),
                                       TEXT_ENCODING_UTF8, nullptr, &textWidth);
            OH_Drawing_BrushSetColor(brush, argb);
            OH_Drawing_CanvasAttachBrush(canvas, brush);
            OH_Drawing_CanvasDrawTextBlob(canvas, blob, centerX - textWidth * 0.5f, baseline);
            OH_Drawing_CanvasDetachBrush(canvas);
            OH_Drawing_TextBlobDestroy(blob);
        }
    }

    auto *pixels = static_cast<uint32_t *>(OH_Drawing_BitmapGetPixels(bitmap));
    if (pixels == nullptr) {
        GERR("BitmapGetPixels returned null");
    } else {
        out.width = width;
        out.height = height;
        out.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height);
    }

    OH_Drawing_FontDestroy(font);
    OH_Drawing_TypefaceDestroy(typeface);
    OH_Drawing_BrushDestroy(brush);
    OH_Drawing_PenDestroy(pen);
    OH_Drawing_CanvasDestroy(canvas);
    OH_Drawing_BitmapDestroy(bitmap);

    return !out.pixels.empty();
}

}  // namespace glass
