import { hilog } from '@kit.PerformanceAnalysisKit';
import { BuilderNode, FrameNode, NodeController, NodeRenderType, UIContext } from '@kit.ArkUI';
import {
  GlassGLGetContentSurfaceId,
  GlassGLRegisterMenuCallback,
  GlassGLSetBool,
  GlassGLSetDensity,
  GlassGLSetNumber,
  GlassGLSetPhoto,
  GlassGLSetScene,
  GlassGLUnregisterMenuCallback
} from 'backdrop';
import { NumberSpec, ToggleSpec, NUMBER_SPECS, TOGGLE_SPECS, buildDefaults } from './Specs';
import { ARKTS_GLASS_CONTENT } from './ArkTsGlassContent';
import { SCENE_ARKTS_GLASS } from './Scenes';

const DOMAIN = 0x0000;
const TAG = 'GLASS';
const SURFACE_ID = 'glassSurface';

/**
 * ArkTS 内容 -> GL 纹理的桥。
 *
 * `BuilderNode(RENDER_TYPE_TEXTURE)` 把组件树渲染进原生侧给的 surfaceId
 * （接收端是 OH_NativeImage），GL 每帧采它当玻璃背景；纹理是 GPU 直达，不过 CPU。
 * 触摸由 ScenePage 的透明层转发进来（postInputEvent），内容照常可交互。
 */
class ArkTsGlassContentController extends NodeController {
  private surfaceId: string = '';
  private root: BuilderNode<[]> | null = null;

  setSurfaceId(id: string): void {
    this.surfaceId = id;
  }

  makeNode(uiContext: UIContext): FrameNode | null {
    if (this.root === null) {
      this.root = new BuilderNode<[]>(uiContext, {
        type: NodeRenderType.RENDER_TYPE_TEXTURE,
        surfaceId: this.surfaceId
      });
      this.root.build(ARKTS_GLASS_CONTENT);
    }
    return this.root.getFrameNode();
  }

  /**
   * 转发触摸。
   *
   * 用 `postInputEvent`（API 20+）而不是 `postTouchEvent`：前者要的是 **window 坐标
   * （px）**，后者要的是"相对目标组件的本地 px 坐标"，两种都得自己换算，但
   * postInputEvent 的命中测试实测才稳定（postTouchEvent 在同样坐标下返回 false）。
   *
   * 事件里的 windowX/windowY 单位是 vp，所以要按官方示例换算成 px：
   * 触摸点全局位置 = 接收组件的 globalPosition + 组件内局部坐标。
   */
  postEvent(event: TouchEvent, uiContext: UIContext): boolean {
    if (this.root === null) {
      return false;
    }
    const gx: number = event.target.area.globalPosition.x as number;
    const gy: number = event.target.area.globalPosition.y as number;
    for (let i = 0; i < event.touches.length; i++) {
      event.touches[i].windowX = uiContext.vp2px(gx + event.touches[i].x);
      event.touches[i].windowY = uiContext.vp2px(gy + event.touches[i].y);
    }
    for (let i = 0; i < event.changedTouches.length; i++) {
      event.changedTouches[i].windowX = uiContext.vp2px(gx + event.changedTouches[i].x);
      event.changedTouches[i].windowY = uiContext.vp2px(gy + event.changedTouches[i].y);
    }
    return this.root.postInputEvent(event);
  }

  dispose(): void {
    if (this.root !== null) {
      this.root.dispose();
      this.root = null;
    }
  }
}

/**
 * 单个目录场景页：整屏一个 XComponent(SURFACE)，所有绘制（背景 / 玻璃 / 折射 /
 * 色散 / 文字）都在 `backdrop` 库的渲染线程里用 GLES 完成。
 *
 * ArkUI 只提供：① 宿主 XComponent；② 返回按钮 / 标题；③（仅页签场景）参数菜单。
 * 菜单直接调 `GlassGLSetNumber/GlassGLSetBool`，编号与 `glass_params.cpp` 一一对应。
 */
@Component
export struct ScenePage {
  @Prop sceneId: number;
  @Prop title: string;
  @Prop showMenu: boolean;
  onBack: () => void = () => {};

