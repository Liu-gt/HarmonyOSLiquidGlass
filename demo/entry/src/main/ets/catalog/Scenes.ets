// ---------------------------------------------------------------------------
// 目录场景表：与原生 `backdrop/src/main/cpp/glass/glass_scene.h` 的枚举一一对应。
//
// 新增场景的顺序：先在 glass_scene.h 里加枚举 -> 这里加一条 SceneDef -> 原生实现
// 对应的 Draw*Scene。**id 必须与枚举值完全一致**（传错就是另一个画面）。
// ---------------------------------------------------------------------------

export const SCENE_TABS: number = 0;

/**
 * ArkTS 实时内容玻璃：XComponent 仍是 `glassSurface`，但背景换成 ArkUI 自己渲染的
 * 内容 —— ArkTS 侧用 `BuilderNode(RENDER_TYPE_TEXTURE)` 把组件树导出成纹理，
 * 原生侧每帧采它当玻璃背景。**id 必须与 glass_scene.h 的 kArkTsGlass 一致。**
 */
export const SCENE_ARKTS_GLASS: number = 13;

export interface SceneDef {
  id: number;
  key: string;
  title: string;
  group: string;
  /** 页签场景带"玻璃参数"菜单（参数编号与原生参数表一致） */
  showMenu: boolean;
}

export const GROUP_LIQUID: string = 'Liquid glass components';
export const GROUP_SYSTEM: string = 'System UIs';
export const GROUP_EXPERIMENTS: string = 'Experiments';
export const GROUP_ARKUI: string = 'ArkUI (ArkTS)';

export const GROUP_ORDER: string[] = [GROUP_LIQUID, GROUP_SYSTEM, GROUP_EXPERIMENTS, GROUP_ARKUI];

export const SCENES: SceneDef[] = [
  { id: 1, key: 'buttons', title: 'Buttons', group: GROUP_LIQUID, showMenu: false },
  { id: 2, key: 'toggle', title: 'Toggle', group: GROUP_LIQUID, showMenu: false },
  { id: 3, key: 'slider', title: 'Slider', group: GROUP_LIQUID, showMenu: false },
  { id: 0, key: 'tabs', title: 'Bottom tabs', group: GROUP_LIQUID, showMenu: true },
  { id: 4, key: 'dialog', title: 'Dialog', group: GROUP_LIQUID, showMenu: false },

  { id: 6, key: 'lock_screen', title: 'Lock screen', group: GROUP_SYSTEM, showMenu: false },
  { id: 5, key: 'control_center', title: 'Control center', group: GROUP_SYSTEM, showMenu: false },
  { id: 7, key: 'magnifier', title: 'Magnifier', group: GROUP_SYSTEM, showMenu: false },

  { id: 12, key: 'playground', title: 'Glass playground', group: GROUP_EXPERIMENTS, showMenu: false },
  { id: 9, key: 'adaptive', title: 'Adaptive luminance glass', group: GROUP_EXPERIMENTS, showMenu: false },
  { id: 8, key: 'progressive_blur', title: 'Progressive blur', group: GROUP_EXPERIMENTS, showMenu: false },
  { id: 10, key: 'scroll', title: 'Scroll container', group: GROUP_EXPERIMENTS, showMenu: false },
  { id: 11, key: 'lazy_scroll', title: 'Lazy scroll container', group: GROUP_EXPERIMENTS, showMenu: false },

  { id: SCENE_ARKTS_GLASS, key: 'arkts_glass', title: 'ArkTS content glass', group: GROUP_ARKUI, showMenu: false }
];
