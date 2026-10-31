# NR Nexus

**DLSS Neural Rendering Add-on for RenoDX**

[简体中文](#简体中文) · [English](#english)

[**下载 / Download**](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases) · [**玩家指南 / User Guide**](docs/USER_GUIDE.md) · [**兼容性 / Compatibility**](#兼容性与已知限制) · [**问题反馈 / GitHub Issues**](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/issues)

## 简体中文

NR Nexus 通过 [ReShade](https://reshade.me/) 为兼容游戏提供 NVIDIA DLSS Neural Rendering
（DLSSNR）画面增强，基于 [RenoDX](https://github.com/clshortfuse/renodx) 开发。
将最多四层 NR 串成顺序处理链，为每层独立选择模型、调整外观和编辑结果，组合你想要的画面。
插件不执行降噪。

**[下载公开版本](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases)**：
最新预发布为 **v0.2.0-dev.8**，正式版为 **v0.1.0**。本仓库当前开发源码为 **0.2.0-dev.11**，尚未公开发布。

| 版本 | 功能范围 |
| --- | --- |
| **0.2.0-dev.11 开发源码，未发布** | 新增逐层独立工作分辨率、记住本游戏显示选择的 F11 监视器、统一帧率与逐层 GPU 诊断、手动 GPU A/B 对比，以及自适应布局和统一定位操作。 |
| **0.2.0-dev.8 公开预发布** | 已含逐层阶段选择、DX12／DX11 Bridge 的 Render → 游戏 DLSS SR/DLAA → Upscaled 链、方案库与每游戏六槽方案，以及辅助输入。 |
| **0.1.0 正式版** | 已含最多四层处理、双模型、逐层独立外观／结果编辑及四套主题；不含逐层阶段选择、方案库／六槽和辅助输入，也不含上述 dev.11 新增功能。 |

### 核心特色

标注“开发版可用”的功能已在 dev.11 实现，尚未随公开安装包发布；其他功能的版本范围见上表。

| 能力 | 玩家用途 |
| --- | --- |
| **最多四层顺序链，每层独立模型／外观／结果** | 将前一层的输出交给下一层；分别调整风格、局部色调、结构、角色遮罩与皮肤，再编辑亮度、色彩、细节、光晕和时序稳定。附加层可独立设置，也可继承第 1 层。 |
| **A/B 双模型** | 在兼容的 310.9.16.0 Runtime 上选择 A（Legacy）或 B（Small FP8），尝试 AA、AB、BA、BB 等不同组合。 |
| **独立工作分辨率（开发版可用）** | 按层分配 NR 的工作尺寸，在细节与 GPU 开销间取舍，不改变游戏 DLSS SR 的分辨率。支持范围和设置方法见[画质](docs/USER_GUIDE.md#zh-quality)与[叠层指南](docs/USER_GUIDE.md#zh-multipass)。 |
| **处理位置与 NR／FG 协同** | 选择 Render、Upscaled 或 Present，将 NR 放在超分前、超分后或最终游戏画面；支持与原生 DLSS FG/MFG 或 XeFG 协同。组合规则见[叠层指南](docs/USER_GUIDE.md#zh-multipass)。 |
| **每游戏六槽与共享方案库** | 三个官方起点加三个自定义方案，保存不同游戏与场景的偏好；通过可搜索方案库导入、导出和分享。见[方案指南](docs/USER_GUIDE.md#zh-presets)。 |
| **F11 常驻性能 HUD（开发版可用，尚未打包）** | 精简／详细视图查看 NR 状态、GPU 开销、帧率与 FG 状态，可选 10／30／60 秒趋势。支持背景透明度、九宫格定位及打开 Home 菜单后拖动记忆。 |
| **逐层 GPU 诊断与 A/B 对比（开发版可用）** | 查看每层模型、工作／输出尺寸及 GPU 耗时，比较调整一层工作尺寸前后的开销，帮助定位昂贵的层。见[诊断指南](docs/USER_GUIDE.md#zh-diagnostics)。 |
| **15 种界面语言与辅助输入** | 自动跟随 ReShade 或手动选语言；输入法遇到问题时，使用 Windows 原生 Unicode 编辑窗填写方案名称和反馈文字。 |
| **四套界面主题** | 在 Cyber Neon、Nexus Dark、Fluent Light、Midnight OLED 中选择喜欢的菜单外观。 |

增加层数会增加 GPU 工作量，建议从单层开始，在同一场景比较画面与开销。
参数、继承和实际组合示例见[玩家指南](docs/USER_GUIDE.md#zh-start)。

![NR 结果编辑器](docs/images/ui/results-zh.webp)

*早前开发界面示意；最新布局以文字说明为准。*

### 下载、安装与使用

**需要：** Windows x64、兼容 **ReShade API 18** 的[完整 Add-on 支持版 ReShade](https://reshade.me/)、
合法取得的兼容 NVIDIA **`nvngx_dlssnr.dll`**，以及该 Runtime 支持的显卡和驱动。
安装包不附带 ReShade、NVIDIA Runtime、模型或厂商 SDK；界面、翻译和着色器已编入插件。

1. 从 [Releases](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases) 下载
   `NR-Nexus-v<version>-<commit>-windows-x64.zip` 及 `.zip.sha256` 并核对校验值。
   GitHub 的 **Source code** 压缩包是源码。
2. 关闭游戏，备份已有插件和设置，将本插件旧副本移出生效的 Add-on 搜索目录，避免旧 Lecram 构建与 Nexus 同时加载。
3. 将 **`renodx-dlss5.addon64`** 放在游戏 EXE 旁或 ReShade 配置的 `AddonPath`，保留许可文件。
   将兼容的 `nvngx_dlssnr.dll` 放在插件旁；该位置没有时会再查游戏 EXE 目录。
4. 打开 ReShade，选择 **NR Nexus**，开启 **Neural Rendering**。从单层开始选择处理位置与模型，
   查看 NR 状态并在同一场景比较 Off／On。首次设置见[开始使用](docs/USER_GUIDE.md#zh-start)。

**升级：** 关闭游戏、备份，按新版说明替换插件。**卸载：** 关闭游戏，移除插件或恢复旧版。
保留无关的 DLSS／FG 文件、ReShade 预设、配置和存档。

### 兼容性与已知限制

兼容性取决于游戏、图形 API、NR Runtime、显卡与驱动。本项目尚无通用的最低显卡／驱动清单。

| 使用情况 | 选择时需要了解 |
| --- | --- |
| **DX12／DX11 Bridge** | 提供 Render／Upscaled 及兼容的 Present 路径；逐层阶段与工作分辨率的可选组合见[叠层指南](docs/USER_GUIDE.md#zh-multipass)。 |
| **Vulkan** | 使用 DLSS 输出处理；Present 会回退到该位置，不支持混合阶段链或独立工作分辨率。 |
| **DLSS Ray Reconstruction** | 选择 Render 时，NR 在 Upscaled 位置处理 RR 输出。 |
| **Present** | 处理的画面包含游戏 HUD；菜单和地图也可能受到 NR 影响。 |
| **A/B 模型** | B 需要精确识别的 **310.9.16.0** Runtime；不兼容运行时上的已保存 B 选择会回退 A，原选择保留。 |

- **XeFG 高亮瞬闪尚未解决。** 遇到该现象时可关闭 NR 对比，排查见[诊断指南](docs/USER_GUIDE.md#zh-diagnostics)。
- 独立工作分辨率仅支持部分 DX11／DX12 组合；同阶段三层以上仍需相同工作尺寸。设置前参阅[叠层指南](docs/USER_GUIDE.md#zh-multipass)。
- 语言显示依赖 ReShade 已加载的字体；缺字处理见[高级设置](docs/USER_GUIDE.md#zh-advanced)。

各版本变化见 [Release Notes](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases)。

### 问题反馈与隐私

[反馈问题](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/issues)时，请提供游戏／API、显卡／驱动、
插件与 Runtime 版本、处理位置、模型／层数、FG 配置、SDR/HDR 模式及复现步骤。

**我遇到了问题...** 可预览诊断并保存本地 ZIP，截图默认排除。文本／JSON 会自动脱敏；
分享前仍请检查附件中的私人路径、密钥和图像个人信息。诊断包为可选项，本地保存不会上传。

**Upload & copy link** 经单独同意后向第三方上传，持有链接者均可下载。
**准备 GitHub Issue** 打开浏览器草稿，由你检查并提交；附带摘要或报告链接分别征求同意。
详见[诊断与报告](docs/USER_GUIDE.md#zh-diagnostics)。

### 构建

开发者需准备 Windows x64、Git、PowerShell 7、Visual Studio 2022 C++ 工具
（**含 CMake**）、**MSVC 14.44** 和 **Windows SDK 10.0.26100.0**。
着色器工具链需要该 SDK 的 FXC、DXC 1.9.2602 或更新版本，以及 Slang 2026.14.1
或更新版本。下列初始化命令获取固定版本的 DXC／Slang 并复制 SDK 的 FXC；
下载地址及 SHA-256 固定于 [host/tools.lock.json](host/tools.lock.json)。

```powershell
git clone --branch dev https://github.com/williamstosh/RenoDX-DLSSNR-Nexus.git
cd RenoDX-DLSSNR-Nexus
pwsh ./tools/setup-build-tools.ps1
pwsh ./tools/setup-host.ps1
pwsh ./tools/build.ps1 -ToolRoot .local/shader-tools -Configuration Release
```

产物为 **`build/Release/renodx-dlss5.addon64`**。

开发时可用 `pwsh ./tools/preview-ui.ps1` 离屏查看生产菜单，Python 需 NumPy／Pillow
（可传 `-PythonExecutable <python.exe>`）。直接打开 `artifacts/ui-snapshots/latest/index.html`。
Quick／Full、指定场景及经检查的文档配图更新共用真实 ImGui 捕获，见[快照工作流](docs/UI_SNAPSHOTS.md)。

[host/lock.json](host/lock.json) 固定 RenoDX 宿主及依赖，
[host/renodx-compat.patch](host/renodx-compat.patch) 补齐所需宿主接口。
初始化获取这些公开源码，并将本仓库链接为 `src/addons/dlss5`；
任意上游检出不能直接替代兼容宿主。

初始化仅创建新目录，拒绝覆盖。已初始化的检出复用现有宿主／工具，只需重新构建；
也可用 `-ToolRoot <shader-tool-directory>` 指定现有着色器工具。
自定义宿主位置时，向初始化传 `-HostRoot <new-directory>`，向构建传
`-RenodxRoot <same-directory>`。厂商 SDK／Runtime 仍适用各自许可，
Runtime DLL、模型和游戏资源另行获取。

<a id="自动构建与发布"></a>

### 版本与发布

[version.h](version.h) 定义插件版本，Git Commit 标识源码。`dev.N` 和可选的 `rc.N` 为预发布版本；
公开下载见 [Releases](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases)，当前版本范围见本页开头。
[CI](.github/workflows/ci.yml) 构建源码，版本 Tag 触发[发布工作流](.github/workflows/release.yml)。
日常提交和 Actions artifacts 是开发构建。打包流程随插件附带 README、玩家指南、配图和许可文件。

### 来源、致谢与许可证

NR Nexus 基于 [RenoDX](https://github.com/clshortfuse/renodx)、
Lecram [V8.5 RC10 公开插件源码](https://github.com/Yukikaze20170315/RenoDX-DLSS5-Generic/tree/v8.5dev)
及其贡献者的工作开发。
[导入源码提交](https://github.com/williamstosh/RenoDX-DLSS5-Generic/commit/9bb6c0fec57ecb28c77938f3feb8155433396d93)
及[历史 fork](https://github.com/williamstosh/RenoDX-DLSS5-Generic)保留为来源依据。

感谢 [GITC-Uplift](https://github.com/ghostinthecamera/GITC-Uplift) 作者
**Hussain Abdullah**，其公开的 DLSS NR 模型选择研究为 NR Nexus 的双模型实现提供了技术参考。
详细来源与参考范围见[第三方声明](THIRD_PARTY_NOTICES.md#gitc-uplift-research-and-local-validation-references)。

本项目是非官方独立项目，未获 NVIDIA、RenoDX 作者或 Lecram 背书。
原始 [MIT 许可](LICENSE)、版权及[第三方声明](THIRD_PARTY_NOTICES.md)保留原文；
MIT 许可不改变第三方 SDK、Runtime 或模型的许可条件。

文档组织还参考了 [OptiScaler](https://github.com/optiscaler/OptiScaler)、
[Magpie Experimental](https://github.com/SAOG0721/Magpie) 和
[Veyra](https://github.com/Likely7/Veyra-NRVideo)。

## English

NR Nexus brings NVIDIA DLSS Neural Rendering (DLSSNR) image enhancement to compatible games through
[ReShade](https://reshade.me/), built on [RenoDX](https://github.com/clshortfuse/renodx).
Build a sequential chain of up to four NR passes, with an independent model, look and result editor for each layer.
The add-on performs no denoising.

**[Download a published version](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases)**:
the latest pre-release is **v0.2.0-dev.8**; the stable release is **v0.1.0**.
The current development source is **0.2.0-dev.11**, which has not been publicly released.

| Version | Available features |
| --- | --- |
| **0.2.0-dev.11 development source, unreleased** | Adds independent per-pass working resolution, an F11 monitor that remembers this game's display choice, shared FPS and per-pass GPU diagnostics, manual GPU A/B comparison, adaptive layouts and consistent placement controls. |
| **0.2.0-dev.8 public pre-release** | Includes per-pass stages, the DX12/DX11 Bridge Render → game DLSS SR/DLAA → Upscaled chain, the profile library and six slots per game, and assisted input. |
| **0.1.0 stable** | Includes up to four passes, dual models, independent per-pass look/result editing and four themes. Per-pass stages, the profile library/six slots, assisted input and the dev.11 additions above are absent. |

### What makes Nexus different

Features marked “available in development” are implemented in dev.11 and await a public package. See the version table for other features.

| Feature | What you can use it for |
| --- | --- |
| **Up to four sequential passes, each with its own model, look and results** | Feed each layer's output into the next. Adjust style, local tone, structure, character masks and skin, then shape brightness, colour, detail, halos and temporal stability. Additional layers can use independent settings or inherit Pass 1. |
| **Dual models A/B** | Choose A (Legacy) or B (Small FP8) with the compatible 310.9.16.0 runtime; try combinations such as AA, AB, BA and BB. |
| **Independent working resolution (available in development)** | Allocate NR working sizes per layer to balance detail and GPU cost, while keeping the game's DLSS SR resolution unchanged. See [Quality](docs/USER_GUIDE.md#en-quality) and [Multi-Pass](docs/USER_GUIDE.md#en-multipass) for supported combinations and controls. |
| **Processing location and NR/FG coordination** | Choose Render, Upscaled or Present to process before upscaling, after upscaling or on the final game image. Coordinate NR with native DLSS FG/MFG or XeFG; see [Multi-Pass](docs/USER_GUIDE.md#en-multipass) for combination rules. |
| **Six slots per game and a shared profile library** | Start with three official profiles and keep three custom ones for different games and scenes. Search, import, export and share profiles through the library. See [Profiles](docs/USER_GUIDE.md#en-presets). |
| **F11 persistent performance HUD (development source, not yet packaged)** | Compact/Details views show NR state, GPU cost, FPS and FG status, with optional 10/30/60-second trends. Adjust background opacity, choose a nine-position anchor or open the Home menu to drag it; placement is remembered. |
| **Per-pass GPU diagnostics and A/B comparison (available in development)** | Inspect each layer's model, working/output dimensions and GPU time. Compare costs before and after changing one layer's working size to identify expensive passes. See [Diagnostics](docs/USER_GUIDE.md#en-diagnostics). |
| **15 interface languages and assisted input** | Follow ReShade's language automatically or choose one manually. If inline IME input fails, use the native Windows Unicode editor for profile names and feedback text. |
| **Four interface themes** | Choose Cyber Neon, Nexus Dark, Fluent Light or Midnight OLED for the menu's appearance. |

More passes add GPU work. Start with one pass and compare appearance and cost in the same scene.
See the [User Guide](docs/USER_GUIDE.md#en-start) for settings, inheritance and practical examples.

![NR result editor](docs/images/ui/results.webp)

*Earlier development interface example; follow the text for the latest layout.*

### Download, install and use

**Requirements:** Windows x64; [ReShade with full add-on support](https://reshade.me/), compatible with
**ReShade API 18**; a legally obtained compatible NVIDIA **`nvngx_dlssnr.dll`**;
and a GPU and driver supported by that runtime. The package excludes ReShade, NVIDIA runtimes, models and
vendor SDK distributions. The interface, translations and shaders are embedded in the add-on.

1. Download `NR-Nexus-v<version>-<commit>-windows-x64.zip` and its `.zip.sha256` from
   [Releases](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases), and verify the checksum.
   GitHub's **Source code** archives contain the source.
2. Close the game, back up the existing add-on and settings, and move older copies out of active add-on search
   folders to avoid loading an old Lecram build alongside Nexus.
3. Put **`renodx-dlss5.addon64`** beside the game EXE or in ReShade's configured `AddonPath`, keeping the licenses.
   Put the compatible `nvngx_dlssnr.dll` beside the add-on; if absent there, the game EXE directory is checked next.
4. Open ReShade, select **NR Nexus** and enable **Neural Rendering**. Start with one pass, choose its location
   and model, check NR status and compare Off/On in the same scene. See [Getting oriented](docs/USER_GUIDE.md#en-start).

**Upgrade:** close the game, back up and replace the add-on according to the new release's notes.
**Uninstall:** close the game and remove the add-on or restore the previous version.
Keep unrelated DLSS/FG files, ReShade presets, configuration and saves.

### Compatibility and known limits

Compatibility depends on the game, graphics API, NR runtime, GPU and driver.
There is no universal minimum GPU/driver list for this project.

| Setup | What to know when choosing |
| --- | --- |
| **DX12/DX11 Bridge** | Offers Render/Upscaled and compatible Present paths. See [Multi-Pass](docs/USER_GUIDE.md#en-multipass) for per-pass stage and working-resolution combinations. |
| **Vulkan** | Processes DLSS output; Present falls back to this location. Mixed-stage chains and independent working resolution are unavailable. |
| **DLSS Ray Reconstruction** | When Render is selected, NR processes the RR output at Upscaled. |
| **Present** | Includes the game's HUD; NR can also affect menus and maps. |
| **Models A/B** | B requires the specifically recognized **310.9.16.0** runtime. An incompatible saved B choice falls back to A while retaining the saved choice. |

- **XeFG highlight flicker remains unresolved.** Compare with NR off if it occurs; see [Diagnostics](docs/USER_GUIDE.md#en-diagnostics).
- Independent working resolution supports selected DX11/DX12 combinations. Three or more passes in one stage still
  require matching working sizes; consult [Multi-Pass](docs/USER_GUIDE.md#en-multipass) before configuring them.
- Language display depends on ReShade's loaded font; see [Advanced](docs/USER_GUIDE.md#en-advanced) for missing glyphs.

See [Release Notes](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases) for version-specific changes.

### Feedback and privacy

[Report a problem](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/issues) with the game/API, GPU/driver,
add-on and runtime versions, processing location, models/pass count, FG setup, SDR/HDR mode and reproduction steps.

**I'm having issues...** previews diagnostics and saves a local ZIP, excluding screenshots by default.
Text/JSON are automatically redacted; review attachments for private paths, secrets and personal image content
before sharing. Diagnostic packages are optional, and local saving does not upload them.

**Upload & copy link** uploads to a third party after separate consent; anyone with the link can download it.
**Prepare GitHub Issue** opens a browser draft for you to review and submit. Including a summary or report link
requires separate consent. See [Diagnostics and reports](docs/USER_GUIDE.md#en-diagnostics).

### Building

For developers: use Windows x64, Git, PowerShell 7, Visual Studio 2022's C++
tools **including CMake**, **MSVC 14.44**, and **Windows SDK 10.0.26100.0**.
The shader toolchain needs FXC from that SDK, DXC 1.9.2602 or later and Slang
2026.14.1 or later. The setup command below obtains the pinned DXC/Slang builds
and copies the SDK's FXC; downloads and SHA-256 values are in
[host/tools.lock.json](host/tools.lock.json).

```powershell
git clone --branch dev https://github.com/williamstosh/RenoDX-DLSSNR-Nexus.git
cd RenoDX-DLSSNR-Nexus
pwsh ./tools/setup-build-tools.ps1
pwsh ./tools/setup-host.ps1
pwsh ./tools/build.ps1 -ToolRoot .local/shader-tools -Configuration Release
```

The output is **`build/Release/renodx-dlss5.addon64`**.

For an offscreen view of the production menu, run `pwsh ./tools/preview-ui.ps1`
with a Python containing NumPy/Pillow (`-PythonExecutable <python.exe>`).
Open `artifacts/ui-snapshots/latest/index.html` locally. Quick/Full previews,
scene selection and checked documentation-image updates share the same ImGui
capture source; see the [snapshot workflow](docs/UI_SNAPSHOTS.md).

[host/lock.json](host/lock.json) pins RenoDX and its dependencies;
[host/renodx-compat.patch](host/renodx-compat.patch) supplies the required host
interfaces. Setup fetches these public sources and links this checkout as
`src/addons/dlss5`; an arbitrary upstream checkout is not a substitute.

Setup creates new directories and refuses overwrites. On an initialized
checkout, reuse the prepared host/tools and rerun the build. Existing shader
tools can be supplied with `-ToolRoot <shader-tool-directory>`. For a different
host location, use setup's `-HostRoot <new-directory>` and build's
`-RenodxRoot <same-directory>`. Vendor SDK/runtime terms still apply; runtime
DLLs, models and game assets are obtained separately.

<a id="automated-builds-and-releases"></a>

### Versions and releases

[version.h](version.h) defines the add-on version; Git commits identify the source.
`dev.N` and optional `rc.N` identify pre-releases. Published downloads are on
[Releases](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases); the opening table lists current versions.
[CI](.github/workflows/ci.yml) builds the source, and version tags trigger the
[release workflow](.github/workflows/release.yml). Daily commits and Actions artifacts are development builds.
The packaging workflow includes the README, User Guide, images and licenses alongside the add-on.

### Origins, thanks and license

NR Nexus builds on [RenoDX](https://github.com/clshortfuse/renodx), Lecram's
[V8.5 RC10 public add-on source](https://github.com/Yukikaze20170315/RenoDX-DLSS5-Generic/tree/v8.5dev)
and their contributors. The
[imported source commit](https://github.com/williamstosh/RenoDX-DLSS5-Generic/commit/9bb6c0fec57ecb28c77938f3feb8155433396d93)
and [historical fork](https://github.com/williamstosh/RenoDX-DLSS5-Generic)
remain available for provenance.

Thanks to **Hussain Abdullah**, author of
[GITC-Uplift](https://github.com/ghostinthecamera/GITC-Uplift), whose published
DLSS NR model-selection research informed NR Nexus's dual-model implementation.
See the [reference scope and provenance](THIRD_PARTY_NOTICES.md#gitc-uplift-research-and-local-validation-references).

This is an unofficial independent project, without endorsement from NVIDIA,
RenoDX's authors or Lecram. The original [MIT license](LICENSE), copyright and
[third-party notices](THIRD_PARTY_NOTICES.md) are preserved. The MIT license
does not relicense third-party SDKs, runtimes or models.

Documentation organization also draws on [OptiScaler](https://github.com/optiscaler/OptiScaler),
[Magpie Experimental](https://github.com/SAOG0721/Magpie) and
[Veyra](https://github.com/Likely7/Veyra-NRVideo).

---
