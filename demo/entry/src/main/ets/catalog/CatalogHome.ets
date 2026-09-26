import { SCENES, SceneDef, GROUP_ORDER } from './Scenes';

/**
 * 目录首页（对齐 Kyant 参考实现的 HomeContent）：
 * 标题 + 三个分组（Liquid glass components / System UIs / Experiments），
 * 点条目进入对应场景页。
 */
@Component
export struct CatalogHome {
  onOpen: (id: number, title: string, showMenu: boolean) => void =
    (id: number, title: string, showMenu: boolean) => {};

  private scenesOf(group: string): SceneDef[] {
    return SCENES.filter((s: SceneDef) => s.group === group);
  }

  build() {
    Column() {
      Scroll() {
        Column({ space: 0 }) {
          Text('Backdrop Catalog')
            .fontSize(28)
            .fontWeight(FontWeight.Medium)
            .fontColor(Color.White)
            .width('100%')
            .padding({ left: 16, right: 16, top: 56, bottom: 16 })

          ForEach(GROUP_ORDER, (group: string) => {
            Text(group)
              .fontSize(15)
              .fontWeight(FontWeight.Medium)
              .fontColor('#0088FF')
              .width('100%')
              .padding({ left: 16, right: 16, top: 24, bottom: 8 })
            ForEach(this.scenesOf(group), (scene: SceneDef) => {
              Text(scene.title)
                .fontSize(17)
                .fontColor(Color.White)
                .width('100%')
                .padding(16)
                .onClick(() => {
                  this.onOpen(scene.id, scene.title, scene.showMenu);
                })
            }, (scene: SceneDef) => scene.key)
          }, (group: string) => group)

          Blank().height(32)
        }
        .width('100%')
      }
      .width('100%')
      .height('100%')
      .align(Alignment.Top)
      .scrollable(ScrollDirection.Vertical)
      .scrollBar(BarState.Off)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0F0F12')
    // 沉浸式：背景延伸到状态栏与导航条下（列表内容仍在安全区内布局）
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM])
  }
}
