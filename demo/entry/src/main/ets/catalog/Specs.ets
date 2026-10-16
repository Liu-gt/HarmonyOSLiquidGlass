// ---------------------------------------------------------------------------
// 玻璃页签条（Bottom tabs）的参数表。**编号必须与
// `backdrop/src/main/cpp/glass/glass_params.cpp` 的数组下标一致**；改编号时两处同改。
//
// 其余目录场景（Buttons / Toggle / Slider / Dialog ...）的参数是直接对齐
// 参考实现（Kyant 的 Compose 版本）的固定值，不暴露可调菜单。
// ---------------------------------------------------------------------------

export interface NumberSpec {
  id: number;
  group: string;
  label: string;
  min: number;
  max: number;
  step: number;
  def: number;
  decimals: number;
  unit: string;
}

export interface ToggleSpec {
  id: number;
  label: string;
  def: boolean;
  group?: string;
}

/**
 * 参数表。**`id` 必须与 `glass_params.cpp` 的 values_ 下标完全一致。**
 * 改编号时两处同改。
 */
export const NUMBER_SPECS: NumberSpec[] = [
  { id: 0, group: '底座·材质', label: '底座高度', min: 44, max: 80, step: 1, def: 56, decimals: 0, unit: 'dp' },
  { id: 1, group: '底座·材质', label: '模糊半径', min: 0, max: 30, step: 0.1, def: 0.3, decimals: 1, unit: 'dp' },
  { id: 2, group: '底座·材质', label: '折射高度', min: 0, max: 48, step: 0.1, def: 21, decimals: 1, unit: 'dp' },
  { id: 3, group: '底座·材质', label: '折射强度', min: 0, max: 48, step: 0.1, def: 18, decimals: 1, unit: 'dp' },
  { id: 4, group: '底座·材质', label: '底色不透明度', min: 0, max: 0.6, step: 0.01, def: 0.45, decimals: 2, unit: '' },
  { id: 5, group: '底座·材质', label: '按压缩放', min: 0, max: 20, step: 1, def: 8, decimals: 0, unit: 'dp' },
  { id: 6, group: '底座·材质', label: '光斑·平面', min: 0, max: 0.3, step: 0.001, def: 0.003, decimals: 3, unit: '' },
  { id: 7, group: '底座·材质', label: '光斑·径向', min: 0, max: 0.4, step: 0.001, def: 0.008, decimals: 3, unit: '' },
  { id: 8, group: '底座·材质', label: '光斑半径系数', min: 0.5, max: 3, step: 0.1, def: 1.0, decimals: 1, unit: 'x' },
  { id: 9, group: '底座·按钮', label: '按钮尺寸', min: 28, max: 72, step: 1, def: 46, decimals: 0, unit: 'dp' },
  { id: 10, group: '底座·按钮', label: '基础模糊', min: 0, max: 12, step: 0.1, def: 4, decimals: 1, unit: 'dp' },
  { id: 11, group: '底座·按钮', label: '模糊·底座系数', min: 0, max: 1, step: 0.05, def: 0.3, decimals: 2, unit: '' },
  { id: 12, group: '底座·按钮', label: '静止折射', min: 0, max: 16, step: 0.5, def: 5, decimals: 1, unit: 'dp' },
  { id: 13, group: '底座·按钮', label: '按压折射增量', min: 0, max: 24, step: 0.5, def: 11, decimals: 1, unit: 'dp' },
  { id: 14, group: '底座·按钮', label: '高光·静止', min: 0, max: 1, step: 0.05, def: 0.6, decimals: 2, unit: '' },
  { id: 15, group: '底座·按钮', label: '高光·按压增量', min: 0, max: 1, step: 0.05, def: 0.4, decimals: 2, unit: '' },
  { id: 16, group: '底座·按钮', label: '阴影·静止', min: 0, max: 1, step: 0.05, def: 0.4, decimals: 2, unit: '' },
  { id: 17, group: '底座·按钮', label: '阴影·按压增量', min: 0, max: 1, step: 0.05, def: 0.6, decimals: 2, unit: '' },
  { id: 18, group: '底座·按钮', label: '内阴影半径', min: 0, max: 16, step: 0.5, def: 6, decimals: 1, unit: 'dp' },
  { id: 19, group: '底座·按钮', label: '按压缩放', min: 1, max: 1.4, step: 0.01, def: 1.14, decimals: 2, unit: '' },
  { id: 20, group: '底座·布局', label: '底板内边距', min: 0, max: 24, step: 0.5, def: 14, decimals: 1, unit: 'dp' },
  { id: 21, group: '底座·布局', label: '拖动视差', min: 0, max: 16, step: 0.5, def: 4, decimals: 1, unit: 'dp' },
  { id: 22, group: '底座·布局', label: '外边距·水平', min: 0, max: 90, step: 1, def: 64, decimals: 0, unit: 'dp' },
  { id: 23, group: '底座·布局', label: '外边距·垂直', min: 0, max: 60, step: 1, def: 24, decimals: 0, unit: 'dp' },
  { id: 24, group: '底座·布局', label: '页签字号', min: 6, max: 22, step: 0.5, def: 8, decimals: 1, unit: 'sp' },
  { id: 25, group: '底座·布局', label: '选中字透明度', min: 0, max: 1, step: 0.05, def: 1, decimals: 2, unit: '' },
  { id: 26, group: '底座·布局', label: '未选中字透明度', min: 0, max: 1, step: 0.05, def: 0.65, decimals: 2, unit: '' },
  { id: 27, group: '底座·动画', label: '回弹阻尼', min: 0.2, max: 1.5, step: 0.05, def: 1, decimals: 2, unit: '' },
  { id: 28, group: '底座·动画', label: '回弹刚度', min: 50, max: 1000, step: 10, def: 300, decimals: 0, unit: '' },
  { id: 29, group: '底座·动画', label: '按钮阻尼', min: 0.2, max: 1.5, step: 0.05, def: 0.45, decimals: 2, unit: '' },
  { id: 30, group: '底座·动画', label: '按钮刚度', min: 100, max: 1500, step: 10, def: 600, decimals: 0, unit: '' },
  { id: 31, group: '底座·动画', label: '光斑阻尼', min: 0.2, max: 1.5, step: 0.05, def: 0.5, decimals: 2, unit: '' },
  { id: 32, group: '底座·动画', label: '光斑刚度', min: 50, max: 1000, step: 10, def: 300, decimals: 0, unit: '' },
  { id: 33, group: '指示器·静止', label: '折射高度', min: 0, max: 32, step: 0.1, def: 0, decimals: 1, unit: 'dp' },
  { id: 34, group: '指示器·静止', label: '折射强度', min: 0, max: 32, step: 0.1, def: 0, decimals: 1, unit: 'dp' },
  { id: 35, group: '指示器·静止', label: '选中底色', min: 0, max: 0.25, step: 0.005, def: 0.19, decimals: 3, unit: '' },
  { id: 36, group: '指示器·静止', label: '表面白', min: 0, max: 0.2, step: 0.005, def: 0, decimals: 3, unit: '' },
  { id: 37, group: '指示器·静止', label: '边缘高光', min: 0, max: 1, step: 0.05, def: 0, decimals: 2, unit: '' },
  { id: 38, group: '指示器·静止', label: '阴影', min: 0, max: 1, step: 0.05, def: 0, decimals: 2, unit: '' },
  { id: 39, group: '指示器·静止', label: '内阴影半径', min: 0, max: 12, step: 0.5, def: 0, decimals: 1, unit: 'dp' },
  { id: 40, group: '指示器·静止', label: '内阴影强度', min: 0, max: 1, step: 0.05, def: 0, decimals: 2, unit: '' },
  { id: 41, group: '指示器·布局', label: '指示器高度', min: 36, max: 64, step: 1, def: 48, decimals: 0, unit: 'dp' },
  { id: 42, group: '指示器·布局', label: '胶囊内缩', min: 0, max: 12, step: 0.5, def: 0, decimals: 1, unit: 'dp' },
  { id: 43, group: '指示器·拖动', label: '折射高度', min: 0, max: 32, step: 0.1, def: 13, decimals: 1, unit: 'dp' },
  { id: 44, group: '指示器·拖动', label: '折射强度', min: 0, max: 32, step: 0.1, def: 10, decimals: 1, unit: 'dp' },
  { id: 45, group: '指示器·拖动', label: '选中底色', min: 0, max: 0.25, step: 0.005, def: 0, decimals: 3, unit: '' },
  { id: 46, group: '指示器·拖动', label: '表面白', min: 0, max: 0.2, step: 0.005, def: 0, decimals: 3, unit: '' },
  { id: 47, group: '指示器·拖动', label: '边缘高光', min: 0, max: 1, step: 0.05, def: 0.5, decimals: 2, unit: '' },
  { id: 48, group: '指示器·拖动', label: '阴影', min: 0, max: 1, step: 0.05, def: 0, decimals: 2, unit: '' },
  { id: 49, group: '指示器·拖动', label: '内阴影半径', min: 0, max: 12, step: 0.5, def: 7, decimals: 1, unit: 'dp' },
  { id: 50, group: '指示器·拖动', label: '内阴影强度', min: 0, max: 1, step: 0.05, def: 0.20, decimals: 2, unit: '' },
  { id: 51, group: '指示器·形变', label: '指示器按压缩放', min: 1, max: 1.8, step: 0.01, def: 1.44, decimals: 3, unit: '' },
  { id: 52, group: '指示器·形变', label: '速度形变·横向', min: 0, max: 1.5, step: 0.05, def: 0.75, decimals: 2, unit: '' },
  { id: 53, group: '指示器·形变', label: '速度形变·纵向', min: 0, max: 1.5, step: 0.05, def: 0.4, decimals: 2, unit: '' },
  { id: 81, group: '指示器·形变', label: '按压采样缩小', min: 0, max: 0.3, step: 0.01, def: 0.18, decimals: 2, unit: '' },
  { id: 82, group: '指示器·形变', label: '按压横向缩放', min: 1, max: 1.8, step: 0.01, def: 1.26, decimals: 2, unit: '' },
  { id: 89, group: '指示器·形变', label: '按压文字放大', min: 0, max: 0.6, step: 0.01, def: 0.2, decimals: 2, unit: '' },
  { id: 54, group: '指示器·动画', label: '位移阻尼', min: 0.2, max: 1.5, step: 0.05, def: 1, decimals: 2, unit: '' },
  { id: 55, group: '指示器·动画', label: '位移刚度', min: 100, max: 2000, step: 10, def: 400, decimals: 0, unit: '' },
  { id: 56, group: '指示器·动画', label: '速度阻尼', min: 0.2, max: 1.5, step: 0.05, def: 0.45, decimals: 2, unit: '' },
  { id: 57, group: '指示器·动画', label: '速度刚度', min: 50, max: 1000, step: 10, def: 400, decimals: 0, unit: '' },
  { id: 58, group: '指示器·动画', label: '进度阻尼', min: 0.2, max: 1.5, step: 0.05, def: 1, decimals: 2, unit: '' },
  { id: 59, group: '指示器·动画', label: '进度刚度', min: 100, max: 2000, step: 10, def: 1000, decimals: 0, unit: '' },
  { id: 60, group: '指示器·动画', label: '缩放X阻尼', min: 0.2, max: 1.5, step: 0.05, def: 0.6, decimals: 2, unit: '' },
  { id: 61, group: '指示器·动画', label: '缩放X刚度', min: 50, max: 1000, step: 10, def: 250, decimals: 0, unit: '' },
  { id: 62, group: '指示器·动画', label: '缩放Y阻尼', min: 0.2, max: 1.5, step: 0.05, def: 0.7, decimals: 2, unit: '' },
  { id: 63, group: '指示器·动画', label: '缩放Y刚度', min: 50, max: 1000, step: 10, def: 250, decimals: 0, unit: '' },
  { id: 70, group: '底座·材质', label: '投影模糊半径', min: 0, max: 60, step: 1, def: 24, decimals: 0, unit: 'dp' },
  { id: 71, group: '底座·材质', label: '投影向下偏移', min: 0, max: 24, step: 0.5, def: 6, decimals: 1, unit: 'dp' },
  { id: 72, group: '底座·材质', label: '投影强度', min: 0, max: 0.5, step: 0.01, def: 0.07, decimals: 2, unit: '' },
  { id: 92, group: '底座·材质', label: '按压投影模糊', min: 0, max: 80, step: 1, def: 40, decimals: 0, unit: 'dp' },
  { id: 93, group: '底座·材质', label: '按压投影强度', min: 0, max: 0.2, step: 0.005, def: 0.035, decimals: 3, unit: '' },
  { id: 69, group: '底座·材质', label: '棱边高光宽度', min: 0.1, max: 6, step: 0.1, def: 0.3, decimals: 1, unit: 'dp' },
  { id: 68, group: '底座·材质', label: '底板圆角半径', min: 0, max: 40, step: 0.5, def: 30, decimals: 1, unit: 'dp' },
  { id: 83, group: '底座·布局', label: '页签图标尺寸', min: 16, max: 36, step: 0.5, def: 25.6, decimals: 1, unit: 'dp' },
  { id: 64, group: '底座·布局', label: '背景滚动幅度', min: 1, max: 3, step: 0.1, def: 1.0, decimals: 1, unit: 'x' },
  { id: 65, group: '指示器·静止', label: '模糊半径', min: 0, max: 20, step: 0.1, def: 0, decimals: 1, unit: 'dp' },
  { id: 66, group: '指示器·拖动', label: '模糊半径', min: 0, max: 20, step: 0.1, def: 0, decimals: 1, unit: 'dp' },
  { id: 67, group: '指示器·布局', label: '指示器外扩', min: 0, max: 24, step: 0.5, def: 10, decimals: 1, unit: 'dp' },
  { id: 73, group: '底座·材质', label: '浮动件圆角·左上', min: 0, max: 40, step: 0.5, def: 25, decimals: 1, unit: 'dp' },
  { id: 74, group: '底座·材质', label: '浮动件圆角·右上', min: 0, max: 40, step: 0.5, def: 25, decimals: 1, unit: 'dp' },
  { id: 75, group: '底座·材质', label: '浮动件圆角·右下', min: 0, max: 40, step: 0.5, def: 25, decimals: 1, unit: 'dp' },
  { id: 76, group: '底座·材质', label: '浮动件圆角·左下', min: 0, max: 40, step: 0.5, def: 25, decimals: 1, unit: 'dp' },
  { id: 77, group: '底座·材质', label: '亮度', min: -0.5, max: 0.5, step: 0.01, def: 0, decimals: 2, unit: '' },
  { id: 78, group: '底座·材质', label: '对比度', min: 0, max: 2, step: 0.01, def: 1, decimals: 2, unit: '' },
  { id: 79, group: '底座·材质', label: '饱和度', min: 0, max: 2, step: 0.01, def: 1, decimals: 2, unit: '' },
  { id: 80, group: '底座·材质', label: '效果不透明度', min: 0, max: 1, step: 0.01, def: 1, decimals: 2, unit: '' }
];