  @State menuOpen: boolean = false;
  @State activeGroup: number = 0;
  @State values: number[] = buildDefaults();
  @State toggles: boolean[] = [true, true, true, false, false, false, false, false];
  @State resetNonce: number = 0;
  /** 原生侧就绪后拿到的内容纹理 surfaceId（非空才挂 NodeContainer） */
  @State contentSurfaceId: string = '';

  private alive: boolean = true;
  private contentPollTimer: number = -1;
  private contentController: ArkTsGlassContentController = new ArkTsGlassContentController();

  aboutToAppear(): void {
    // 原生侧按 dp 排版：把本设备的显示密度报上去（vp -> px）
    GlassGLSetDensity(this.getUIContext().vp2px(1));

    if (this.showMenu) {
      // 页签场景：把描述表里的默认值推给原生渲染器（其它场景用固定参考值）
      for (let i = 0; i < this.values.length; i++) {
        GlassGLSetNumber(i, this.values[i]);
      }
      for (let i = 0; i < this.toggles.length; i++) {
        GlassGLSetBool(i, this.toggles[i]);
      }
    }

    GlassGLSetScene(this.sceneId);
    hilog.info(DOMAIN, TAG, 'scene page appear scene=%{public}d', this.sceneId);

    this.alive = true;
    if (this.showMenu && GlassGLRegisterMenuCallback !== undefined) {
      // 页签场景顶部三个玻璃按钮画在 GL 里、点击由原生判定；这里接住"该开哪个菜单"
      GlassGLRegisterMenuCallback(this.onMenuRequest);
    }
    this.loadPhoto();

    if (this.sceneId === SCENE_ARKTS_GLASS) {
      // 渲染线程起来后才会建 OH_NativeImage，surfaceId 拿不到就等下一拍
      this.pollContentSurfaceId();
    }
  }

  aboutToDisappear(): void {
    // 页面要走人了：先摘掉回链，免得原生的点击回调打到已销毁的页面上
    this.alive = false;
    if (GlassGLUnregisterMenuCallback !== undefined) {
      GlassGLUnregisterMenuCallback();
    }
    if (this.contentPollTimer >= 0) {
      clearTimeout(this.contentPollTimer);
      this.contentPollTimer = -1;
    }
    // 先拆 BuilderNode 再让 XComponent 走（surface 销毁时原生会释放 OH_NativeImage）
    this.contentController.dispose();
  }

  /**
   * 轮询原生侧的内容纹理 surfaceId，拿到就把它交给 BuilderNode。
   *
   * 之所以要轮询：surfaceId 由渲染线程在 `InitGl` 时创建 OH_NativeImage 后才有，
   * 而页面 `aboutToAppear` 早于 XComponent 的 surface 创建。
   */
  private pollContentSurfaceId(): void {
    if (!this.alive || this.sceneId !== SCENE_ARKTS_GLASS) {
      return;
    }
    const id: string = GlassGLGetContentSurfaceId();
    if (id.length > 0) {
      hilog.info(DOMAIN, TAG, 'content surfaceId=%{public}s', id);
      this.contentController.setSurfaceId(id);
      this.contentSurfaceId = id;
      return;
    }
    this.contentPollTimer = setTimeout(() => {
      this.pollContentSurfaceId();
    }, 50);
  }

  /**
   * 原生点击顶部按钮 -> 开对应分组的参数菜单。
   * target：0=底座，1=指示器·静止，2=指示器·拖动。
   */
  private onMenuRequest = (target: number): void => {
    if (!this.alive) {
      return;
    }
    const name: string =
      target === 2 ? '指示器·拖动' : (target === 1 ? '指示器·静止' : '底座·材质');
    const idx: number = this.groups().indexOf(name);
    if (idx >= 0) {
      this.activeGroup = idx;
    }
    this.menuOpen = true;
    hilog.info(DOMAIN, TAG, 'menu request target=%{public}d group=%{public}d', target, idx);
  };

