# Third-party notices

## RenoDX and Lecram V8.5 RC10 source

- RenoDX: <https://github.com/clshortfuse/renodx>
- Public add-on source: <https://github.com/Yukikaze20170315/RenoDX-DLSS5-Generic/tree/v8.5dev>
- Preserved historical fork: <https://github.com/williamstosh/RenoDX-DLSS5-Generic/tree/v8.5dev>
- Imported commit: `9bb6c0fec57ecb28c77938f3feb8155433396d93`.
- License: MIT; the complete original license and
  `Copyright (c) 2025 Carlos Lopez Jr.` remain in [LICENSE](LICENSE).
- Files: the imported root C++ sources/headers, `shaders/`, and `ui/`.
  Original `Copyright (C) 2026` and SPDX notices are preserved as supplied.
- Migration changes: independent NR Nexus documentation, version header,
  version/build tooling, and the add-on version constant. Core rendering,
  shaders, hook behavior, runtime identity, and original copyright text are
  unchanged.

On 2026-10-23 all 95 baseline blobs matched the public `v8.5dev` source tree,
including its MIT LICENSE. The import message's shorter origin `3a6cfc6` is
preserved historical metadata; this repository does not claim to contain the
author's complete development history. The public mirror and exact imported
commit above are the verifiable source provenance.

## External build and runtime dependencies

These projects are referenced by the compatible RenoDX host; their code,
SDK distributions, binaries, and models are **not vendored in this repository**:

| Dependency | Source and license information |
| --- | --- |
| ReShade | <https://github.com/crosire/reshade> (BSD-3-Clause; additional loader/runtime terms may apply) |
| Dear ImGui | <https://github.com/ocornut/imgui> (MIT) |
| Microsoft Detours | <https://github.com/microsoft/Detours> (MIT) |
| frozen | <https://github.com/serge-sans-paille/frozen> (Apache-2.0) |
| GTL | <https://github.com/greg7mdp/gtl> (Apache-2.0) |
| nlohmann/json | <https://github.com/nlohmann/json> (MIT) |
| NVIDIA Streamline | <https://github.com/NVIDIA-RTX/Streamline> (its upstream license and bundled third-party notices) |
| NVIDIA DLSS SDK/runtime | <https://github.com/NVIDIA/DLSS> (NVIDIA's applicable SDK/runtime terms) |

An add-on binary can incorporate dependency code even when this source-only
repository does not. Before distributing one, check the exact versions' licenses
and include the required copyright, license, and NOTICE files in the package.
The root MIT license does not relicense NVIDIA SDKs, runtime DLLs, or models.
