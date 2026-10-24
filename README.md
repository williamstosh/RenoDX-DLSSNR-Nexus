# NR Nexus

**DLSS Neural Rendering Add-on for RenoDX**

[English](#english) | [简体中文](#简体中文)

[**Download / 下载（Pre-release）**](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases) · [**GitHub Issues / 问题反馈**](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/issues)

## English

NR Nexus adds NVIDIA DLSS Neural Rendering (DLSSNR) image enhancement to
compatible games. Built with [RenoDX](https://github.com/clshortfuse/renodx),
it runs through [ReShade](https://reshade.me/)'s add-on system and provides an
in-game control panel. The RenoDX integration is built into the add-on; it
works with the game's existing DLSS pipeline and performs no denoising.

This is an independent, unofficial project developed from Lecram V8.5 RC10.
It extends that source with NR/FG coordination, Present color-restoration
policies, motion-sharing fallback and a privacy-aware issue-report workflow.
Availability depends on the game, rendering path and separately supplied NR
runtime; see the validation scope below.

**Currently available as a Pre-release.** Download from
[Releases](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases) and read
the selected version's notes before installing.

[Features](#features) · [Install and use](#download-install-and-use) · [Compatibility](#compatibility-and-known-limits) · [Feedback](#feedback-and-privacy) · [Developer build](#building)

### Features

| Capability | What it does |
| --- | --- |
| Neural rendering | Applies NR to game images, with controls for quality, strength, processing size and precision. |
| Per-pass NR models | With DLSS NR 310.9.16, selects A (Legacy / preset 1) or B (Small FP8 / preset 2) independently for up to four passes. |
| Three processing positions | `Render` processes input before DLSS upscaling; `Upscaled` processes the DLSS output; `Present` processes the final image after game post-processing/HUD and before ReShade effects. |
| NR with frame generation | Integrates native DLSS FG/MFG and XeFG routes, processing game frames before FG while retaining the chosen backend and rendering settings. |
| SDR/HDR restoration | Independent restoration policies for SDR and HDR in diagnostics. These default to Off; selecting an NR position does not enable them automatically. |
| D3D11 motion-sharing fallback | Converts certain motion-vector textures when they cannot be shared directly with the NR processing path. This is a resource fallback, not blanket D3D11 game support. |
| Problem reports | Text-only feedback, optional diagnostics and screenshots, local ZIP saving and browser-based GitHub Issue drafts. |

These features are implemented; their tested and experimental scope is
separated under [Compatibility](#compatibility-and-known-limits).

The NR model control keeps the existing `NRPreset` key: `0` selects B on
310.9.16.0, `1` selects A and `2` selects B. `NRPass2Preset` through
`NRPass4Preset` use `0` to inherit the main model, `1` for A and `2` for B,
independently of appearance sharing. Existing explicit choices are retained.
Other or unidentified runtimes disable B and resolve a saved B request to A;
their automatic selection keeps the runtime default. Legacy preset 3 remains
stored but resolves to A on 310.9.16. It is hidden from the model menu;
an existing selection of 3 is shown as Custom without rewriting it.
A model change recreates the chain and
resets its history; old handles remain protected until GPU retirement and
working image resources are reused.
Diagnostics distinguish the preset requested at creation from a runtime-confirmed
network. A successful API call alone does not confirm the actual network or image quality.
These model controls are available starting with v0.1.0-dev.3; dev.2 predates them.

### Download, install and use

#### Requirements

- Windows x64 and a compatible game/DLSS rendering chain. D3D12 is the
  game-tested path listed below; Vulkan and D3D11 have additional limits.
- [ReShade with full add-on support](https://reshade.me/), compatible with
  **ReShade API 18**. ReShade loads the `.addon64` file.
- A legally obtained, compatible NVIDIA **`nvngx_dlssnr.dll`**, and a GPU/driver
  combination supported by that runtime and rendering path. There is no
  project-wide validated minimum GPU/driver list; follow the runtime's own
  requirements and check the in-game diagnostics.

The package does **not** include ReShade, NVIDIA runtime DLLs, models or vendor
SDK distributions. Shaders, UI and translations are embedded in the add-on.

#### Installation and first use

1. Open [Releases](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases).
   Download the `NR-Nexus-v<version>-<commit>-windows-x64.zip` **release asset**
   and its `.zip.sha256` checksum. GitHub's **Source code** archives are for
   development, not installation. Compare the ZIP's SHA-256 with the sidecar.
2. Close the game. Back up the installed add-on and settings. Move older copies
   of this add-on out of active add-on search paths; avoid loading an old
   Lecram build and NR Nexus together. Keep backups outside those paths.
3. Extract **`renodx-dlss5.addon64`** beside the game executable or into
   ReShade's configured `AddonPath`. Keep the accompanying license materials.
   Preserve ReShade configuration, presets, saves and other DLSS/FG files.
4. Place the compatible `nvngx_dlssnr.dll` beside the add-on. If no copy is
   found there, the add-on checks the game EXE directory. Avoid conflicting
   runtime copies.
5. Start the game and enable its compatible DLSS rendering mode. Open ReShade
   with its configured overlay key, select **NR Nexus**, and turn on
   **Neural Rendering**. Use **Hook point** to choose `Render`, `Upscaled` or
   `Present`; read any fallback or missing-resource messages. If using FG,
   retain the game's intended FG setup and check the displayed backend.
6. Check the version/runtime line and NR activity state.
   **Neural Rendering is active** / `ENGAGED` and diagnostics help establish that NR is working;
   loading the add-on alone does not. Compare NR Off/On in the same scene.
   An active status is not a guarantee of every-frame success or visual quality.

To **upgrade**, close the game, back up the current add-on/settings and replace
only the add-on with the new release's file, following its notes. Do not leave
old copies in another active add-on folder. To **uninstall**, close the game and
remove the add-on or restore the previous backed-up file; retain unrelated
ReShade, game and runtime files.

### Compatibility and known limits

The following describes evidence and implementation scope, not a universal
game-support list. See the selected
[Release notes](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases)
for version-specific results.

| Environment or combination | Status and scope |
| --- | --- |
| The Witcher 3, Steam/D3D12, HDR, Present, XeFG 4× | Focused dev.2 game sessions verified NR toggles, movement, visible NR effect and HUD. |
| D3D12 `Render` / `Upscaled` / `Present` | Implemented. The focused result above does not qualify every processing position, game or FG combination. |
| 310.9.16 dual models (dev.3) | A/B and AB/BA/BB passed synthetic GPU inference and focused Witcher 3 D3D12/Present scene checks. Main B→A switching was exercised in game. Live network-name confirmation uses a local isolated probe; the add-on reports creation requests. HDR visual quality and long sessions remain unvalidated. |
| Native DLSS FG/MFG | Integrated with focused automated coverage. A complete in-game matrix across modes and multipliers has not been validated for this release. |
| SDR and HDR restoration | Independent policies with focused automated coverage; the complete in-game SDR/HDR matrix has not been validated. |
| D3D11 motion sharing | Texture fallback passed targeted hardware/WARP checks; full game compatibility remains unverified. |
| Vulkan | Source path exists, but broad game validation is incomplete. Vulkan `Present` is not implemented and falls back to the DLSS output path. |

- **Known issue: XeFG highlight flicker remains unresolved** on the inherited
  shared rendering chain.
- With DLSS Ray Reconstruction, a `Render` request stays on the `Upscaled`
  path. Check the actual activity/fallback message rather than the selection alone.
- Present processing includes the game's HUD; NR may remain active in
  menus/maps when valid guide resources are available.
- Automated tests do not prove per-game image quality or performance.
  ReShade's wider API support does not imply the same support here.

### Feedback and privacy

Open a [GitHub Issue](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/issues)
with the game/API, GPU and driver, NR Nexus version, loaded NR runtime version,
NR position, FG backend/multiplier, SDR/HDR mode and reproduction steps.
Diagnostics are optional.

In the add-on, **I'm having issues...** (or **Send a report about these issues**)
can prepare text feedback, generate diagnostics, preview/select attachments and
save a local ZIP. Screenshots are excluded by default. Exported text and JSON
are automatically redacted, but **redaction cannot guarantee removal of all
sensitive data**: unfamiliar secrets, private paths and image pixels may remain.
Review every attachment before sharing.

**Upload & copy link** requires your separate, explicit consent to a third-party
upload. The UI displays the service and retention; anyone holding the link may
download the archive. Local saving sends no upload request, upload failure keeps
the ZIP, and user-saved ZIPs are not automatically deleted.

**Prepare GitHub Issue** opens a browser draft; the add-on keeps no GitHub token
and does not submit it. Including a diagnostic summary or report link needs
separate consent. Review and submit the Issue yourself. If the form or prefill
is incomplete, copy the full Markdown into a blank Issue. ZIP saving, uploading
and preparing an Issue are separate actions.

### Building

For developers: use Windows x64, Git, PowerShell 7, Visual Studio 2022's C++
tools **including CMake**, **MSVC 14.44**, and **Windows SDK 10.0.26100.0**.
The shader toolchain needs FXC from that SDK, DXC 1.9.2602 or later and Slang
2026.14.1 or later. The setup command below obtains the pinned DXC/Slang builds
and copies the SDK's FXC; downloads and SHA-256 values are in
[host/tools.lock.json](host/tools.lock.json).

```powershell
git clone https://github.com/williamstosh/RenoDX-DLSSNR-Nexus.git
cd RenoDX-DLSSNR-Nexus
pwsh ./tools/setup-build-tools.ps1
pwsh ./tools/setup-host.ps1
pwsh ./tools/build.ps1 -ToolRoot .local/shader-tools -Configuration Release
```

The output is **`build/Release/renodx-dlss5.addon64`**.

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

[version.h](version.h) defines the independent SemVer; Git commits identify the
source. Daily commits and Actions artifacts are not published releases.
`dev.N` and optional `rc.N` builds are **Pre-releases**; stable builds are
identified separately. Use
[GitHub Releases](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases)
for downloads and version-specific changes.
[GitHub Actions](.github/workflows/ci.yml) builds the source; version tags
trigger the [release workflow](.github/workflows/release.yml).

### Origins, thanks and license

NR Nexus builds on [RenoDX](https://github.com/clshortfuse/renodx), Lecram's
[V8.5 RC10 public add-on source](https://github.com/Yukikaze20170315/RenoDX-DLSS5-Generic/tree/v8.5dev)
and their contributors. The
[imported source commit](https://github.com/williamstosh/RenoDX-DLSS5-Generic/commit/9bb6c0fec57ecb28c77938f3feb8155433396d93)
and [historical fork](https://github.com/williamstosh/RenoDX-DLSS5-Generic)
remain available for provenance.

This is an unofficial independent project, without endorsement from NVIDIA,
RenoDX's authors or Lecram. The original [MIT license](LICENSE), copyright and
[third-party notices](THIRD_PARTY_NOTICES.md) are preserved. The MIT license
does not relicense third-party SDKs, runtimes or models.

---

## 简体中文

NR Nexus 为兼容游戏提供 NVIDIA DLSS Neural Rendering（DLSSNR）画面增强。
项目基于 [RenoDX](https://github.com/clshortfuse/renodx) 开发，通过
[ReShade](https://reshade.me/) 的 Add-on 系统加载，提供游戏内控制面板。
RenoDX 集成已编入插件；NR 围绕游戏已有的 DLSS 渲染链路运行，不执行降噪。

这是基于 Lecram V8.5 RC10 开放源码发展的非官方独立项目，扩展了 NR／FG 协调、
Present 色彩恢复策略、运动纹理共享回退，以及带隐私预览的问题反馈流程。
可用性取决于游戏、渲染路径及另行准备的 NR Runtime，验证范围见下文。

**当前提供 Pre-release 测试版。** 请从
[Releases](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases) 下载，
安装前阅读所选版本说明。

[核心功能](#核心功能) · [安装与使用](#下载安装与使用) · [兼容性](#兼容性与已知限制) · [问题反馈](#问题反馈与隐私) · [开发者构建](#构建)

### 核心功能

| 能力 | 用途 |
| --- | --- |
| 神经渲染增强 | 对游戏画面应用 NR，可调节质量、强度、处理尺寸及精度。 |
| 逐 Pass NR 模型 | 使用 DLSS NR 310.9.16 时，最多四个 Pass 可独立选择 A（Legacy／Preset 1）或 B（Small FP8／Preset 2）。 |
| 三种处理位置 | `Render` 在 DLSS 超分前处理输入；`Upscaled` 处理 DLSS 输出；`Present` 处理游戏后处理／HUD 之后、ReShade 效果之前的最终画面。 |
| NR 配合帧生成 | 集成原生 DLSS FG/MFG 与 XeFG 路径，在帧生成前处理游戏帧，保留所选后端和渲染设置。 |
| SDR/HDR 恢复 | 诊断中提供相互独立的 SDR、HDR 恢复策略，默认 Off；切换 NR 处理位置不会自动启用这些策略。 |
| D3D11 运动纹理共享回退 | 部分运动向量纹理无法直接共享给 NR 处理路径时进行转换；这是资源回退能力，不代表全面支持 D3D11 游戏。 |
| 问题报告 | 支持仅文字反馈、可选诊断与截图、本地 ZIP 保存及浏览器 GitHub Issue 草稿。 |

以上功能已实现；已验证及实验性范围在[兼容性](#兼容性与已知限制)中分别说明。

模型设置保留现有 `NRPreset` 键：`0` 在 310.9.16.0 上选择 B，`1` 选择 A，
`2` 选择 B。`NRPass2Preset` 至 `NRPass4Preset` 的 `0` 表示继承主模型，
`1` 为 A、`2` 为 B，与外观参数共享相互独立。已有显式选择保留。
其他或未识别的 Runtime 禁用 B，将已保存的 B 请求解析为 A；其自动选项继续使用
Runtime 默认值。旧 Preset 3 保留存储，在 310.9.16 上解析为 A；菜单不显示该选项，
已有值为 3 时显示“自定义”，不自动改写配置。
切换模型会重建整链并重置历史，旧 Feature 按 GPU 完成证明退休，工作图像资源复用。
诊断区分创建时请求的 Preset 与 Runtime 确认的网络；API 调用成功不代表实际模型或画质验收通过。
这些模型控件自 v0.1.0-dev.3 起提供，dev.2 安装包不包含。

### 下载、安装与使用

#### 使用前提

- Windows x64，以及兼容的游戏／DLSS 渲染链路。下文实机验证采用 D3D12；
  Vulkan 与 D3D11 另有适用限制。
- 兼容 **ReShade API 18** 的[完整 Add-on 支持版 ReShade](https://reshade.me/)，
  由它加载 `.addon64` 插件。
- 合法取得的兼容 NVIDIA **`nvngx_dlssnr.dll`**，以及该 Runtime 和渲染路径支持的
  GPU／驱动组合。本项目没有已验证的统一最低显卡／驱动清单，请遵循 Runtime 自身要求，
  并检查游戏内诊断。

安装包**不包含** ReShade、NVIDIA Runtime DLL、模型或厂商 SDK 分发包；
着色器、UI 和翻译已编入插件。

#### 安装与首次启用

1. 打开 [Releases](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases)，
   下载 `NR-Nexus-v<version>-<commit>-windows-x64.zip` **发布资产**及其
   `.zip.sha256` 校验文件，核对 ZIP 的 SHA-256。GitHub 自动生成的 **Source code**
   压缩包用于开发，不是可安装插件。
2. 关闭游戏，备份已安装插件及设置。将本插件的旧副本移出所有生效的 Add-on 搜索目录，
   避免旧 Lecram 构建与 NR Nexus 同时加载；备份也应放在这些目录之外。
3. 将 **`renodx-dlss5.addon64`** 放在游戏 EXE 目录或 ReShade 已配置的 `AddonPath`，
   保留随附许可材料。不覆盖 ReShade 配置、预设、存档或其他 DLSS／FG 文件。
4. 将兼容的 `nvngx_dlssnr.dll` 放在插件旁；该位置没有文件时，插件再查找游戏 EXE 目录。
   避免不同位置存在冲突的 Runtime 副本。
5. 启动游戏并启用兼容的 DLSS 渲染模式。使用 ReShade 已配置的叠加层快捷键，
   打开 **NR Nexus**，开启 **Neural Rendering**。在 **介入点（Hook point）** 中选择
   **渲染（Render，超分前）**、**超分后（Upscaled）**或**呈现（Present）**，
   留意回退或资源缺失提示。使用 FG 时保留游戏预期的帧生成设置，并核对显示的后端。
6. 检查版本／Runtime 状态栏及 NR 活动状态。`ENGAGED`、活动提示和诊断可帮助确认
   NR 正在工作；插件已加载不等于 NR 已生效。在同一场景对比 NR Off／On；
   活动状态不保证每帧成功或画质达标。

**升级**时关闭游戏，备份当前插件／设置，按新版说明替换插件文件，不要在另一个生效目录
留下旧副本。**卸载**时关闭游戏，移除该插件或恢复旧备份文件，保留无关的 ReShade、
游戏和 Runtime 文件。

### 兼容性与已知限制

下表区分实际证据与实现范围，不是通用游戏支持列表。
版本专属验证结果请查看[Release Notes](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases)。

| 环境或组合 | 状态与范围 |
| --- | --- |
| 巫师 3，Steam／D3D12、HDR、Present、XeFG 4× | dev.2 定向实机验证通过 NR 开关、移动、可见 NR 效果及 HUD 检查。 |
| D3D12 `Render`／`Upscaled`／`Present` | 已实现；上述定向结果不代表所有处理位置、游戏或 FG 组合均已验收。 |
| 310.9.16 双模型（dev.3） | A/B、AB/BA/BB 通过合成 GPU 推理及巫师3 D3D12／Present 定向场景检查；实机测试主模型 B→A 切换。实际网络名由本地隔离探针确认，插件显示创建请求；HDR 人眼画质及长时运行尚未验收。 |
| 原生 DLSS FG/MFG | 已集成并有定向自动化覆盖；该版本尚未验证跨模式、倍率的完整实机矩阵。 |
| SDR、HDR 恢复 | 策略相互独立并有定向自动化覆盖；尚未验证完整的游戏内 SDR/HDR 组合。 |
| D3D11 运动纹理共享 | 纹理回退已通过定向硬件／WARP 检查，完整游戏兼容性尚未验证。 |
| Vulkan | 源码存在相应路径，但广泛游戏验收不足；Vulkan `Present` 尚未实现，会回退到 DLSS 输出路径。 |

- **已知问题：继承共享渲染链中的 XeFG 高亮瞬闪尚未解决。**
- 使用 DLSS Ray Reconstruction 时，`Render` 请求保持在 `Upscaled` 路径；
  请以实际活动／回退提示为准。
- Present 处理包含游戏 HUD；菜单／地图提供有效引导资源时，NR 仍可能运行。
- 自动化测试不证明各游戏的画质或性能；ReShade 支持更多 API，
  不等于本插件具有相同支持范围。

### 问题反馈与隐私

可直接提交 [GitHub Issue](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/issues)，
建议提供游戏／API、显卡与驱动、NR Nexus 版本、实际加载的 NR Runtime 版本、
NR 介入点、FG 后端／倍率、SDR/HDR 模式和复现步骤；诊断包不是必需项。

插件中的**我遇到了问题...**（或**报告这些问题**）可准备文字反馈、生成诊断、
预览／选择附件并保存本地 ZIP。截图默认排除。导出文本和 JSON 会自动脱敏，
但**自动脱敏不能保证清除全部敏感信息**，未知格式密钥、私人路径和图片像素中的
个人信息仍可能保留；分享前请逐一检查附件。

**Upload & copy link** 需要你单独、明确同意向第三方上传。界面显示服务商及保留期限，
持有链接者均可能下载。仅本地保存不发送上传请求，上传失败保留 ZIP；
用户主动保存的 ZIP 不会被自动删除。

**准备 GitHub Issue** 只打开浏览器草稿，插件不保存 GitHub Token，也不提交 Issue。
附带诊断摘要或报告链接分别需要同意，请在 GitHub 检查后自行提交。
若表单或预填内容不完整，可复制完整 Markdown 到空白 Issue。
ZIP 保存、上传及准备 Issue 是独立操作。

### 构建

开发者需准备 Windows x64、Git、PowerShell 7、Visual Studio 2022 C++ 工具
（**含 CMake**）、**MSVC 14.44** 和 **Windows SDK 10.0.26100.0**。
着色器工具链需要该 SDK 的 FXC、DXC 1.9.2602 或更新版本，以及 Slang 2026.14.1
或更新版本。下列初始化命令获取固定版本的 DXC／Slang 并复制 SDK 的 FXC；
下载地址及 SHA-256 固定于 [host/tools.lock.json](host/tools.lock.json)。

```powershell
git clone https://github.com/williamstosh/RenoDX-DLSSNR-Nexus.git
cd RenoDX-DLSSNR-Nexus
pwsh ./tools/setup-build-tools.ps1
pwsh ./tools/setup-host.ps1
pwsh ./tools/build.ps1 -ToolRoot .local/shader-tools -Configuration Release
```

产物为 **`build/Release/renodx-dlss5.addon64`**。

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

[version.h](version.h) 定义独立 SemVer，Git Commit 用于追溯源码。
日常提交及 Actions artifacts 不等于已发布版本。`dev.N` 和可选的 `rc.N`
属于 **Pre-release**，稳定版本单独标识。正式下载及版本更新详情以
[GitHub Releases](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/releases) 为准。
[GitHub Actions](.github/workflows/ci.yml) 构建源码，
版本 Tag 触发[发布工作流](.github/workflows/release.yml)。

### 来源、致谢与许可证

NR Nexus 基于 [RenoDX](https://github.com/clshortfuse/renodx)、
Lecram [V8.5 RC10 公开插件源码](https://github.com/Yukikaze20170315/RenoDX-DLSS5-Generic/tree/v8.5dev)
及其贡献者的工作开发。
[导入源码提交](https://github.com/williamstosh/RenoDX-DLSS5-Generic/commit/9bb6c0fec57ecb28c77938f3feb8155433396d93)
及[历史 fork](https://github.com/williamstosh/RenoDX-DLSS5-Generic)保留为来源依据。

本项目是非官方独立项目，未获 NVIDIA、RenoDX 作者或 Lecram 背书。
原始 [MIT 许可](LICENSE)、版权及[第三方声明](THIRD_PARTY_NOTICES.md)保留原文；
MIT 许可不改变第三方 SDK、Runtime 或模型的许可条件。
