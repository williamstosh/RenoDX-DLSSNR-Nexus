# Source migration

Verified scope: 2026-10-23, independent repository initialization.

The imported branch tip is
`9bb6c0fec57ecb28c77938f3feb8155433396d93`, with parent
`2663ecd393b0d402318c4cfd1fdc847f85e90beb`. These are the complete reachable
source-only history of the imported `v8.5dev` branch. The import commit records
Lecram V8.5 RC10 source origin `3a6cfc6`. The original MIT license and notices
remain unchanged. No other branches or upstream tags are migrated.

The repository contains only the add-on's 95 baseline source/license/document
files plus independent project documentation and version/build tooling. It
does not copy the full RenoDX project, external dependencies, private workspace
documentation, diagnostic evidence, binaries, or models.

## Deferred integration

The original research and development repositories keep their branches,
remotes, uncommitted files, build directories, and running environment.
Uncommitted Present SDR/HDR fixes, compatibility utility implementations,
NR+FG integration, tests, and runtime qualification stay there. These changes
must not be mistaken for features present in the migrated RC10 baseline.

After acceptance, commit those changes in their authorized original repository,
then review the intended public diff and its license/safety status. Integrate
only the accepted scope into `dev`; retain source-commit provenance when nested
repositories or a source-only layout require a scoped patch instead of a direct
cherry-pick. Review any dependencies on the two shared utility headers before
selecting a build host. Only then decide whether the original environment should
switch its default remote. No switch is required during the migration.

## Known validation boundary

The unmodified RenoDX host commit `374d07bf0e2fcef112ed1dfee98541bcf16487dd`
lacks five shared API symbols required by the imported RC10 source. This was a
known build prerequisite before migration; the local compatibility changes are
uncommitted and deliberately excluded. Version checks and shader compilation
cannot substitute for a complete compatible-host build or game acceptance.

### Migration validation results

- Version checker, PowerShell syntax checks, and direct local documentation
  links passed. A mismatched requested tag and a missing tag argument were
  rejected without creating a tag.
- An isolated MSVC x64 Release build configured through RenoDX's existing
  CMake target. All 20 DirectX and 12 Vulkan shader binaries were generated.
  The C++ build failed on the five missing shared interfaces listed above;
  no complete add-on binary was qualified. No uncommitted compatibility code
  was imported to make that build pass.
- A separate minimal PE probe reused the real generated RenoDX resource file
  and this project's CMake version hook. Its compiled resource reported
  `FileVersion=0.1.0-dev.1`, numeric `0.1.0.0`, and
  `OriginalFilename=renodx-dlss5.addon64`. This validates the version mechanism,
  not a complete plugin or game run.
- The public baseline and its two reachable commits were reviewed. No embedded
  credentials, real private paths, proprietary binaries/models, or local
  diagnostic receipts were found. Profile paths in `report_bundle.hpp` are
  generic redaction examples. New public documents contain no private paths.

The inherited optional report uploader relies on WinHTTP's redirect policy
without a destination allowlist. Its upload/privacy behavior is unchanged and
was not runtime-qualified during migration; review report contents before
sharing. No uploads were performed for this task.

Use `git log` in this repository for the migration commit identity. Local build
logs and the final migration receipt are kept in ignored `tmp/`; they are not
public acceptance claims. No Release is created during initialization.
