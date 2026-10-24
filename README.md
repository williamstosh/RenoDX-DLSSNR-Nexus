# NR Nexus

[English](#english) | [简体中文](#简体中文)

## English

**DLSS Neural Rendering Add-on for RenoDX**

[NR Nexus](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus) is an independent,
source-only project for NVIDIA DLSS Neural Rendering (DLSSNR, NGX feature 18),
often referred to as DLSS5. It applies a visual enhancement around a game's
existing DLSS evaluation; it performs no denoising.

### Development status

The independent SemVer is defined in [version.h](version.h).
This integration contains the Present SDR/HDR restoration policies, configuration
schema v8, native DLSS Frame Generation and XeFG coordination, and D3D11 motion
texture sharing fallback developed from Lecram **V8.5 RC10**,
commit [`9bb6c0f`](https://github.com/williamstosh/RenoDX-DLSS5-Generic/commit/9bb6c0fec57ecb28c77938f3feb8155433396d93).
NR can run before upscaling, after upscaling, or in the Present path; frame
generation routing preserves the existing backend and rendering settings.

Complete Release builds have passed using two independently prepared hosts.
Focused C++ tests cover host object identity and transactions, SDR/HDR policies,
configuration migration, native/translated FG, resource lifetime, mode changes,
and D3D11 motion sharing on hardware and WARP. SDK/model substitutes in these
tests do not qualify real game output.

The new Nexus binary passed a focused Witcher 3 HDR/Present test with XeFG 4×:
loading, NR toggles, short movement, HUD and protected exit/restoration were
checked, with user confirmation of the visible NR effect and HUD. Unchanged
paths reuse earlier acceptance under its original candidate and tested scope;
this is not a fresh four-game visual matrix. This is a development milestone
with no qualified release binary, tag, or GitHub Release. `main` holds accepted
integrations; `dev` is the development branch. The
[original fork](https://github.com/williamstosh/RenoDX-DLSS5-Generic)
remains available for historical reference.

### Building

The host and eight dependency commits are pinned in [host/lock.json](host/lock.json).
[The compatibility patch](host/renodx-compat.patch) supplies the two object-identity
and three transaction interfaces missing from RenoDX
[`374d07b`](https://github.com/clshortfuse/renodx/commit/374d07bf0e2fcef112ed1dfee98541bcf16487dd).
Setup fetches those exact public sources into a new local directory and links
this checkout as `src/addons/dlss5`. It does not read another development workspace.

Use Windows x64, PowerShell 7, Visual Studio 2022 C++ tools with MSVC 14.44,
Windows SDK 10.0.26100.0, FXC, DXC 1.9.2602 or later, and Slang 2026.14.1 or later.
Supply the shader tools in `<shader-tool-directory>`:

```powershell
pwsh ./tools/setup-host.ps1
pwsh ./tools/check-version.ps1
pwsh ./tools/build.ps1 -ToolRoot <shader-tool-directory>
pwsh ./tools/run-tests.ps1 -Suite all
```

Setup defaults to `.local/renodx` and refuses to overwrite an existing directory;
use `-HostRoot <new-host-directory>` for a fresh independent checkout, then pass
that path to the build script as `-RenodxRoot` and the test script as `-HostRoot`.
The build checks pinned inputs, rebuilds Detours from source, and uses RenoDX's
`dlss5` target with the independent version definitions. The output is
`build/Release/renodx-dlss5.addon64`.

The ReShade tab/window is **NR Nexus** (**NR Nexus (Debug)** in Debug builds).
The status line shows `NR Nexus v<SemVer> | DLSSNR <loaded runtime version> | <game.exe>`.
The add-on version comes from `version.h`; the runtime version and process name
are read at runtime. Brand strings and PE metadata share [brand.h](brand.h).
Translations are maintained directly in `ui/lang/`; the upstream translation
generator is not included. `tools/check-branding.ps1` checks changed source files
by default (also run by the build); use `-Path ui/strings.hpp` for a focused check
or `-All` for an explicit source audit. `tools/run-tests.ps1 -Suite branding`
checks all language tables. Historical attribution and compatibility identifiers
such as loader/configuration names and report paths retain their original values.

Sources, shaders, host patch and test tools are included. The full RenoDX host,
vendor SDK distributions, runtime DLLs/models, ReShade loader and game assets
are obtained separately under their own licenses. The loader filename,
namespaces, exported names, configuration keys, hooks and runtime identifiers
retain their existing names.

### Compatibility and verification limits

Support is limited to the tested backend, inputs and scenario. The inherited
common-chain XeFG highlight flicker remains unresolved; its exclusion from an
earlier acceptance scope is not a root-cause fix. Required guides can still
allow NR on menus/maps with valid supplied resources. The source retains
SDR/HDR diagnostic policies; configuration v8 defaults use scope1, SDR Off and
HDR Off without changing NR quality settings.

The [game test entry](tools/game-test/README.md) describes the inherited Witcher 3
Steam automation and its local target setup. It protects saves/configuration,
but it is not a generic game driver and does not replace visual acceptance.

### Reports and privacy

**I'm having issues...** lets you describe a problem, choose an optional
screenshot comparison, collect diagnostics, preview/select attachments and save
a local ZIP. You can also continue without diagnostics. All exported text,
including report.json, is redacted; common user paths, Windows identity fields
and recognizable credentials are removed. Screenshots are excluded by default.
Review the full staged files before sharing: unknown secrets, other private paths
and personal information in image pixels can remain.

**Upload & copy link** requires separate consent and tries the displayed
Litterbox/temp.sh hosts in the configured order (NRReportHost, including none).
The UI shows each host and its configured retention; anyone with the link can
download the archive. Local save sends no upload request, and upload failure keeps
the ZIP. Saved ZIPs are never automatically pruned.

**Prepare GitHub Issue** opens the official repository form in your browser with
a reviewed text draft. Including a diagnostic summary or temporary report link
requires separate consent. The plugin holds no GitHub token and submits no Issue;
you review and submit on GitHub. The complete Markdown can be copied if prefill,
login, connectivity or browser launch fails. ZIP saving, uploading and Issue
preparation are independent. Reports retain the legacy schema/path identifiers
with additive diagnostics and explicit availability/measurement descriptions.

GitHub activates form templates from the repository's default branch. If a form
is unavailable or ignores the prefill, use **Copy Markdown and open a blank
Issue** to paste the complete draft yourself.

Run `tools/run-tests.ps1 -Suite report -PythonExecutable PYTHON` for synthetic
report/ZIP/privacy checks and loopback-only upload tests. The existing
RENODX_NR_TEST_REPORT* test hooks remain loopback-only and suppress browser
launches. Local configuration, reports and validation evidence are not included here.

### Origins and licensing

NR Nexus is based on [RenoDX](https://github.com/clshortfuse/renodx) and Lecram's
V8.5 RC10 public add-on sources. It is not endorsed by NVIDIA, RenoDX's authors
or Lecram. The original [MIT license](LICENSE), copyright notices and
[third-party notices](THIRD_PARTY_NOTICES.md) are preserved in their original
language. Lecram and NVIDIA runtime versions are separate from NR Nexus SemVer.

---

## 简体中文

**面向 RenoDX 的 DLSS 神经渲染插件**

[NR Nexus](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus) 是独立、仅提供源码的
NVIDIA DLSS 神经渲染项目（DLSSNR，NGX feature 18，通常称为 DLSS5）。它围绕游戏已有的
DLSS 求值过程应用视觉增强，不执行降噪。

### 开发状态

独立 SemVer 定义于 [version.h](version.h)。本次集成包含基于 Lecram
**V8.5 RC10** 提交 [`9bb6c0f`](https://github.com/williamstosh/RenoDX-DLSS5-Generic/commit/9bb6c0fec57ecb28c77938f3feb8155433396d93)
开发的 Present SDR/HDR 恢复策略、配置 schema v8、原生 DLSS 帧生成及 XeFG 协调、
D3D11 运动纹理共享回退。NR 可在超分前、超分后或 Present 路径执行；帧生成路由保留现有后端与渲染设置。

两套独立准备的宿主均已完成完整 Release 构建。定向 C++ 测试覆盖宿主对象身份与事务、
SDR/HDR 策略、配置迁移、原生／转接 FG、资源生命周期、模式切换及硬件／WARP 的 D3D11 运动共享。
测试中的 SDK／模型替身不能证明真实游戏输出合格。

新 Nexus 二进制已通过巫师3 HDR／Present、XeFG 4× 定向验证：核对实际加载、NR 开关、短移动、HUD、
受保护的退出和恢复，NR 可见效果与 HUD 获用户确认。未变化路径按原候选及已测范围复用此前验收，
不代表新做了一套四游戏视觉矩阵。当前为开发里程碑，尚无通过发布验收的二进制、标签或 GitHub Release。
`main` 保存已验收集成，
`dev` 用于开发；[原 fork](https://github.com/williamstosh/RenoDX-DLSS5-Generic)保留为历史参考。

### 构建

宿主及八项依赖的提交固定于 [host/lock.json](host/lock.json)。
[兼容补丁](host/renodx-compat.patch)补齐 RenoDX
[`374d07b`](https://github.com/clshortfuse/renodx/commit/374d07bf0e2fcef112ed1dfee98541bcf16487dd)
缺失的两个对象身份接口和三个事务接口。初始化从公开源获取指定提交，写入新的本地目录，
并将本仓库链接为 `src/addons/dlss5`，不读取其他开发工作区。

使用 Windows x64、PowerShell 7、Visual Studio 2022 C++ 工具及 MSVC 14.44、
Windows SDK 10.0.26100.0、FXC、DXC 1.9.2602 或更新版本、Slang 2026.14.1 或更新版本。
将着色器工具放在 `<shader-tool-directory>`：

```powershell
pwsh ./tools/setup-host.ps1
pwsh ./tools/check-version.ps1
pwsh ./tools/build.ps1 -ToolRoot <shader-tool-directory>
pwsh ./tools/run-tests.ps1 -Suite all
```

初始化默认写入 `.local/renodx`，拒绝覆盖已有目录；需要全新独立副本时使用
`-HostRoot <new-host-directory>`，再向构建脚本传入该路径的 `-RenodxRoot`、
向测试脚本传入 `-HostRoot`。构建核对固定输入，从源码重建 Detours，复用 RenoDX 的
`dlss5` 目标并加入独立版本定义，产物为 `build/Release/renodx-dlss5.addon64`。

ReShade 页签／窗口名称为 **NR Nexus**（Debug 构建为 **NR Nexus (Debug)**）。
状态栏显示 `NR Nexus v<SemVer> | DLSSNR <已加载运行时版本> | <游戏.exe>`。
插件版本来自 `version.h`；运行时版本和进程名称在运行时读取。品牌文本与 PE 元数据共用
[brand.h](brand.h)。翻译直接维护于 `ui/lang/`，仓库未包含上游翻译生成器。
`tools/check-branding.ps1` 默认只检查变更的源码文件，构建时也会调用；可用
`-Path ui/strings.hpp` 定向检查，或用 `-All` 显式审查全部源码。
`tools/run-tests.ps1 -Suite branding` 检查全部语言表。历史来源及加载器／配置名称、
报告路径等兼容标识沿用原值。

仓库包含源码、着色器、宿主补丁和测试工具。完整 RenoDX 宿主、厂商 SDK 分发包、
运行时 DLL／模型、ReShade 加载器及游戏资源须按各自许可另行获取。加载器文件名、
命名空间、导出名称、配置键、钩子及运行时标识沿用现有名称。

### 兼容性与验证边界

支持范围限于已测后端、输入和场景。继承的 XeFG 共同链路高亮瞬闪尚未解决；
此前验收范围排除不等于根因修复。Required guides 在提供有效资源的菜单／地图上仍可能允许 NR。
源码保留 SDR/HDR 诊断策略；配置 v8 默认采用 scope1、SDR Off、HDR Off，不改变 NR 质量设置。

[游戏测试入口](tools/game-test/README.md)说明继承的巫师3 Steam 自动化及本地目标配置。
它保护存档／配置，但不是通用游戏驱动，也不能代替视觉验收。

### 报告与隐私

**我遇到了问题…** 支持描述问题、选择可选截图、生成诊断、预览／选择附件及保存本地 ZIP，
也可不生成诊断继续反馈。所有导出文本（含 report.json）统一脱敏，移除常见用户路径、
Windows 身份字段及可识别凭据；截图默认排除。分享前请检查完整暂存文件：未知格式密钥、
其他私人路径及图片像素中的个人信息仍可能保留。

**Upload & copy link** 需要单独授权，按 NRReportHost 配置（含 none）依次尝试显示的
Litterbox／temp.sh 服务。界面说明各服务及配置的保留期限，持有链接者均可下载。
仅本地保存不发送上传请求，上传失败保留 ZIP；用户保存的 ZIP 不会被自动清理。

**准备 GitHub Issue** 在系统浏览器打开官方仓库表单并预填已检查的文本；附带诊断摘要或临时报告
链接分别需要授权。插件不保存 GitHub Token、不提交 Issue，用户在 GitHub 检查后自行提交。
预填、登录、网络或浏览器启动失败时，可复制完整 Markdown 手动反馈。
ZIP 保存、临时上传及 Issue 准备相互独立；原报告 schema／路径标识保留，诊断字段增量扩展，
注明可用状态及计量口径。

GitHub 从仓库默认分支启用表单模板。模板不可用或未接受预填时，可使用**复制 Markdown 并打开空白
Issue**，自行粘贴完整草稿。

使用 `tools/run-tests.ps1 -Suite report -PythonExecutable PYTHON` 执行合成报告、ZIP、
隐私及仅回环上传测试；原 RENODX_NR_TEST_REPORT* 入口保留回环限制并抑制浏览器启动。
本仓库不包含本地配置、报告或验收原始证据。

### 来源与许可

NR Nexus 基于 [RenoDX](https://github.com/clshortfuse/renodx) 和 Lecram V8.5 RC10 公开插件源码，
未获 NVIDIA、RenoDX 作者或 Lecram 背书。原始 [MIT 许可](LICENSE)、版权和
[第三方声明](THIRD_PARTY_NOTICES.md)保留原文。Lecram 及 NVIDIA 运行时版本与 NR Nexus SemVer 分开记录。
