# NR Nexus project rules

- This is the standalone, source-only `RenoDX-DLSSNR-Nexus` repository. Keep
  changes within the requested scope; preserve `renodx-dlss5.addon64`, exported
  runtime names, configuration identity, and core NR/FG/hook behavior unless a
  separate change explicitly authorizes and validates them.
- `version.h` is the sole independent SemVer source. Daily commits do not bump
  versions. Update it only for an explicit, verifiable milestone; Lecram and
  NVIDIA runtime versions are provenance, not this project's version.
- Reuse RenoDX's build target through `tools/build.ps1`; keep the displayed
  version, PE FileVersion, archive stem, and any `v<version>` tag consistent.
  Use short commit metadata for traceability, not long descriptive suffixes.
- Create an immutable version tag only after the corresponding committed
  milestone passes affected builds, focused tests, public safety/license checks,
  and applicable runtime/visual acceptance. Never move or reuse a tag. A version
  in source alone is not a validated release.
- A validated development milestone may be published automatically as a
  Pre-release. RCs require internal qualification; stable Releases require the
  owner's explicit confirmation. Do not infer release approval from a push.
- `main` holds reviewed integrations; `dev` holds development. Preserve the
  original fork and the concurrent NR+FG workspace. Integrate its changes only
  after they are committed, reviewed for public safety, and accepted for the
  intended scope. See [migration scope](docs/MIGRATION.md).
- Before reusing third-party code, shaders, assets, or dependencies, verify
  source, license compatibility, and redistribution rights. Preserve all
  copyright/license/NOTICE text and update `THIRD_PARTY_NOTICES.md` for actual
  reuse. Technical ideas alone do not require attribution entries.
- Never commit secrets, private machine paths, proprietary NVIDIA DLLs/SDKs or
  models, game data, local receipts, logs, caches, or build outputs. Review every
  newly reachable historical commit before a public push.
- Before delivery run `tools/check-version.ps1`, the affected build and focused
  checks, and `git diff --check`. Record build success, installation, runtime
  validation, visual acceptance, and release authorization separately. A failed
  host compatibility build does not qualify a binary or justify changing core
  rendering logic during migration.
