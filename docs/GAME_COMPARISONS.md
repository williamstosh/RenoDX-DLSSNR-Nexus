# 游戏实机画面对比 / In-game Comparisons

[简体中文](#简体中文) · [English](#english) · [README](../README.md)

## 简体中文

此开发工具把用户提供的同一场景 NR OFF／ON 截图制作成静态分割图与离线拖动预览。它独立于 ReShade 菜单的 [UI 快照工具](UI_SNAPSHOTS.md)，无需安装游戏、插件或 NVIDIA Runtime。公开图库初始为空，不包含合成游戏效果。

### 生成候选

需要 Python 3.10+ 与 Pillow（测试环境为 Python 3.12／Pillow 13）。使用已有 Python，或在本地隔离环境安装：

```powershell
python -m venv .local/game-compare-venv
./.local/game-compare-venv/Scripts/python.exe -m pip install Pillow
./.local/game-compare-venv/Scripts/python.exe tools/game-compare.py generate `
  --off "D:\Screenshots\OFF.png" --on "D:\Screenshots\ON.png" `
  --game "Cyberpunk 2077" --scene "Night City Street"
```

路径示例仅说明命令，不代表已有该游戏的审核素材。中文与空格路径可直接加引号。`--off`／`--on` 是明确的身份声明；文件名含相反的独立 ON／OFF 标记时拒绝处理。无状态标记的文件不能从画面推断开关；由提供者确认。游戏和场景名称必须提供，避免将截图编号猜成场景名。

结果位于 `artifacts/game-comparisons/candidates/<game>/<scene>/<内容标识>/`：

| 文件 | 用途 |
| --- | --- |
| `off.webp`、`on.webp` | 同尺寸、无水印的交互全图 |
| `preview.webp` | 原宽高比的左 OFF／右 ON 各半图，青色分界线、统一标签及 NR Nexus 标识 |
| `report.json` | 本地审核报告：原文件名／尺寸／哈希、处理参数、输出哈希／字节数及警告；不记录原图绝对路径 |
| `index.html` | 直接双击打开的交互候选，无需服务器 |

`artifacts/game-comparisons/candidates.json` 是本地候选索引。同输入与参数复用原候选；不同图片、参数、模板或工具样式生成独立修订，不覆盖旧候选和已审核素材。候选文件被修改后拒绝复用。默认输出均受仓库忽略规则保护。可用 `--output` 指定其他本地目录；仓库内仅允许 `artifacts/`、`tmp/`、`.local/`。工具不自动删除原图或候选。

### 真实性与色彩

默认保持完整尺寸，交互全图使用**无损 WebP**，静态预览为质量 95 的 WebP。图像复杂时无损文件可能较大；报告记录实际体积。可显式用 `--quality 95` 对两张交互全图采用相同有损参数，或 `--format png` 输出全部 PNG。`--max-width 2560` 对两张全图统一等比例缩小；保持全分辨率更适合精细比较。

分辨率不同默认报错；只有宽高比严格一致，才允许显式 `--match-size` 缩至较小的共同尺寸。不会拉伸、自动裁切、配准、调色、锐化、降噪或生成游戏画面。EXIF 方向在两侧按其声明解释并记录，除此之外没有几何变换。

普通模式支持 PNG／JPEG／WebP 的单帧、不透明 SDR 图。相同 ICC 的输入统一转为 sRGB，输出仅保留标准 sRGB ICC；剥离 EXIF／GPS／XMP／源文本。无 ICC 时明确记录按 SDR sRGB 解释，需人工确认。高位深 PNG、PQ／HLG／HDR 增益图及不支持的颜色模式不在普通模式中静默转换；Windows scRGB JXR 使用下方 HDR 模式，其他 HDR 来源需先可靠导出。

粗略亮度与边缘检查可提示大范围变化、镜头偏移；不会自动校正。明显差异会将候选标记为需额外复核，批准时需显式确认警告。它不能可靠识别所有菜单、运动、错时或 ON／OFF 传反；每组均需人工核对身份、构图、遮挡、光照与时间。

水印固定采用系统无衬线字体、深色半透明底、青色细线，不分发字体或厂商 Logo。可用 `--label-off`／`--label-on` 改标签，`--font` 选本地合法字体；样式不随场景变化。极小画面或过长标签无法容纳时明确报错。

仅记录用户提供的元数据：`--api`、`--gpu`、`--plugin-version`、`--runtime`、`--pass`、`--model`、`--working-resolution`。不推断显卡、帧率、设置或版本；缺省字段保持未知。

### 保留 HDR：Windows JXR → Ultra HDR

新增可选 HDR 模式，不影响只安装 Pillow 的普通模式。在同一 Python 环境安装依赖：

```powershell
python -m pip install -r tools/game-compare-hdr-requirements.txt
python tools/game-compare.py generate --hdr --off "OFF.jxr" --on "ON.jxr" `
  --game "游戏名称" --scene "场景名称"
```

`--hdr` 明确声明输入为 Windows 捕获的**线性 scRGB JXR**（浮点 RGBA，1.0 对应 80 nit），不是任意 HDR 格式检测器。两图必须同尺寸、不透明且方向正常；保留完整分辨率，不接受此模式的裁切或缩放。原始 JXR 和配套 PNG 均不覆盖。

HDR 模式生成六张展示图：

| 文件 | 用途 |
| --- | --- |
| `off.jpg`、`on.jpg` | 带完整尺寸、三通道 HDR 增益图的 Ultra HDR JPEG，默认交互源 |
| `off-sdr.webp`、`on-sdr.webp` | 固定曲线生成的无损 SDR 备用图，供页面切换 |
| `preview-hdr.jpg` | 左 OFF／右 ON 的 HDR 拼接图，线性光合成同模板水印后重新编码增益图 |
| `preview.webp` | 带相同标签的 SDR 拼接预览，供 README 使用 |

Ultra HDR JPEG 的 SDR 底图可供不支持 HDR 的环境查看；保留 HDR 数据不等于所有环境都会显示 HDR。页面默认选 HDR，并提供 HDR／SDR 切换及处理后全图入口。实际 HDR 需要浏览器、操作系统 HDR 设置和显示器共同支持。`dynamic-range: high` 只是环境提示，不作为实际画面 HDR 验收证明；离屏截图也不能证明屏幕亮度。GitHub README 选用稳定的 SDR 预览，HDR 内容在交互页面与全图中审核。

两图共享以下处理：scRGB 80 nit 参考白换算、Rec.709→Rec.2020 线性色域转换、Ultra HDR 203 nit 参考白、由两图最大通道共同确定的目标峰值、固定曝光 1／白点 11.2 的 Hable SDR 曲线及编码质量。不会分别按各图峰值调曝光或归一化。扩展 scRGB 负通道通过色域转换尽量保留；Rec.2020 中绝对值不超过 1e-5 的负数值残差归零并记录，超出该范围则拒绝，不擅自裁掉色域。SDR 底图的色域／亮度压缩不冒充 HDR 原图。

Ultra HDR 输出是有损编码，默认底图质量 95、全分辨率增益图质量 100，可用 `--quality` 同时控制两张底图。报告记录源 JXR 哈希、共同参数、增益图信息、浮点重建误差和输出字节数；编码器创建必要的增益图／色彩标记，不复制源 EXIF、GPS 或 Game DVR 私人记录。精细比较优先保留当前高质量输出；原始浮点数据仍以 JXR 为准。

转换参考 [jxr2uhdr](https://github.com/tfx2001/jxr2uhdr)，许可与适配范围见 [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md)。HDR 编解码由可选 imagecodecs／jxrlib／libultrahdr 提供，不附入插件安装包。`--note "简短采集说明"` 可记录时间差等需审核的信息，不能写私人路径。

### 今后交给 Codex

提供可访问的两张文件及明确 OFF／ON 身份，并说“加入对比图库”。Codex 会调用 `generate`、检查报告和实际预览，返回候选路径与警告，等待审核。附件不可读时需提供实际文件路径；身份不明时先确认。此指令仅生成候选，不授权公开复制、Git 提交或部署。

### 审核与公开复制

人工审核后，先查看准确文件计划：

```powershell
python tools/game-compare.py approve --candidate "<候选目录>"
```

默认仅列出新增展示图片（普通模式三张、HDR 模式六张）、更新清单和页面，无删除。获得**审核通过及公开复制授权**后，使用同一命令加 `--apply --reviewed --publish-authorized`；有构图警告时还需 `--ack-warnings`。这一步只在本地复制至 `docs/compare/assets/`，更新 `catalog.json` 及页面内嵌清单；不会执行 Git 或部署。旧场景保留，不同修订并存。合成测试 `--synthetic` 永远不能批准到公开图库。

公开页面只显示 `approved` 场景。`catalog.json` 为权威清单，命令将其嵌入同一个 HTML 模板以支持 `file://`；新增素材无需手改 HTML。不要手动向公开清单加入候选。公开目录只复制展示图片，不复制报告、源文件名、源路径或原图。画面自身可能含玩家名、聊天或私人信息，仍须审核。

### Pages 与 README

[在线对比图库](https://williamstosh.github.io/RenoDX-DLSSNR-Nexus/compare/)已上线，使用 **Deploy from a branch → main → /docs** 发布。将后续**审核通过且获准公开的素材**按项目流程审查并合入 `main` 后，Pages 自动更新。目录已有 `index.html` 入口与 `.nojekyll`，资源使用相对路径，无需新增 Actions。分支发布会公开 `/docs` 内的已跟踪文件；内部资料保持忽略，候选留在该目录之外。

在 Pages 实际上线并包含对应场景后，可生成最多三组 README 候选：

```powershell
python tools/game-compare.py readme --scene "<清单中的场景ID>" `
  --pages-url "https://williamstosh.github.io/RenoDX-DLSSNR-Nexus/compare/"
```

命令先核验线上页面和场景，再生成本地 `artifacts/game-comparisons/readme-candidate.md`；审核后加 `--apply --publish-authorized` 才修改 README 标记区。中文和英文共用同一预览图，图片与文字都链接到对应 `#scene=`，不复制冗余图片。不覆盖 README 其余内容。Git 提交／推送、main 合并和公开部署按各自授权执行。

### 验证

```powershell
python -B -m unittest discover -s tests -p test_game_compare.py -v
python -B -m unittest discover -s tests -p test_game_compare_hdr.py -v
node tests/test_game_compare_browser.cjs
```

图像测试仅需 Pillow；浏览器测试额外需要本地 Playwright 和 Chromium。可用 `npm install --prefix .local/game-compare-browser --no-audit --no-fund playwright` 安装测试库，并用环境变量 `PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH` 指向已有 Chromium／Chrome 可执行文件，或使用 Playwright 自带浏览器。它们只用于测试，不是网页依赖。所有素材为明确标记的中性合成图，不是 NR 画质示例。页面支持鼠标／触屏拖动、原生范围滑杆的方向键／Home／End、完全 OFF／ON、双击／重置、大图、全图查看、多场景与中英文切换。触屏模拟不等于手机硬件验收；最终实际呈现由所有者审核。

## English

This development tool turns explicitly identified, same-scene NR OFF/ON screenshots into a split preview and an offline interactive viewer. It is separate from the [ReShade UI snapshot workflow](UI_SNAPSHOTS.md). No game, add-on or NVIDIA runtime is needed. The public catalog starts empty.

Use Python 3.10+ and Pillow (tested with Python 3.12/Pillow 13). Run `python tools/game-compare.py generate --off "OFF.png" --on "ON.png" --game "Game" --scene "Scene"`. Quote paths containing spaces or Chinese characters. The role flags declare identity; conflicting ON/OFF filename tokens are rejected. Pixels cannot establish which image has NR enabled. Game and scene names are required rather than guessed.

Candidates are stored under `artifacts/game-comparisons/candidates/<game>/<scene>/<content-id>/`, with `off.webp`, `on.webp`, `preview.webp`, `report.json` and a directly openable `index.html`. A local `candidates.json` indexes them. Identical inputs/options reuse the candidate; different inputs/options/templates produce a separate revision. Changed candidate files are rejected. Inputs and old candidates are never automatically removed. `--output` can select another local directory; repository output must remain under ignored `artifacts/`, `tmp/` or `.local/`.

The default keeps full resolution and exports **lossless WebP** interaction images, plus a quality-95 split preview. Lossless files can be large; reports include byte sizes. Use `--quality 95` for identical lossy encoding on both images, `--format png` for PNG, or `--max-width 2560` for common proportional downscaling. Different dimensions fail unless `--match-size` explicitly selects a smaller common size with exactly matching aspect ratios. There is no stretch, crop, registration, image enhancement or generated game content. EXIF orientation is honored and recorded.

Standard mode accepts opaque, single-frame SDR PNG/JPEG/WebP. Matching ICC profiles are converted consistently to sRGB; exports keep only standardized sRGB ICC, removing EXIF/GPS/XMP/source text. Untagged inputs are explicitly treated as SDR sRGB and require confirmation. High-bit PNG and recognizable HDR transfer/gain-map data are rejected in standard mode, never silently tone-mapped. Windows scRGB JXR uses the optional HDR mode below; other HDR sources need a reliable prior export.

### Preserve HDR: Windows JXR to Ultra HDR

Install optional codecs with `python -m pip install -r tools/game-compare-hdr-requirements.txt`, then run `python tools/game-compare.py generate --hdr --off "OFF.jxr" --on "ON.jxr" --game "Game" --scene "Scene"`.

`--hdr` explicitly declares Windows linear scRGB JXR (floating-point RGBA, 1.0 = 80 nits); it is not a detector for arbitrary HDR color spaces. Both images must have matching full dimensions, opaque alpha and normal orientation. No resize/crop is accepted in this mode, and original JXR/companion PNG files stay intact.

The six display assets are `off.jpg`/`on.jpg` (Ultra HDR JPEG with full-resolution three-channel gain maps), `off-sdr.webp`/`on-sdr.webp` (lossless SDR alternatives), `preview-hdr.jpg` (HDR 50/50 split with annotations) and `preview.webp` (SDR split for README). HDR annotations are composed in linear light and the gain map is encoded again; existing gain maps are not pasted together.

The viewer defaults to HDR sources and offers HDR/SDR switching. Ultra HDR carries both an SDR base and HDR reconstruction data; actual HDR presentation needs compatible browser, OS HDR configuration and display. The environment hint `dynamic-range: high` does not verify physical HDR output, and neither do headless screenshots. README uses the stable SDR preview; review HDR through the viewer/full-image links.

Both sides use the same scRGB 80-nit reference conversion, linear Rec.709-to-Rec.2020 matrix, Ultra HDR 203-nit reference, shared peak computed from the maximum channel across both sources, fixed Hable SDR exposure 1/white 11.2 and encoder settings. There is no independent exposure normalization. Extended scRGB colors are preserved through the wider gamut; tiny negative Rec.2020 residuals within 1e-5 are zeroed and counted, while larger out-of-gamut values fail. SDR tone/gamut compression is not presented as the original HDR.

Ultra HDR is lossy: default base quality is 95, full-resolution gain-map quality is 100; `--quality` changes both base encodings consistently. Reports include JXR hashes, shared parameters, gain-map metadata, floating-point round-trip errors and file sizes. The encoder creates required color/gain-map metadata without copying source EXIF/GPS/Game DVR records. Original floating-point JXR remains authoritative.

The conversion adapts [jxr2uhdr](https://github.com/tfx2001/jxr2uhdr); attribution and scope are in [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md). Optional imagecodecs/jxrlib/libultrahdr dependencies are installed locally and excluded from add-on packages. Use repeated `--note "short capture note"` options to record timing or review context without private paths. HDR tests use only synthetic gradients and are included in the verification commands above.

Coarse luma/edge checks flag large changes or possible camera offsets without correcting them. Such warnings require explicit acknowledgement before approval. Menu overlays, movement, timing and reversed roles cannot all be detected automatically. Every pair needs human review. Watermarks use one dark translucent/cyan template and local system fonts; no fonts or vendor logos are distributed. Override text with `--label-off`/`--label-on`, or choose a local licensed `--font`. Labels that cannot fit fail clearly.

Optional user-supplied fields are `--api`, `--gpu`, `--plugin-version`, `--runtime`, `--pass`, `--model`, and `--working-resolution`. Hardware, FPS, settings and versions are never invented.

For future Codex tasks, provide two accessible files and explicit OFF/ON roles, then ask to add them to the comparison gallery. Codex generates and inspects a local candidate, returns preview paths/warnings and waits for review. Inaccessible attachments require real file paths; unclear roles require clarification. This request alone does not authorize publication.

After review, `python tools/game-compare.py approve --candidate "<directory>"` prints the exact public-copy plan without applying it. Only after review **and public-copy authorization**, add `--apply --reviewed --publish-authorized`, plus `--ack-warnings` when required. It copies display images (three in standard mode, six in HDR mode) into `docs/compare/assets/`, updates `catalog.json` and the embedded HTML catalog, and retains old scenes. It never runs Git or deploys. `--synthetic` fixtures cannot enter the public catalog. Reports, source filenames/paths and originals are excluded; review visible content for personal information.

`catalog.json` is authoritative; embedding it into the one HTML template supports offline `file://` use. Only approved entries appear publicly; no per-scene HTML editing is needed.

The [comparison gallery](https://williamstosh.github.io/RenoDX-DLSSNR-Nexus/compare/) is live, published through **Deploy from a branch → main → /docs**. After review and public-copy authorization, merge selected assets into `main` through the project workflow; Pages updates automatically. `docs/index.html` and `.nojekyll` provide the entry point with relative resource paths. No new Actions workflow is required. Branch publishing exposes tracked content under `/docs`; ignored internal material and candidates stay outside the published tree.

Once the selected scenes are live, run `python tools/game-compare.py readme --scene "<approved-id>" --pages-url "https://williamstosh.github.io/RenoDX-DLSSNR-Nexus/compare/"`. The command checks the live scene and prepares `artifacts/game-comparisons/readme-candidate.md`; add `--apply --publish-authorized` only after review. Select up to three scenes. Both language sections share the same preview images, linked to the scene viewer. Other README content stays intact. Commits, pushes, main merges and deployment follow their respective authorizations.

Run the two verification commands above. Image tests need only Pillow. Browser tests additionally need Playwright/Chromium: install the test library with `npm install --prefix .local/game-compare-browser --no-audit --no-fund playwright`, and set `PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH` to an existing Chromium/Chrome executable, or use a Playwright browser installation. These are test-only dependencies. Fixtures are clearly synthetic; touch emulation is not mobile hardware acceptance. The viewer supports pointer/touch dragging, keyboard arrows/Home/End, full OFF/ON, reset/double-click, large/full-image views, scene selection and Chinese/English. The owner makes the final visual decision.