export const TOGGLE_SPECS: ToggleSpec[] = [
  { id: 0, label: '色散（底座）', def: false },
  { id: 1, label: '提饱和（底座）', def: true },
  { id: 2, label: '色散（指示器）', def: true },
  { id: 5, label: '环境光（棱边受光半圈）', def: false },
  { id: 6, label: '深度效应（折射）', def: false },
  { id: 7, label: '纯色高光（底座）', def: false }
];

/**
 * Dialog 场景参数表：分为「弹窗·材质」（id 94..116）与「按钮·材质」（id 117..139）。
 * 与 `backdrop/src/main/cpp/glass/glass_params.h` 中的 pid::kDialog* 一一对应。
 */
export const DIALOG_NUMBER_SPECS: NumberSpec[] = [
  // ---- 弹窗 · 材质（94..116）----
  { id: 94, group: '弹窗·材质', label: '背景暗化', min: 0, max: 0.8, step: 0.01, def: 0.34, decimals: 2, unit: '' },
  { id: 95, group: '弹窗·材质', label: '弹窗圆角半径', min: 0, max: 70, step: 0.5, def: 43.0, decimals: 1, unit: 'dp' },
  { id: 96, group: '弹窗·材质', label: 'G2圆角平滑度', min: 0, max: 0.65, step: 0.01, def: 0.45, decimals: 2, unit: '' },
  { id: 97, group: '弹窗·材质', label: '模糊半径', min: 0, max: 36, step: 0.5, def: 3.0, decimals: 1, unit: 'dp' },
  { id: 98, group: '弹窗·材质', label: '折射高度', min: 0, max: 48, step: 0.5, def: 35.0, decimals: 1, unit: 'dp' },
  { id: 99, group: '弹窗·材质', label: '折射强度', min: 0, max: 80, step: 0.5, def: 63.5, decimals: 1, unit: 'dp' },
  { id: 100, group: '弹窗·材质', label: '边缘清晰折射', min: 0, max: 1, step: 0.02, def: 0.00, decimals: 2, unit: '' },
  { id: 101, group: '弹窗·材质', label: '边缘底色保留', min: 0, max: 1, step: 0.02, def: 0.32, decimals: 2, unit: '' },
  { id: 102, group: '弹窗·材质', label: '提饱和度', min: 0.5, max: 2.5, step: 0.05, def: 1.35, decimals: 2, unit: '' },
  { id: 103, group: '弹窗·材质', label: '亮度', min: -0.5, max: 0.5, step: 0.01, def: 0.18, decimals: 2, unit: '' },
  { id: 104, group: '弹窗·材质', label: '对比度', min: 0.5, max: 1.5, step: 0.01, def: 1.00, decimals: 2, unit: '' },
  { id: 105, group: '弹窗·材质', label: '底色不透明度', min: 0, max: 0.95, step: 0.01, def: 0.17, decimals: 2, unit: '' },
  { id: 106, group: '弹窗·材质', label: '底色·R', min: 0, max: 1, step: 0.01, def: 0.96, decimals: 2, unit: '' },
  { id: 107, group: '弹窗·材质', label: '底色·G', min: 0, max: 1, step: 0.01, def: 0.97, decimals: 2, unit: '' },
  { id: 108, group: '弹窗·材质', label: '底色·B', min: 0, max: 1, step: 0.01, def: 1.00, decimals: 2, unit: '' },
  { id: 109, group: '弹窗·材质', label: '棱边高光强度', min: 0, max: 1.5, step: 0.02, def: 0.40, decimals: 2, unit: '' },
  { id: 110, group: '弹窗·材质', label: '棱边高光宽度', min: 0.1, max: 4.0, step: 0.05, def: 0.65, decimals: 2, unit: 'dp' },
  { id: 111, group: '弹窗·材质', label: '环境光强度', min: 0, max: 1.0, step: 0.02, def: 0.48, decimals: 2, unit: '' },
  { id: 112, group: '弹窗·材质', label: '内阴影半径', min: 0, max: 32, step: 0.5, def: 0.0, decimals: 1, unit: 'dp' },
  { id: 113, group: '弹窗·材质', label: '内阴影强度', min: 0, max: 0.5, step: 0.01, def: 0.00, decimals: 2, unit: '' },
  { id: 114, group: '弹窗·材质', label: '外投影模糊', min: 0, max: 64, step: 1, def: 45.0, decimals: 0, unit: 'dp' },
  { id: 115, group: '弹窗·材质', label: '外投影偏移', min: 0, max: 20, step: 0.5, def: 5.0, decimals: 1, unit: 'dp' },
  { id: 116, group: '弹窗·材质', label: '外投影强度', min: 0, max: 0.5, step: 0.01, def: 0.14, decimals: 2, unit: '' },

  // ---- 按钮 · 材质（117..139）----
  { id: 117, group: '按钮·材质', label: '按钮圆角半径', min: 0, max: 30, step: 0.5, def: 25.0, decimals: 1, unit: 'dp' },
  { id: 118, group: '按钮·材质', label: 'G2圆角平滑度', min: 0, max: 0.65, step: 0.01, def: 0.00, decimals: 2, unit: '' },
  { id: 119, group: '按钮·材质', label: '模糊半径', min: 0, max: 24, step: 0.5, def: 24.0, decimals: 1, unit: 'dp' },
  { id: 120, group: '按钮·材质', label: '折射高度', min: 0, max: 32, step: 0.5, def: 20.5, decimals: 1, unit: 'dp' },
  { id: 121, group: '按钮·材质', label: '折射强度', min: 0, max: 60, step: 0.5, def: 40.0, decimals: 1, unit: 'dp' },
  { id: 122, group: '按钮·材质', label: '边缘清晰折射', min: 0, max: 1, step: 0.02, def: 0.00, decimals: 2, unit: '' },
  { id: 123, group: '按钮·材质', label: '边缘底色保留', min: 0, max: 1, step: 0.02, def: 0.68, decimals: 2, unit: '' },
  { id: 124, group: '按钮·材质', label: '提饱和度', min: 0.5, max: 2.5, step: 0.05, def: 1.40, decimals: 2, unit: '' },
  { id: 125, group: '按钮·材质', label: '亮度', min: -0.5, max: 0.5, step: 0.01, def: -0.03, decimals: 2, unit: '' },
  { id: 126, group: '按钮·材质', label: '对比度', min: 0.5, max: 1.5, step: 0.01, def: 1.00, decimals: 2, unit: '' },
  { id: 127, group: '按钮·材质', label: '底色不透明度', min: 0, max: 0.95, step: 0.01, def: 0.25, decimals: 2, unit: '' },
  { id: 128, group: '按钮·材质', label: '底色·R', min: 0, max: 1, step: 0.01, def: 1.00, decimals: 2, unit: '' },
  { id: 129, group: '按钮·材质', label: '底色·G', min: 0, max: 1, step: 0.01, def: 1.00, decimals: 2, unit: '' },
  { id: 130, group: '按钮·材质', label: '底色·B', min: 0, max: 1, step: 0.01, def: 1.00, decimals: 2, unit: '' },
  { id: 131, group: '按钮·材质', label: '棱边高光强度', min: 0, max: 1.5, step: 0.02, def: 0.22, decimals: 2, unit: '' },
  { id: 132, group: '按钮·材质', label: '棱边高光宽度', min: 0.1, max: 4.0, step: 0.05, def: 0.45, decimals: 2, unit: 'dp' },
  { id: 133, group: '按钮·材质', label: '环境光强度', min: 0, max: 1.0, step: 0.02, def: 0.32, decimals: 2, unit: '' },
  { id: 134, group: '按钮·材质', label: '内阴影半径', min: 0, max: 24, step: 0.5, def: 0.0, decimals: 1, unit: 'dp' },
  { id: 135, group: '按钮·材质', label: '内阴影强度', min: 0, max: 0.5, step: 0.01, def: 0.00, decimals: 2, unit: '' },
  { id: 136, group: '按钮·材质', label: '外投影模糊', min: 0, max: 40, step: 0.5, def: 19.0, decimals: 1, unit: 'dp' },
  { id: 137, group: '按钮·材质', label: '外投影偏移', min: 0, max: 16, step: 0.5, def: 3.0, decimals: 1, unit: 'dp' },
  { id: 138, group: '按钮·材质', label: '外投影强度', min: 0, max: 0.4, step: 0.005, def: 0.050, decimals: 3, unit: '' },
  { id: 139, group: '按钮·材质', label: '按压放大增量', min: 0, max: 16, step: 0.5, def: 1.5, decimals: 1, unit: 'dp' }
];

