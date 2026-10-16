// ---------------------------------------------------------------------------
// 「ArkTS content glass」场景里被玻璃折射的内容。
//
// 这块 UI 由 ArkUI 正常渲染，但不直接送屏：ScenePage 用
// `BuilderNode(RENDER_TYPE_TEXTURE)` 把它导出成纹理（接收端是原生侧的
// OH_NativeImage），GL 每帧采样这张纹理当背景 —— 玻璃里的折射是实时的，
// 动画（LoadingProgress / TextClock）与交互（Slider / Toggle）都能透出来。
//
// 布局里的 vp 数值与原生 `DrawArkTsGlassScene` 的玻璃几何对应：
//   卡片区 190..340vp  -> 大玻璃卡片
//   滑块区 416..460vp  -> 玻璃胶囊
// 改这里的位置时记得同步改那边（反之亦然）。
// ---------------------------------------------------------------------------

@Builder
function arkTsGlassContentBuilder() {
  ArkTsGlassContent()
}

/**
 * 导出给 ScenePage 的 BuilderNode 用。
 *
 * 用 `wrapBuilder` 包成可按引用传递的对象：跨文件直接用 @Builder 函数有导入限制，
 * 这样只导出一个值，最省事。
 */
export const ARKTS_GLASS_CONTENT: WrappedBuilder<[]> = wrapBuilder(arkTsGlassContentBuilder);

@Component
export struct ArkTsGlassContent {
  @State value: number = 35;

  @Builder
  card(colorA: string, colorB: string, title: string) {
    Column() {
      Text(title)
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
        .fontColor(Color.White)
        .margin({ top: 12, left: 12 })
    }
    .alignItems(HorizontalAlign.Start)
    .layoutWeight(1)
    .height('100%')
    .borderRadius(22)
    .linearGradient({ angle: 135, colors: [[colorA, 0.0], [colorB, 1.0]] })
  }

  build() {
    Column() {
      // 让开 ScenePage 的返回 chip（ArkUI 覆盖层，不在本纹理里）
      Blank().height(130)

      Text('ArkTS 实时内容')
        .fontSize(28)
        .fontWeight(FontWeight.Bold)
        .fontColor(Color.White)
        .height(40)

      // ---- 卡片区（190..340vp）：大玻璃卡片压在这里 ----
      Row({ space: 12 }) {
        this.card('#FF7A45', '#FF3B6B', '折射')
        this.card('#2E9BFF', '#5B4DFF', '色散')
        this.card('#22C55E', '#0EA5E9', '实时')
      }
      .width('100%')
      .height(150)
      .margin({ top: 20 })

      // ---- 实时性证明：转圈 + 每秒跳动的时钟 ----
      Row({ space: 12 }) {
        LoadingProgress()
          .width(36)
          .height(36)
          .color('#4DA3FF')
        TextClock()
          .fontSize(24)
          .fontWeight(FontWeight.Medium)
          .fontColor(Color.White)
        Blank()
        Text('ArkUI 渲染中')
          .fontSize(12)
          .fontColor('#80FFFFFF')
      }
      .width('100%')
      .height(48)
      .margin({ top: 16 })

      // ---- 滑块区（416..460vp）：玻璃胶囊压在这里，拖动隔着玻璃可见 ----
      Row({ space: 12 }) {
        Slider({ value: this.value, min: 0, max: 100, style: SliderStyle.OutSet })
          .layoutWeight(1)
          .height(44)
          .trackColor('#33FFFFFF')
          .selectedColor('#4DA3FF')
          .blockColor(Color.White)
          .onChange((v: number) => {
            this.value = v;
          })
        Text(this.value.toFixed(0) + '%')
          .fontSize(13)
          .fontColor('#CCFFFFFF')
          .width(44)
          .textAlign(TextAlign.End)
      }
      .width('100%')
      .height(44)
      .margin({ top: 12 })


      Blank()
    }
    .width('100%')
    .height('100%')
    .padding({ left: 20, right: 20 })
    .alignItems(HorizontalAlign.Start)
    .backgroundColor('#0E0E12')
  }
}