  /**
   * 把 rawfile 里的裸 RGBA 照片（前 8 字节 = u32 LE 宽、高）交给原生。
   * 走 ArrayBuffer 直传：原生侧不需要 rawfile / image 解码 NDK。
   */
  private async loadPhoto(): Promise<void> {
    try {
      const ctx = this.getUIContext().getHostContext();
      if (ctx === undefined) {
        hilog.error(DOMAIN, TAG, 'loadPhoto: host context is undefined');
        return;
      }
      const data: Uint8Array = await ctx.resourceManager.getRawFileContent('photo.rgba');
      GlassGLSetPhoto(data.buffer);
      hilog.info(DOMAIN, TAG, 'photo bytes=%{public}d', data.byteLength);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'loadPhoto failed: %{public}s', JSON.stringify(e));
    }
  }

  private groups(): string[] {
    const out: string[] = [];
    for (let i = 0; i < NUMBER_SPECS.length; i++) {
      if (out.indexOf(NUMBER_SPECS[i].group) < 0) {
        out.push(NUMBER_SPECS[i].group);
      }
    }
    return out;
  }

  private specsOf(group: string): NumberSpec[] {
    return NUMBER_SPECS.filter((s: NumberSpec) => s.group === group);
  }

  private applyNumber(id: number, value: number): void {
    const next: number[] = this.values.slice();
    next[id] = value;
    this.values = next;
    GlassGLSetNumber(id, value);
  }

  private applyToggle(id: number, value: boolean): void {
    const next: boolean[] = this.toggles.slice();
    next[id] = value;
    this.toggles = next;
    GlassGLSetBool(id, value);
  }

  private reset(): void {
    const list: NumberSpec[] = this.specsOf(this.groups()[this.activeGroup]);
    const next: number[] = this.values.slice();
    for (let i = 0; i < list.length; i++) {
      next[list[i].id] = list[i].def;
      GlassGLSetNumber(list[i].id, list[i].def);
    }
    this.values = next;
    this.resetNonce = this.resetNonce + 1;
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      // ArkTS 实时内容玻璃：内容树的宿主。
      // 它只负责布局/渲染到纹理（RENDER_TYPE_TEXTURE 不直接送屏），
      // 尺寸与 XComponent 对齐才能让纹理和屏幕 1:1、触摸坐标不用换算。
      if (this.sceneId === SCENE_ARKTS_GLASS && this.contentSurfaceId.length > 0) {
        NodeContainer(this.contentController)
          .width(LayoutPolicy.matchParent)
          .height(LayoutPolicy.matchParent)
          .ignoreLayoutSafeArea([LayoutSafeAreaType.SYSTEM], [LayoutSafeAreaEdge.TOP, LayoutSafeAreaEdge.BOTTOM])
          .hitTestBehavior(HitTestMode.None)
      }

      // XComponent(SURFACE) 用 expandSafeArea 只会移动 surface、不改变 surface 尺寸（实测 1280x2598），
      // 所以这里用 ignoreLayoutSafeArea 扩布局 + matchParent 撑满，让 GL surface 真正铺满全屏。
      XComponent({
        id: SURFACE_ID,
        type: XComponentType.SURFACE,
        libraryname: 'backdrop'
      })
        .width(LayoutPolicy.matchParent)
        .height(LayoutPolicy.matchParent)
        .ignoreLayoutSafeArea([LayoutSafeAreaType.SYSTEM], [LayoutSafeAreaEdge.TOP, LayoutSafeAreaEdge.BOTTOM])

      // 触摸转发层：XComponent(SURFACE) 的 ArkUI `onTouch` 不触发（事件只走原生回调），
      // 所以在这个场景里叠一层透明节点收事件，再 postTouchEvent 给 BuilderNode 的内容树。
      if (this.sceneId === SCENE_ARKTS_GLASS && this.contentSurfaceId.length > 0) {
        Column()
          .width(LayoutPolicy.matchParent)
          .height(LayoutPolicy.matchParent)
          .ignoreLayoutSafeArea([LayoutSafeAreaType.SYSTEM], [LayoutSafeAreaEdge.TOP, LayoutSafeAreaEdge.BOTTOM])
          .hitTestBehavior(HitTestMode.Block)
          .onTouch((event: TouchEvent) => {
            this.contentController.postEvent(event, this.getUIContext());
          })
      }

      // 返回目录 + 场景标题
      Row({ space: 8 }) {
        Text('‹')
          .fontSize(26)
          .fontColor(Color.White)
          .fontWeight(FontWeight.Medium)
        Text(this.title)
          .fontSize(15)
          .fontColor(Color.White)
          .fontWeight(FontWeight.Medium)
      }
      .padding({ left: 14, right: 16, top: 6, bottom: 6 })
      .backgroundColor('#4D000000')
      .borderRadius(18)
      .margin({ left: 12, top: 44 })
      .onClick(() => {
        this.onBack();
      })

      // 页签场景：原生玻璃按钮的回链兜底（万一回链出问题也能开菜单）
      if (this.showMenu && !this.menuOpen) {
        Button('参数')
          .fontSize(13)
          .height(34)
          .padding({ left: 14, right: 14 })
          .margin({ left: 12, top: 92 })
          .backgroundColor('#CC1C1C1E')
          .onClick(() => {
            this.menuOpen = true;
          })
      }

      if (this.menuOpen) {
        this.Params()
      }
    }
    .width('100%')
    .height('100%')
  }

  @Builder
  Params() {
    Column() {
      Row() {
        Text('玻璃参数').fontSize(15).fontWeight(FontWeight.Medium).fontColor(Color.White).layoutWeight(1)
        Button('重置').fontSize(12).height(28).padding({ left: 12, right: 12 })
          .backgroundColor('#33FFFFFF').onClick(() => { this.reset(); })
        Button('关闭').fontSize(12).height(28).margin({ left: 8 }).padding({ left: 12, right: 12 })
          .backgroundColor('#33FFFFFF').onClick(() => { this.menuOpen = false; })
      }.width('100%').margin({ bottom: 8 })

      Scroll() {
        Column() {
          ForEach(this.groups(), (g: string, gi: number) => {
            Text(g)
              .fontSize(12).fontColor(gi === this.activeGroup ? '#0091FF' : '#B0FFFFFF')
              .width('100%').margin({ top: 6, bottom: 4 })
              .onClick(() => { this.activeGroup = gi; })
          }, (g: string) => 'g_' + g)

          ForEach(this.specsOf(this.groups()[this.activeGroup]), (s: NumberSpec) => {
            Column() {
              Row() {
                Text(s.label).fontSize(12).fontColor('#E0FFFFFF').layoutWeight(1)
                Text(this.format(s)).fontSize(12).fontColor('#8CFFFFFF')
              }.width('100%')
              Slider({ value: this.values[s.id], min: s.min, max: s.max, step: s.step, style: SliderStyle.OutSet })
                .width('100%').height(30)
                .trackColor('#33FFFFFF').selectedColor('#0091FF').blockColor(Color.White)
                .onChange((v: number) => { this.applyNumber(s.id, v); })
            }.width('100%').margin({ bottom: 2 })
          }, (s: NumberSpec) => 'n' + s.id + '_' + this.resetNonce)

          ForEach(TOGGLE_SPECS, (t: ToggleSpec) => {
            Row() {
              Text(t.label).fontSize(12).fontColor('#E0FFFFFF').layoutWeight(1)
              Toggle({ type: ToggleType.Switch, isOn: this.toggles[t.id] })
                .onChange((isOn: boolean) => { this.applyToggle(t.id, isOn); })
            }.width('100%').margin({ top: 8 })
          }, (t: ToggleSpec) => 'b' + t.id)
        }.width('100%')
      }
      .width('100%').align(Alignment.Top).scrollable(ScrollDirection.Vertical).scrollBar(BarState.Auto)
      .constraintSize({ maxHeight: 520 })
    }
    .width(310)
    .padding(14)
    .backgroundColor('#F01C1C1E')
    .borderRadius(16)
    .border({ width: 1, color: '#33FFFFFF' })
    .position({ x: '100%', y: 44 })
    .translate({ x: -324, y: 0 })
  }

  private format(s: NumberSpec): string {
    const text: string = this.values[s.id].toFixed(s.decimals);
    return s.unit.length > 0 ? (text + ' ' + s.unit) : text;
  }
}
