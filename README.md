# NR Nexus

[English](#english) | [简体中文](#简体中文)

## English

**Unofficial DLSS Neural Rendering add-on for RenoDX/ReShade**

[NR Nexus](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus) is an independent,
source-only project for NVIDIA DLSS Neural Rendering (DLSSNR, NGX feature 18),
often referred to as DLSS5. It applies a visual enhancement around a game's
existing DLSS evaluation; it performs no denoising.

### Development status

The independent version is **v0.1.0-dev.1**, defined in [version.h](version.h).
This is a source development snapshot with no qualified binary, release tag,
or GitHub Release. The imported baseline is Lecram **V8.5 RC10**,
commit [`9bb6c0f`](https://github.com/williamstosh/RenoDX-DLSS5-Generic/commit/9bb6c0fec57ecb28c77938f3feb8155433396d93).
Later Present SDR/HDR fixes and NR + Frame Generation work are not included.
The host API compatibility gap described below still blocks a complete add-on
build; there is no Nexus installation or game/visual acceptance.

`main` holds reviewed integrations; `dev` is the development branch.
The [original fork](https://github.com/williamstosh/RenoDX-DLSS5-Generic)
remains available for historical reference.

### Building

This repository contains add-on sources, shaders, and project tooling. Obtain
RenoDX, vendor SDKs, NVIDIA runtime DLLs/models, the ReShade loader, and other
dependencies separately under their respective licenses; none are bundled here.

Place this checkout at `src/addons/dlss5` in a **compatible RenoDX checkout**.
A committed host with the required shared interfaces is a prerequisite;
arbitrary upstream HEAD is not a supported substitute. With this prerequisite
satisfied, use the build commands below.

The known unmodified host is RenoDX
[`374d07b`](https://github.com/clshortfuse/renodx/commit/374d07bf0e2fcef112ed1dfee98541bcf16487dd).
It lacks `directx::NativeIdentity`, `directx::SameNativeObject`,
`vtable::TransactionMutex`, `vtable::BeginTransaction`, and
`vtable::transaction_contended`, which this source calls. Compatible
implementations have not been integrated into this repository; shader or
version-resource checks alone do not qualify a complete build.

Required tools: PowerShell 7, Visual Studio 2022 C++ tools, Windows SDK, FXC,
DXC, and Slang.

```powershell
pwsh ./tools/check-version.ps1
pwsh ./tools/build.ps1 -RenodxRoot <compatible-renodx-checkout> -ToolRoot <shader-tool-directory>
```

The wrapper reuses RenoDX's `dlss5` target and adds the project's version
definitions. The loader filename remains `renodx-dlss5.addon64`; exported names,
namespaces, configuration keys, hooks, and runtime identifiers retain their
existing names until a separately authorized and validated rename.

### Reports and privacy

The inherited optional **Upload & copy link** action sends a report archive to
a third-party temporary file host only when selected by the player. Reports can
contain game/process paths and diagnostics; review the archive before sharing.
Report archives and local diagnostic evidence are not included in this repository.

### Origins and licensing

NR Nexus is based on [RenoDX](https://github.com/clshortfuse/renodx) and Lecram's
V8.5 RC10 public add-on sources. It is not endorsed by NVIDIA, RenoDX's authors,
or Lecram. The original [MIT license](LICENSE), copyright notices, and
[third-party notices](THIRD_PARTY_NOTICES.md) are preserved in their original
language. Lecram and NVIDIA runtime versions are separate from NR Nexus SemVer.

---

## 简体中文

**面向 RenoDX/ReShade 的非官方 DLSS 神经渲染插件**

[NR Nexus](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus) 是独立、仅提供源码的
NVIDIA DLSS 神经渲染项目（DLSSNR，NGX feature 18，通常称为 DLSS5）。它围绕游戏已有的
DLSS 求值过程应用视觉增强，不执行降噪。

### 开发状态

独立版本为 **v0.1.0-dev.1**，定义于 [version.h](version.h)。当前是源码开发快照，
尚无通过验收的二进制、发布标签或 GitHub Release。导入基线为 Lecram **V8.5 RC10**，
提交 [`9bb6c0f`](https://github.com/williamstosh/RenoDX-DLSS5-Generic/commit/9bb6c0fec57ecb28c77938f3feb8155433396d93)。
后续 Present SDR/HDR 修复及 NR + 帧生成开发尚未包含在内。下述宿主 API 兼容性缺口仍阻塞完整插件构建，
当前尚无 Nexus 安装或游戏／视觉验收。

`main` 保存已审查集成，`dev` 用于开发。
[原 fork](https://github.com/williamstosh/RenoDX-DLSS5-Generic) 保留为历史参考。

### 构建

本仓库包含插件源码、着色器和项目工具。RenoDX、厂商 SDK、NVIDIA 运行时 DLL／模型、
ReShade 加载器及其他依赖须按各自许可另行获取，均未随仓库分发。

将本仓库置于**兼容 RenoDX 工作副本**的 `src/addons/dlss5`。完整构建要求宿主已提交且具备所需共享接口，
不能用任意上游 HEAD 替代。满足此前提后，才可使用下方构建命令。

已知未经修改的宿主为 RenoDX
[`374d07b`](https://github.com/clshortfuse/renodx/commit/374d07bf0e2fcef112ed1dfee98541bcf16487dd)，
缺少本源码调用的 `directx::NativeIdentity`、`directx::SameNativeObject`、
`vtable::TransactionMutex`、`vtable::BeginTransaction` 和 `vtable::transaction_contended`。
兼容实现尚未集成到本仓库，仅着色器或版本资源检查通过不代表完整构建合格。

所需工具：PowerShell 7、Visual Studio 2022 C++ 工具、Windows SDK、FXC、DXC 和 Slang。

```powershell
pwsh ./tools/check-version.ps1
pwsh ./tools/build.ps1 -RenodxRoot <compatible-renodx-checkout> -ToolRoot <shader-tool-directory>
```

包装器复用 RenoDX 的 `dlss5` 目标并加入项目版本定义。加载器文件名保留为 `renodx-dlss5.addon64`；
导出名称、命名空间、配置键、钩子和运行时标识沿用现有名称，重命名须单独授权并验证。

### 报告与隐私

继承的可选 **Upload & copy link** 操作仅在玩家主动选择时，将报告压缩包发送至第三方临时文件托管服务。
报告可能包含游戏／进程路径及诊断信息，分享前请检查内容。本仓库不包含报告压缩包或本地诊断证据。

### 来源与许可

NR Nexus 基于 [RenoDX](https://github.com/clshortfuse/renodx) 和 Lecram V8.5 RC10 公开插件源码，
未获 NVIDIA、RenoDX 作者或 Lecram 背书。原始 [MIT 许可](LICENSE)、版权和
[第三方声明](THIRD_PARTY_NOTICES.md)保留原文。Lecram 及 NVIDIA 运行时版本与 NR Nexus SemVer 分开记录。