export const DIALOG_TOGGLE_SPECS: ToggleSpec[] = [
  { id: 8, group: '弹窗·材质', label: '色散（弹窗）', def: false },
  { id: 9, group: '弹窗·材质', label: '深度效应（弹窗）', def: false },
  { id: 10, group: '弹窗·材质', label: '环境光（弹窗）', def: true },
  { id: 11, group: '弹窗·材质', label: '纯色高光（弹窗）', def: false },
  { id: 12, group: '按钮·材质', label: '色散（按钮）', def: false },
  { id: 13, group: '按钮·材质', label: '深度效应（按钮）', def: true },
  { id: 14, group: '按钮·材质', label: '环境光（按钮）', def: true },
  { id: 15, group: '按钮·材质', label: '纯色高光（按钮）', def: false }
];

export function buildDefaults(): number[] {
  const arr: number[] = [];
  for (let i = 0; i < 140; i++) {
    arr[i] = 0;
  }
  for (let i = 0; i < NUMBER_SPECS.length; i++) {
    arr[NUMBER_SPECS[i].id] = NUMBER_SPECS[i].def;
  }
  for (let i = 0; i < DIALOG_NUMBER_SPECS.length; i++) {
    arr[DIALOG_NUMBER_SPECS[i].id] = DIALOG_NUMBER_SPECS[i].def;
  }
  return arr;
}

export function buildToggleDefaults(): boolean[] {
  // 0..7 = 页签场景开关（保持既有初始态）；8..15 = Dialog 场景开关（定版默认值）
  return [
    true, true, true, false, false, false, false, false,
    false, false, true, false, false, true, true, false
  ];
}
