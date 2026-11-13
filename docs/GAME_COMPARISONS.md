# 游戏实机画面对比 / In-game Comparisons

[简体中文](#简体中文) · [English](#english) · [README](../README.md)

## 简体中文

此工具把用户明确标注的同场景 NR OFF／ON 截图制作成静态预览与离线拖动对比，独立于 [UI 快照](UI_SNAPSHOTS.md)，无需安装游戏、插件或 NVIDIA Runtime。公开图库已上线，仅收录经人工审核且获公开授权的场景。

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
| `readme.webp` | 完整 ON 的 SDR 缩略图，无分割／水印，最多 1600 像素宽，WebP 质量 90 |
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

HDR 默认公开三张图：off.jpg、on.jpg 和完整 ON 的 readme.webp（SDR，无分割／水印，最多 1600 像素宽，WebP 质量 90）。preview.webp 只作本地静态分割预览；下表的 SDR fallback 和 HDR split 需显式选择生成，也不默认公开。

| 文件 | 用途 |
| --- | --- |
| `off.jpg`、`on.jpg` | 全分辨率 Ultra HDR JPEG，内置 SDR 底图，默认交互源 |
| `readme.webp` | 完整 ON 的 SDR 缩略图，最多 1600 像素宽、质量 90，供 README 使用 |
| `readme.jpg` | 仅 `--readme-hdr` 替代 WebP：完整 ON、全分辨率 Ultra HDR，无分割／标签／水印 |
| `off-sdr.webp`、`on-sdr.webp` | 仅 --sdr-fallback 生成的本地无损 SDR 备用图 |
| `preview-hdr.jpg` | 仅 --hdr-preview 生成的本地 HDR 拼接图 |
| `preview.webp` | 本地带标签的 SDR 分割预览 |

Ultra HDR JPEG 内置 SDR 底图，不支持 HDR 的环境自动回退，无需独立 SDR 文件。页面对保留 sdr_off／sdr_on 的旧清单和显式 fallback 候选继续提供 HDR／SDR 切换；新三图记录直接显示 JPEG。普通 JPEG 解码和强制 SDR 浏览器测试覆盖底图一致性。实际 HDR 需要浏览器、操作系统 HDR 设置和显示器共同支持，尚未进行物理 HDR 验收；dynamic-range: high 仅作环境提示。README 默认使用完整 ON 的 SDR 缩略图，也可显式选择下述 Ultra HDR JPEG 试验。

`generate --hdr --readme-hdr` 从同一原始 JXR 的 ON HDR 数据及 SDR 底图直接编码 `readme.jpg`，不经过 ON JPEG 再编码。保留完整图像和增益图分辨率，无叠加标记；底图与增益图默认质量均为 90，增益图倍率 1、gamma 1、三通道，目标峰值沿用 OFF／ON 共享值。`--readme-quality 1..100` 同时调整 README 底图与增益图质量（默认 90，也适用于普通 WebP），不改变对照全图参数。未选择 `--readme-hdr` 时仍输出原 SDR WebP；此标志在 generate 中要求 `--hdr`。报告记录来源哈希、质量／倍率／完整分辨率／目标峰值、增益图检查和 HDR 浮点回解误差，不复制源 EXIF。README 的实际 HDR 呈现仍待物理显示器审核。

两图共享以下处理：scRGB 80 nit 参考白换算、Rec.709→Rec.2020 线性色域转换、Ultra HDR 203 nit 参考白、由两图最大通道共同确定的目标峰值、固定曝光 1／白点 11.2 的 Hable SDR 曲线及编码质量。不会分别按各图峰值调曝光或归一化。扩展 scRGB 负通道通过色域转换尽量保留；Rec.2020 中绝对值不超过 1e-5 的负数值残差归零并记录，超出该范围则拒绝，不擅自裁掉色域。SDR 底图的色域／亮度压缩不冒充 HDR 原图。

Ultra HDR 是有损编码。新候选默认 --hdr-profile balanced：底图质量 95、增益图质量 95、缩小倍率 2；--hdr-profile archival 保留原质量 95／100、倍率 1。可用 --quality 1..100、--gainmap-quality 1..100、--gainmap-scale 1..128、--gainmap-channels 1 或 3、--gainmap-gamma 正数覆盖。实际编码值写入报告；倍率只缩小增益图，不改变 JPEG 全图尺寸。报告逐文件列字节、本地候选总量、预期公开体积／增量建议、源 JXR 哈希、增益图尺寸及浮点重建误差。编码器不复制源 EXIF／GPS／Game DVR 记录；原始浮点数据仍以 JXR 为准。现有已批准 JPEG 不因默认值改变而重编码。

可复现多档命令：对同一组 generate --hdr 输入，分别追加 --hdr-profile archival、--hdr-profile balanced、--quality 95 --gainmap-quality 95 --gainmap-scale 4，或 --quality 92 --gainmap-quality 95 --gainmap-scale 2。每档保留独立候选，人工审核高光与细节后再决定是否替换公开全图。

转换参考 [jxr2uhdr](https://github.com/tfx2001/jxr2uhdr)，许可与适配范围见 [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md)。HDR 编解码由可选 imagecodecs／jxrlib／libultrahdr 提供，不附入插件安装包。`--note "简短采集说明"` 可记录时间差等需审核的信息，不能写私人路径。

### 今后交给 Codex

提供可访问的两张文件及明确 OFF／ON 身份，并说“加入对比图库”。Codex 会调用 `generate`、检查报告和实际预览，返回候选路径与警告，等待审核。附件不可读时需提供实际文件路径；身份不明时先确认。此指令仅生成候选，不授权公开复制、Git 提交或部署。

### 审核与公开复制

人工审核后，先查看准确文件计划：

```powershell
python tools/game-compare.py approve --candidate "<候选目录>"
```

默认只列新增展示图（新普通候选四张、新 HDR 候选三张）、清单／页面更新及字节变化，无删除。普通候选也生成完整 ON 的 readme.webp。获得审核通过及公开复制授权后，加 --apply --reviewed --publish-authorized；构图警告另需 --ack-warnings。只复制本地公开目录和更新清单／页面，不执行 Git 或部署。旧候选按原报告处理，旧场景保留；合成测试永远不能批准公开。

### 精简已批准素材

```powershell
python tools/game-compare.py optimize --scene SCENE_ID --preserve-hdr-pair `
  --game-en GAME_EN --scene-en SCENE_EN --delete-old-assets
```

运行 python tools/game-compare.py optimize --scene <已有scene-id> --preserve-hdr-pair --game-en <英文游戏名> --scene-en <英文场景名> --delete-old-assets。默认仅生成本地 readme.webp、单份 report.json 和 add／update／delete 字节计划。原 OFF／ON JPEG 路径和字节复用，从 ON JPEG 内嵌 SDR base 生成缩略图，置于该 scene 的 revisions/<内容标识>/ 子目录；scene ID、审核状态和原输入身份保持不变。可选 game_en／scene_en 缺省时回退原名称，网页和 README 均支持。

可选 `optimize --scene SCENE_ID --readme-hdr --readme-quality 90` 改为生成全分辨率 `readme.jpg`，编码参数与上方 README HDR 相同。从已批准 ON JPEG 浮点解码 HDR，并读取其内嵌 SDR 底图，不再次色调映射；目标峰值为原增益图元数据的线性 `hdr_capacity_max × 203 nit`，不是 log2。此路径有一次额外有损重编码，报告明确记录来源 JPEG 哈希及代际损失；原 OFF／ON 字节不变。若原 JXR 可用，优先从原图 generate，一次编码后审核 README 图片。新旧 README 格式共用现有计划、审核、授权、备份及删除流程；保留旧 WebP 可随时回退。

先 dry-run 并审核 README 图片及计划，由 README 维护者更新旧图片引用，再在同次命令加 --apply --reviewed --publish-authorized 执行。--delete-old-assets 仅选择本场景不再被清单引用的旧派生图；省略则保留旧文件。README 仍引用旧图时阻止删除。删除前逐文件备份至本地候选 backup/ 并校验哈希，核对所有新引用后精确删除。其他场景及原候选／报告保留；不复制或重编码原 OFF／ON JPEG，不重写 Git 历史。

确需替换全图编码时，先从原输入生成新候选，再以 approve --candidate <新候选> --replace-scene <已有scene-id> --original-candidate <原候选> 查看 revision 计划。工具核对原输入哈希及当前公开图片；apply 仍需审核和公开授权，旧文件默认保留。

公开页面只显示 `approved` 场景。`catalog.json` 为权威清单，命令将其嵌入同一个 HTML 模板以支持 `file://`；新增素材无需手改 HTML。不要手动向公开清单加入候选。公开目录只复制展示图片，不复制报告、源文件名、源路径或原图。画面自身可能含玩家名、聊天或私人信息，仍须审核。

### Pages 与 README

[在线对比图库](https://williamstosh.github.io/DLSS5-NR-Nexus/compare/)已上线，使用 **Deploy from a branch → main → /docs** 发布。将后续**审核通过且获准公开的素材**按项目流程审查并合入 `main` 后，Pages 自动更新。目录已有 `index.html` 入口与 `.nojekyll`，资源使用相对路径，无需新增 Actions。分支发布会公开 `/docs` 内的已跟踪文件；内部资料保持忽略，候选留在该目录之外。

在 Pages 实际上线并包含对应场景后，可生成最多三组 README 候选：

```powershell
python tools/game-compare.py readme --scene "<清单中的场景ID>" `
  --pages-url "https://williamstosh.github.io/DLSS5-NR-Nexus/compare/"
```

命令先核验线上页面和场景，再生成本地 readme-candidate.md。审核后加 --apply --publish-authorized 才修改标记区。中英共用清单指定的 readme.webp 或 readme.jpg，缺少完整 ON 图片时拒绝并提示先运行 optimize，不回退半幅 preview。仅图片链接到对应 #scene=，不另附重复文字链接。英文显示名缺省时回退原名称，其他 README 内容保留。Git／main 合并／部署分别授权。

### 验证

```powershell
python -B -m unittest discover -s tests -p test_game_compare.py -v
python -B -m unittest discover -s tests -p test_game_compare_hdr.py -v
node tests/test_game_compare_browser.cjs
```

图像测试仅需 Pillow；浏览器测试额外需要本地 Playwright 和 Chromium。可用 `npm install --prefix .local/game-compare-browser --no-audit --no-fund playwright` 安装测试库，并用环境变量 `PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH` 指向已有 Chromium／Chrome 可执行文件，或使用 Playwright 自带浏览器。它们只用于测试，不是网页依赖。所有素材为明确标记的中性合成图，不是 NR 画质示例。页面支持鼠标／触屏拖动、原生范围滑杆的方向键／Home／End、完全 OFF／ON、双击／重置、大图、全图查看、多场景与中英文切换。触屏模拟不等于手机硬件验收；最终实际呈现由所有者审核。

## English

This tool turns explicitly identified, same-scene NR OFF/ON screenshots into static previews and an offline interactive viewer. It is separate from the [UI snapshot workflow](UI_SNAPSHOTS.md). No game, add-on or NVIDIA runtime is needed. The public gallery is live and contains only reviewed scenes authorized for publication.

Use Python 3.10+ and Pillow (tested with Python 3.12/Pillow 13). Run `python tools/game-compare.py generate --off "OFF.png" --on "ON.png" --game "Game" --scene "Scene"`. Quote paths containing spaces or Chinese characters. The role flags declare identity; conflicting ON/OFF filename tokens are rejected. Pixels cannot establish which image has NR enabled. Game and scene names are required rather than guessed.

Candidates are stored under `artifacts/game-comparisons/candidates/<game>/<scene>/<content-id>/`, with `off.webp`, `on.webp`, `preview.webp`, `report.json` and a directly openable `index.html`. A local `candidates.json` indexes them. Identical inputs/options reuse the candidate; different inputs/options/templates produce a separate revision. Changed candidate files are rejected. Inputs and old candidates are never automatically removed. `--output` can select another local directory; repository output must remain under ignored `artifacts/`, `tmp/` or `.local/`.

The default keeps full resolution and exports **lossless WebP** interaction images, plus a quality-95 split preview. Lossless files can be large; reports include byte sizes. Use `--quality 95` for identical lossy encoding on both images, `--format png` for PNG, or `--max-width 2560` for common proportional downscaling. Different dimensions fail unless `--match-size` explicitly selects a smaller common size with exactly matching aspect ratios. There is no stretch, crop, registration, image enhancement or generated game content. EXIF orientation is honored and recorded.

Standard mode accepts opaque, single-frame SDR PNG/JPEG/WebP. Matching ICC profiles are converted consistently to sRGB; exports keep only standardized sRGB ICC, removing EXIF/GPS/XMP/source text. Untagged inputs are explicitly treated as SDR sRGB and require confirmation. High-bit PNG and recognizable HDR transfer/gain-map data are rejected in standard mode, never silently tone-mapped. Windows scRGB JXR uses the optional HDR mode below; other HDR sources need a reliable prior export.

### Preserve HDR: Windows JXR to Ultra HDR

Install optional codecs with `python -m pip install -r tools/game-compare-hdr-requirements.txt`, then run `python tools/game-compare.py generate --hdr --off "OFF.jxr" --on "ON.jxr" --game "Game" --scene "Scene"`.

`--hdr` explicitly declares Windows linear scRGB JXR (floating-point RGBA, 1.0 = 80 nits); it is not a detector for arbitrary HDR color spaces. Both images must have matching full dimensions, opaque alpha and normal orientation. No resize/crop is accepted in this mode, and original JXR/companion PNG files stay intact.

New HDR candidates publish only off.jpg/on.jpg (full-resolution Ultra HDR JPEG with embedded SDR bases) and readme.webp (complete ON SDR thumbnail, no split or watermark, maximum width 1600, WebP quality 90). The labeled SDR split preview stays local. --hdr-preview optionally generates a local HDR split; --sdr-fallback optionally generates local lossless SDR alternatives. Neither is copied publicly by default. HDR annotations are composed in linear light before encoding a new gain map.

Ultra HDR JPEG includes an SDR base that browsers without HDR support show automatically, without separate SDR downloads. Legacy catalogs with sdr_off/sdr_on and explicit local fallback candidates retain manual HDR/SDR switching; new three-image records use JPEG directly. Ordinary JPEG decoding and forced-SDR browser tests verify base-image consistency. Physical HDR presentation requires compatible browser, OS settings and display; physical HDR acceptance remains outstanding. The dynamic-range: high hint reports environment capability only. README defaults to the complete ON SDR thumbnail; the following optional Ultra HDR JPEG export is available for experiments.

`generate --hdr --readme-hdr` replaces the WebP with `readme.jpg`, encoded directly from the original ON JXR HDR data and SDR base, without an intermediate ON JPEG decode. It keeps the complete image and gain map at full resolution, without splits, labels or watermarks. Base and gain-map quality default to 90, gain-map scale to 1, gamma to 1, and channels to three; the target peak remains the shared OFF/ON value. `--readme-quality 1..100` changes both README qualities (default 90; also applies to SDR WebP), independently of comparison-image settings. Without `--readme-hdr`, SDR WebP remains the default; generate requires `--hdr` with this flag. Reports record the source hash, quality/scale/full resolution/target peak, gain-map inspection and HDR floating-point roundtrip error. Source EXIF is excluded. Physical HDR presentation in README remains unverified.

Both sides use the same scRGB 80-nit reference conversion, linear Rec.709-to-Rec.2020 matrix, Ultra HDR 203-nit reference, shared peak computed from the maximum channel across both sources, fixed Hable SDR exposure 1/white 11.2 and encoder settings. There is no independent exposure normalization. Extended scRGB colors are preserved through the wider gamut; tiny negative Rec.2020 residuals within 1e-5 are zeroed and counted, while larger out-of-gamut values fail. SDR tone/gamut compression is not presented as the original HDR.

Ultra HDR is lossy. New candidates default to --hdr-profile balanced: base quality 95, gain-map quality 95, scale 2. --hdr-profile archival retains base/gain-map quality 95/100 and scale 1. Override with --quality 1..100, --gainmap-quality 1..100, --gainmap-scale 1..128, --gainmap-channels 1 or 3, and positive --gainmap-gamma. Actual encoder options are recorded. Scale reduces the gain map, not JPEG base dimensions. Reports include per-file bytes, local total, expected public size/delta advisory, source hashes, gain-map dimensions and round-trip errors. Existing approved JPEGs are not re-encoded automatically. Source EXIF/GPS/Game DVR records are excluded; original JXR remains authoritative.

To reproduce profiles, repeat generate --hdr with the same inputs and append --hdr-profile archival, --hdr-profile balanced, --quality 95 --gainmap-quality 95 --gainmap-scale 4, or --quality 92 --gainmap-quality 95 --gainmap-scale 2. Each produces a separate candidate for human review of highlights and detail.

The conversion adapts [jxr2uhdr](https://github.com/tfx2001/jxr2uhdr); attribution and scope are in [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md). Optional imagecodecs/jxrlib/libultrahdr dependencies are installed locally and excluded from add-on packages. Use repeated `--note "short capture note"` options to record timing or review context without private paths. HDR tests use only synthetic gradients and are included in the verification commands above.

Coarse luma/edge checks flag large changes or possible camera offsets without correcting them. Such warnings require explicit acknowledgement before approval. Menu overlays, movement, timing and reversed roles cannot all be detected automatically. Every pair needs human review. Watermarks use one dark translucent/cyan template and local system fonts; no fonts or vendor logos are distributed. Override text with `--label-off`/`--label-on`, or choose a local licensed `--font`. Labels that cannot fit fail clearly.

Optional user-supplied fields are `--api`, `--gpu`, `--plugin-version`, `--runtime`, `--pass`, `--model`, and `--working-resolution`. Hardware, FPS, settings and versions are never invented. Optional --game-en and --scene-en provide English display names in the viewer and README; absent fields fall back to original names without changing scene IDs.

For future Codex tasks, provide two accessible files and explicit OFF/ON roles, then ask to add them to the comparison gallery. Codex generates and inspects a local candidate, returns preview paths/warnings and waits for review. Inaccessible attachments require real file paths; unclear roles require clarification. This request alone does not authorize publication.

After review, approve --candidate <directory> prints the public-copy plan and byte changes. New standard candidates publish four images, new HDR candidates three; old candidates retain their original report selections. After human review and public-copy authorization add --apply --reviewed --publish-authorized, plus --ack-warnings when required. Only local public assets, catalog.json and embedded HTML data are updated. No Git or deployment is performed. Old scenes remain; synthetic fixtures cannot be approved. Reports, filenames, paths and originals stay local. Review visible content for personal information.

### Optimize approved scenes while preserving HDR JPEGs

```powershell
python tools/game-compare.py optimize --scene SCENE_ID --preserve-hdr-pair `
  --game-en GAME_EN --scene-en SCENE_EN --delete-old-assets
```

Run python tools/game-compare.py optimize --scene <existing-id> --preserve-hdr-pair --game-en <English-game> --scene-en <English-scene> --delete-old-assets. Default behavior only prepares a local readme.webp, one report.json and an add/update/delete byte plan with retained JPEG hashes. Both JPEGs retain their exact paths and bytes without copying or re-encoding. The thumbnail comes from the primary SDR image of ON JPEG, including Pillow MPO containers, under the scene revisions/<content-id>/ directory. Scene ID, approved status and original input identity remain unchanged; English fields are optional.

Optional `optimize --scene SCENE_ID --readme-hdr --readme-quality 90` generates full-resolution `readme.jpg` with the same README HDR settings above. It uses the approved ON JPEG floating-point HDR decode and embedded SDR base without retone. Target peak is the original gain-map metadata's linear `hdr_capacity_max × 203 nits`, not log2. This adds one lossy encoding generation, explicitly recorded with the source JPEG hash; original OFF/ON bytes stay unchanged. When original JXR files are available, prefer generate from those inputs for a single encode, then review the README image. Both formats use the existing plan, review, authorization, backup and deletion flow; retaining old WebP files allows rollback.

Review the dry-run thumbnail and plan, have the README maintainer update old image links, then apply once with --apply --reviewed --publish-authorized. --delete-old-assets explicitly selects derivatives no longer referenced by any scene; omit it to retain files. Remaining README references block deletion. Selected old files are backed up under the local candidate backup/ directory and hash-checked before public changes. New references are verified before exact deletion. Other scenes, original candidates/reports and inputs remain. No Git operation, history rewriting or deployment is performed.

For a reviewed full-image re-encode, generate from the same original inputs, then plan approve --candidate <new-candidate> --replace-scene <existing-id> --original-candidate <original-candidate>. Input hashes and current public assets are checked; new images use a revision directory while the original deep-link ID stays stable. Applying requires review and publication authorization; old files remain by default.

`catalog.json` is authoritative; embedding it into the one HTML template supports offline `file://` use. Only approved entries appear publicly; no per-scene HTML editing is needed.

The [comparison gallery](https://williamstosh.github.io/DLSS5-NR-Nexus/compare/) is live, published through **Deploy from a branch → main → /docs**. After review and public-copy authorization, merge selected assets into `main` through the project workflow; Pages updates automatically. `docs/index.html` and `.nojekyll` provide the entry point with relative resource paths. No new Actions workflow is required. Branch publishing exposes tracked content under `/docs`; ignored internal material and candidates stay outside the published tree.

Once selected scenes are live, run readme --scene <approved-id> --pages-url <repository-Pages-compare-URL>. It verifies live IDs and prepares artifacts/game-comparisons/readme-candidate.md; add --apply --publish-authorized after review. Select up to three scenes. Both languages share the catalog-selected readme.webp or readme.jpg, with only the image linked and no redundant text link. Legacy records missing a complete ON image are rejected with instructions to run optimize; split previews are never used as fallback. English names fall back to original names. Other README content is preserved. Git operations, main merges and deployment require separate authorization.

Run the verification commands above. Image tests need Pillow; HDR tests and the browser gain-map fixture also need the optional HDR codecs. Browser tests use Playwright/Chromium and the existing local Python virtual environment, or GAME_COMPARE_PYTHON. Set PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH to an existing Chromium/Edge/Chrome executable. These are test dependencies, not viewer dependencies. Fixtures are synthetic; browser/touch emulation does not validate physical HDR or mobile hardware. The viewer supports pointer/touch dragging, keyboard arrows/Home/End, full OFF/ON, reset/double-click, large/fullscreen views, scene history and Chinese/English. The owner makes the final visual decision.
