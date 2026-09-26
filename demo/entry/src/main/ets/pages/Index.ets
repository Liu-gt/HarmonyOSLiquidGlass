import { GlassGLSetPaused } from 'backdrop';
import { CatalogHome } from '../catalog/CatalogHome';
import { ScenePage } from '../catalog/ScenePage';

const SCENE_HOME: number = -1;   // 目录首页

/**
 * 应用入口：目录首页 <-> 场景页。
 *
 * 采用状态驱动页面切换，确保同一时刻仅激活一个 XComponent Surface。
 */
@Entry
@Component
struct Index {
  @State sceneId: number = SCENE_HOME;
  @State sceneTitle: string = '';
  @State sceneMenu: boolean = false;

  build() {
    Stack() {
      if (this.sceneId < 0) {
        CatalogHome({
          onOpen: (id: number, title: string, showMenu: boolean) => {
            this.sceneTitle = title;
            this.sceneMenu = showMenu;
            this.sceneId = id;
          }
        })
      } else {
        ScenePage({
          sceneId: this.sceneId,
          title: this.sceneTitle,
          showMenu: this.sceneMenu,
          onBack: () => {
            this.sceneId = SCENE_HOME;
          }
        })
      }
    }
    .width('100%')
    .height('100%')
    // 沉浸式：绘制延伸到状态栏与导航条下（子组件仍在安全区内布局）
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM])
  }

  onBackPress(): boolean {
    if (this.sceneId !== SCENE_HOME) {
      this.sceneId = SCENE_HOME;
      return true;
    }
    return false;
  }

  // 页面可见性生命周期：页面不可见时暂停 GL 渲染以降低功耗，恢复可见时恢复渲染循环。
  onPageShow(): void {
    if (GlassGLSetPaused !== undefined) {
      GlassGLSetPaused(false);
    }
  }

  onPageHide(): void {
    if (GlassGLSetPaused !== undefined) {
      GlassGLSetPaused(true);
    }
  }
}
