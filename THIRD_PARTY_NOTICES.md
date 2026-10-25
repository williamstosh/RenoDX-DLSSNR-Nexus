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
- Initial migration changes: independent NR Nexus documentation, version header,
  version/build tooling, and the add-on version constant. Core rendering,
  shaders, hook behavior, runtime identity, and original copyright text are
  unchanged.

On 2026-10-23 all 95 baseline blobs matched the public `v8.5dev` source tree,
including its MIT LICENSE. The import message's shorter origin `3a6cfc6` is
preserved historical metadata; this repository does not claim to contain the
author's complete development history. The public mirror and exact imported
commit above are the verifiable source provenance.

## Integrated development and host compatibility

The integration retains the source changes developed from the imported RC10
baseline: Present SDR/HDR restoration policies, configuration migration,
native/translated frame-generation coordination, resource lifetime handling,
and associated shaders and focused tests. Original copyright and SPDX notices
remain intact. The independent version definition is the only adaptation to
the imported production snapshot's project version.

`host/renodx-compat.patch` applies to RenoDX commit
`374d07bf0e2fcef112ed1dfee98541bcf16487dd` (MIT, original copyright above).
It modifies `src/utils/directx.hpp` and `src/utils/vtable.hpp` to supply the
required canonical-object-identity and Detours transaction interfaces.
The full host is not vendored. `host/lock.json` identifies its exact source and
the external dependency commits used by the independent setup/build scripts.

## GITC-Uplift research and local validation references

- Project: [GITC-Uplift](https://github.com/ghostinthecamera/GITC-Uplift).
- Author: Hussain Abdullah.
- Referenced source snapshot:
  [`e1681d14f5ce105f4a7d53d7d10e985cdef5f65e`](https://github.com/ghostinthecamera/GITC-Uplift/tree/e1681d14f5ce105f4a7d53d7d10e985cdef5f65e).
- Related history reviewed:
  [`f949dcdea18756638499e37b4f7d7dbe944b80d7`](https://github.com/ghostinthecamera/GITC-Uplift/commit/f949dcdea18756638499e37b4f7d7dbe944b80d7)
  (310.9 support) and
  [`fb66565d892f0680a31036b92f5464036dd6f787`](https://github.com/ghostinthecamera/GITC-Uplift/commit/fb66565d892f0680a31036b92f5464036dd6f787)
  (per-pass network selection).
- License: [MIT at the referenced snapshot](https://github.com/ghostinthecamera/GITC-Uplift/blob/e1681d14f5ce105f4a7d53d7d10e985cdef5f65e/LICENSE),
  `Copyright (c) 2026 Hussain Abdullah`.

**Technical ideas.** Uplift's published research and implementation informed
the DLSS NR 310.9.16 dual-model work: A/Legacy maps to preset 1 and B/Small FP8
to preset 2; the preset is supplied at Feature creation; later passes can
inherit the main model independently of appearance sharing. Feature recreation,
history reset and GPU retirement were also studied. The relevant sources are
its [README](https://github.com/ghostinthecamera/GITC-Uplift/blob/e1681d14f5ce105f4a7d53d7d10e985cdef5f65e/README.md),
[`src/nr/feature.cpp`](https://github.com/ghostinthecamera/GITC-Uplift/blob/e1681d14f5ce105f4a7d53d7d10e985cdef5f65e/src/nr/feature.cpp),
[`src/ui/settings.cpp`](https://github.com/ghostinthecamera/GITC-Uplift/blob/e1681d14f5ce105f4a7d53d7d10e985cdef5f65e/src/ui/settings.cpp)
and [`src/nr/session.cpp`](https://github.com/ghostinthecamera/GITC-Uplift/blob/e1681d14f5ce105f4a7d53d7d10e985cdef5f65e/src/nr/session.cpp).

**Distributed add-on implementation.** NR Nexus's
[dual-model change](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/commit/93a45834484f3683691856f602e68257e2b5bb0c)
implements model resolution and integration in `nr_model.hpp`, `dlssnr.hpp`
and `vulkan_path.hpp`, using its existing per-pass Feature/history slots and
per-handle GPU retirement. Those slots, the preset parameter key and the
retirement mechanism predate this change. No Uplift Feature/Session
implementation code was copied or adapted into that add-on change, and its
whole-pool retirement strategy was not adopted.

**Local validation tooling and limited code adaptation.** A separate,
unpublished validation probe referenced
[`src/nr/snippet.cpp`](https://github.com/ghostinthecamera/GITC-Uplift/blob/e1681d14f5ce105f4a7d53d7d10e985cdef5f65e/src/nr/snippet.cpp)
for the 310.9.16 logging callback layout and signature, thread-local attribution
of Feature-creation messages, and native network-name parsing. Its small
network-name parser adapts the parsing logic in that file. This limited
local-tool adaptation is distinct from the add-on implementation above.
The probe is absent from the tracked source and release packages; private
callback access and runtime-confirmed network-name reporting are not shipped
NR Nexus features. No Uplift binaries or NVIDIA runtime/model files were
imported by this work.

Uplift's copyright and full MIT notice are retained below for the local parser
adaptation:

```text
MIT License

Copyright (c) 2026 Hussain Abdullah

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

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

## Binary package notices

The Windows x64 add-on incorporates Microsoft Detours and uses code from the
RenoDX, ReShade, Dear ImGui, nlohmann/json, Streamline and DLSS SDK headers.
`tools/package.ps1` obtains the applicable license texts from the commits in
`host/lock.json`, preserves additional header copyright notices and includes
them under `licenses/` alongside this notice and the original project license.
The package does not redistribute the ReShade loader, NVIDIA runtime DLLs,
models, SDK distributions or shader compiler binaries. Those dependencies
remain subject to their own terms and are obtained separately.

## Menu interaction and visualization references

The menu also studied Uplift's [UI implementation](https://github.com/ghostinthecamera/GITC-Uplift/blob/e1681d14f5ce105f4a7d53d7d10e985cdef5f65e/src/ui/overlay.cpp)
and [Guide](https://github.com/ghostinthecamera/GITC-Uplift/wiki/Guide) for setting
hierarchy, conditional result controls and resource-state explanations. Its split-stage
pass scheduling was not adopted.

[Prism NR](https://github.com/ChefDavid0815/prism-nr/tree/9c0a3e90534f2a86a09f387e509c4bb099ce4a88)
(MIT, Copyright (c) 2026 ChefZC) was studied through its
[overlay implementation](https://github.com/ChefDavid0815/prism-nr/blob/9c0a3e90534f2a86a09f387e509c4bb099ce4a88/native/prism-core/src/overlay.cpp)
for measured-data visualization and compact status hierarchy. These are design
references only: no source, renderer, screenshots, fonts or other assets from
either project were copied into this menu change. NR Nexus retains its ReShade
ImGui backend and existing processing architecture.
