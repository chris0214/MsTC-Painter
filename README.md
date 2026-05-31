# msTC Texture Studio

为 [msToonCoordinator](https://bowlroll.net/) MMD 后期效果设计的非破坏性贴图绘制工具。
A non-destructive texture painting tool for the **msToonCoordinator** MMD post-effect.

[English](#english) · [简体中文](#中文)

---

## 中文

### 这是什么

**msTC Texture Studio** 是一个 Substance Painter 风格的桌面应用，用来给 MMD 模型绘制 msToonCoordinator 需要的 4 种控制贴图：

- **ShadowRate** — 控制阴影分档（R=Base / G=Highlight / B=Shadow）
- **SubLightRate** — 控制副光与高光取消（R=SubLight1 / G=SubLight2 / B=Cancel）
- **EdgeRate** — 控制描边强度（R=Edge force）
- **FaceSDF** — 面部阴影方向贴图（灰度）

### 核心特性

- **PMX 模型加载** — 直接拖入 PMX 2.0/2.1 文件，自动读取所有材质 / 贴图
- **2D + 3D 双视口绘制** — 在 2D 画布上画，或者直接在 3D 模型表面涂抹
- **非破坏性图层系统** — 每个材质 × 每种贴图独立的图层栈，支持四种图层：
  - PaintLayer（笔刷写入）
  - FillLayer（纯色填充 + 可选选区）
  - ImageLayer（导入 PNG 作为引用）
  - ChannelLayer（从其他贴图复制单通道）
  - 拖拽重排 / 可见性 / 不透明度 / 重命名 / 合并下层
- **专业级笔刷引擎**
  - 圆形笔刷 + 硬度（软边）+ 不透明度 + 间距
  - Wash / Buildup 两种叠加模式
  - 加权平滑（手抖修正）
  - Mirror X（左右对称绘制）
  - Shift 直线 / 15° 角度锁定
  - 自动 spacing clamp（小笔刷不会变成点点）
- **油漆桶（Bucket Fill）** — Color / UV / Both 三种 scope，支持 UV 边界约束
- **Eyedropper 取色器**
- **Per-stack 撤销重做** — 每个 (材质, 贴图) 独立 200 步历史，256 MB 上限
- **Blender 风格 3D 视口**
  - MMB 旋转 / Shift+MMB 平移 / 滚轮缩放
  - Numpad 1/3/7 切前/侧/顶视图，5 切正交，0 重置
  - F 键 / MMB 双击 / "Set Pivot" 按钮锁定旋转中心
  - 视口内可见的橙色 pivot 标记
  - Shift 慢速精细模式
  - 灵敏度滑块（持久化）
- **3D 视图模式** — Lit / Mask Overlay / Mask Only / UV Checker
- **项目存盘** — `.mstcproj` JSON + 伴生目录的二进制图层数据
- **PNG 单张 / 批量导出** — 直接输出 msTC 可用的控制贴图

### 编译

#### 依赖

| 组件 | 版本 |
|------|------|
| MSVC | 19.40+ (VS 2022 / 2026) |
| Qt | 6.5+ MSVC x64 |
| CMake | 3.20+ |
| Ninja | 任意近期版本 |
| vcpkg | 任意近期版本 |

vcpkg 自动拉取的依赖（见 `vcpkg.json`）：
- fmt
- spdlog
- nlohmann-json
- stb

#### 步骤

1. 编辑 `build.bat`，把里面的 `VCVARSALL` / `QT_DIR` / `CMAKE` / `NINJA` 改成你机器上的路径：
   ```bat
   set "VCVARSALL=G:\VS Build\VC\Auxiliary\Build\vcvarsall.bat"
   set "QT_DIR=G:\Qt\6.10.1\msvc2022_64"
   set "CMAKE=G:\cmake\bin\cmake.exe"
   set "NINJA=G:\cmake\bin\ninja.exe"
   ```
2. 设置 `VCPKG_ROOT` 环境变量指向你的 vcpkg 安装路径（或在 `build.bat` 内修改）。
3. 运行：
   ```
   build.bat            # Debug 构建
   build.bat release    # Release 构建
   ```
4. 产物在 `build/Debug/msTCTextureStudio.exe`（或 `build/Release/`），Qt DLL 自动通过 windeployqt 部署。

#### 启动调试 console

普通双击启动是 GUI-only。需要看 spdlog 输出时从命令行启动：
```
msTCTextureStudio.exe --console
```

### 使用流程

1. **File → Import PMX Model** 加载 MMD 模型。弹出对话框选贴图分辨率（512/1024/2048/4096/8192，按 VRAM 容量挑）。
2. 左侧 Texture 下拉选当前编辑的材质。
3. 顶部 Tab 切 ShadowRate / SubLightRate / EdgeRate / FaceSDF 中的一种贴图。
4. **画**：
   - 2D 画布上：左键画，鼠标滚轮缩放，Space + 拖拽平移，Shift+左键画直线
   - 3D 视口上：左键直接在模型表面画
5. **图层**：右侧 Layers 面板加 Fill / Image / Channel 图层，调透明度，合并。
6. **撤销**：Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z（每个 stack 独立历史）。
7. **存档**：Ctrl+S 存 `.mstcproj` 项目文件。
8. **导出**：File → Export Active Texture（单张）或 Export All Textures（批量到目录）。

### 3D 视口操作

| 操作 | 按键 |
|------|------|
| 旋转 | MMB 拖 / RMB 拖 / Alt+LMB |
| 平移 | Shift+MMB / Alt+Shift+LMB |
| 缩放 | 滚轮 |
| 慢速精细 | 任何拖动时按住 Shift（×0.2） |
| Front / Back 视图 | 1 / Ctrl+1 |
| Right / Left 视图 | 3 / Ctrl+3 |
| Top / Bottom 视图 | 7 / Ctrl+7 |
| 透视 ↔ 正交切换 | 5 |
| 重置视角 | 0 |
| Focus 旋转中心到鼠标位置 | F |
| 在 3D 上画 | LMB |

### 项目结构

```
src/
├── MainWindow.{h,cpp}          # Qt 主窗口、菜单、布局
├── main.cpp                    # 入口、主题、CLI
├── Theme.h                     # 暗色主题 QSS
├── pmx/                        # PMX 2.0/2.1 解析
├── editor/
│   ├── BrushEngine.{h,cpp}     # 笔刷引擎
│   ├── BrushPanel.{h,cpp}      # 笔刷参数 UI
│   ├── CanvasWidget.{h,cpp}    # 2D 画布
│   ├── ChannelColors.h         # 通道语义颜色
│   ├── FloodFill.{h,cpp}       # 油漆桶
│   ├── InfoPanel.{h,cpp}       # 通道说明面板
│   ├── Layer.{h,cpp}           # 图层基类 + 4 种实现
│   ├── LayerPanel.{h,cpp}      # 图层 UI
│   ├── LayerStack.{h,cpp}      # 图层栈 + 合成
│   ├── PixelTarget.h           # 写入接口抽象
│   ├── TextureDocument.{h,cpp} # 像素缓存 + dirty tile
│   ├── TextureGroup.{h,cpp}    # 每个材质的图层组
│   ├── TextureExport.{h,cpp}   # PNG 导出
│   ├── UndoHistory.{h,cpp}     # 撤销重做
│   ├── UvCoverage.{h,cpp}      # UV 覆盖率位图
│   └── UvLines.{h,cpp}         # UV wireframe
├── project/                    # 项目存盘 / 加载
└── renderer/
    ├── Camera.{h,cpp}          # 轨道相机
    ├── DX11Widget.{h,cpp}      # DX11 + Qt
    ├── MaskTextureSet.{h,cpp}  # GPU mask 贴图组
    ├── MeshRaycast.{h,cpp}     # 鼠标拾取
    ├── Shaders.h               # 内联 HLSL
    └── StaticMesh.{h,cpp}      # 静态网格 + GPU 资源

tests/                          # 独立 .bat 编译跑的测试
translations/                   # Qt .ts/.qm 本地化
```

### 测试

每个测试套件独立 `.bat` 编译运行（不靠 ctest）：
```
run_brush_test.bat
run_floodfill_test.bat
run_raycast_test.bat
run_export_test.bat
run_project_test.bat
run_layers_test.bat
run_undo_test.bat
```

### 路线图

- [x] PMX 加载 + 静态网格 + 轨道相机
- [x] 2D 画布 + 笔刷 + 油漆桶 + Eyedropper
- [x] 3D mask 预览（4 种视图模式）
- [x] 3D 直接绘制 + 跨组自动切换
- [x] 项目存盘 + PNG 单/批导出
- [x] 完整图层系统（4 种图层）
- [x] 撤销重做（笔刷 + 油漆桶）
- [x] Blender 风格 3D 视口
- [x] 中文本地化
- [ ] 图层操作的撤销（add/delete/reorder/merge）
- [ ] 日文本地化（ja-JP）— 期待社区贡献
- [ ] L2 真实 msTC 渲染预览
- [ ] portable zip 打包

### 贡献

欢迎 PR。提之前请：
- 跑一下 `build.bat` 确保没破坏编译
- 跑相关的测试 `.bat`
- 代码风格跟现有文件保持一致（4 空格缩进，C++20，没有强制 clang-format）

### 协议

Apache License 2.0 — 见 [LICENSE](LICENSE) 文件。

### 致谢

- [msToonCoordinator](https://bowlroll.net/) — 本工具服务的 MMD 效果
- Qt 6 / DirectX 11 / nlohmann-json / spdlog / fmt / stb

---

## English

### What is this

**msTC Texture Studio** is a Substance-Painter-style desktop app for authoring the four control textures the **msToonCoordinator** MMD post-effect needs:

- **ShadowRate** — shadow tier control (R=Base / G=Highlight / B=Shadow)
- **SubLightRate** — sub-light + highlight cancel (R=SubLight1 / G=SubLight2 / B=Cancel)
- **EdgeRate** — edge force (R=Edge)
- **FaceSDF** — face shadow direction (grayscale)

### Highlights

- PMX 2.0/2.1 import with full per-material breakdown
- Dual 2D/3D paint workflow — paint on the canvas or directly on the 3D mesh
- Non-destructive layer system per (material × kind) — Paint / Fill / Image / Channel layers, drag-reorder, opacity, merge-down
- Pro brush engine — circular brush with hardness, opacity, spacing, wash/buildup, weighted smoothing, mirror-X, Shift-line, auto-spacing for tiny brushes
- Bucket fill with color / UV / both scopes
- Per-stack undo/redo (Ctrl+Z) with 200-step history
- Blender-style 3D viewport — MMB orbit, Shift+MMB pan, numpad views (1/3/7/5/0), F-focus pivot, sensitivity slider
- Pivot indicator with depth test and surface auto-snap on pan
- Project save (`.mstcproj`) + single/batch PNG export
- Full Chinese (zh-CN) localization out of the box

### Build

Requires MSVC 19.40+, Qt 6.5+ MSVC x64, CMake 3.20+, Ninja, vcpkg.

1. Edit `build.bat` and point `VCVARSALL` / `QT_DIR` / `CMAKE` / `NINJA` at your installs.
2. Set `VCPKG_ROOT` env var.
3. Run `build.bat` (Debug) or `build.bat release`.

Output goes to `build/Debug/msTCTextureStudio.exe`. Qt DLLs are deployed via `windeployqt`.

### Run

Double-click for GUI mode. Launch with `--console` (or `-c` / `--debug`) from cmd to see spdlog output.

### Quick start

1. File → Import PMX Model, pick a `.pmx`, choose texture resolution.
2. Pick a material from the Texture dropdown on the left.
3. Switch between ShadowRate / SubLightRate / EdgeRate / FaceSDF tabs.
4. Paint with LMB on the 2D canvas or directly on the 3D model.
5. Build up layers in the right-side Layers panel.
6. File → Export Active Texture / Export All Textures.

### 3D viewport controls

| Action | Binding |
|---|---|
| Orbit | MMB drag / RMB drag / Alt+LMB |
| Pan | Shift+MMB / Alt+Shift+LMB |
| Zoom | Mouse wheel |
| Slow modifier | Hold Shift while dragging (×0.2) |
| Front / Back | 1 / Ctrl+1 |
| Right / Left | 3 / Ctrl+3 |
| Top / Bottom | 7 / Ctrl+7 |
| Perspective ↔ Ortho | 5 |
| Reset view | 0 |
| Focus pivot under cursor | F |
| Paint in 3D | LMB |

### License

Apache License 2.0 — see [LICENSE](LICENSE).
