# NR Nexus

**Unofficial DLSS Neural Rendering Add-on for RenoDX**

[NR Nexus](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus) is an independent,
source-only RenoDX/ReShade add-on project for NVIDIA DLSS Neural Rendering
(DLSSNR, NGX feature 18), often referred to as DLSS5. It applies a visual
enhancement around a game's existing DLSS evaluation; it performs no denoising.

## Development status

The independent version starts at **v0.1.0-dev.1**. This is a source development
snapshot, with no qualified binary, release tag, or GitHub Release at migration.

The imported baseline is Lecram **V8.5 RC10**, commit
[`9bb6c0f`](https://github.com/williamstosh/RenoDX-DLSS5-Generic/commit/9bb6c0fec57ecb28c77938f3feb8155433396d93).
The two existing source-history commits are retained. Subsequent local Present
SDR/HDR fixes and the ongoing NR + Frame Generation integration remain in the
original development workspace and **are not included** in this snapshot.
Compatibility and visual qualification of that integration are still pending;
results from those separate candidates do not qualify this repository's build.

| Branch | Purpose |
| --- | --- |
| `main` | Reviewed independent project baseline and milestone integration |
| `dev` | Subsequent development, integrated after review and applicable checks |

The [original fork](https://github.com/williamstosh/RenoDX-DLSS5-Generic) remains
available as historical reference. This repository is a standalone repository.

## Building

This repository contains only the add-on sources, shaders, and project tooling.
It does not redistribute RenoDX, vendor SDKs, NVIDIA DLLs/models, a ReShade loader,
or game assets. Obtain dependencies from their respective upstream projects
under their own licenses.

The source belongs at `src/addons/dlss5` in a **compatible RenoDX checkout**.
The known research host is RenoDX
[`374d07b`](https://github.com/clshortfuse/renodx/commit/374d07bf0e2fcef112ed1dfee98541bcf16487dd),
but its unmodified shared utilities lack interfaces required by RC10:
`directx::NativeIdentity`, `directx::SameNativeObject`,
`vtable::TransactionMutex`, `vtable::BeginTransaction`, and
`vtable::transaction_contended`. The local compatibility implementations have
not been committed or imported. A compatible committed host is a prerequisite
for a complete build; arbitrary upstream HEAD is not a supported substitute.

With that prerequisite satisfied, use PowerShell 7, Visual Studio 2022 C++ tools,
Windows SDK, FXC, DXC, and Slang, then run:

```powershell
pwsh ./tools/check-version.ps1
pwsh ./tools/build.ps1 -RenodxRoot <compatible-renodx-checkout> -ToolRoot <shader-tool-directory>
```

The build wrapper reuses RenoDX's `dlss5` target and adds only this project's
version definitions. The loader filename remains `renodx-dlss5.addon64`.
Exported names, namespaces, configuration keys, hooks, and runtime identifiers
retain the upstream names until a separately validated internal rename.
See [versioning](docs/VERSIONING.md) and [migration scope](docs/MIGRATION.md).

## Reports and privacy

The inherited optional **Upload & copy link** action sends a report archive to
a third-party temporary file host only when selected by the player. Reports can
contain game/process paths and diagnostic information; review the archive before
sharing. No report archives or local diagnostic receipts are included here.

## Origins and licensing

NR Nexus is based on [RenoDX](https://github.com/clshortfuse/renodx) and Lecram's
V8.5 RC10 public add-on sources. It is an independent unofficial project and is
not endorsed by NVIDIA, RenoDX's authors, or Lecram.

The original [MIT LICENSE](LICENSE) and source copyright notices are preserved.
[Third-party notices](THIRD_PARTY_NOTICES.md) distinguish inherited source from
external build/runtime dependencies. NVIDIA DLSS runtime versions and the
upstream Lecram version are recorded separately from the NR Nexus SemVer.
