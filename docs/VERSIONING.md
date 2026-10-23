# Independent versions

`version.h` owns the SemVer string without a leading `v`. The initial source
version is `0.1.0-dev.1`. The overlay/log version uses `v` plus that string;
the CMake integration supplies the same string as the Windows PE FileVersion.
The numeric PE tuple uses `major,minor,patch,0`; prerelease order belongs to
the full string, not Windows' four-integer tuple.

The loader filename stays `renodx-dlss5.addon64`. A qualified package can use
the `ArchiveStem` printed by `tools/check-version.ps1`, for example
`NR-Nexus-v0.1.0-dev.1-<short-sha>.zip`. Keep the loader filename inside it.
No binary packaging or publication is implied by that suggested name.

Daily commits use Git history and do not increment the version. Update the
header after a concrete development milestone, such as `0.1.0-dev.2`.
Use `0.1.0-rc.1` only after internal qualification; `0.1.0` requires stable
acceptance and the owner's explicit release confirmation. Patch/minor/major
increments follow SemVer; the example sequence is not mandatory. During `0.x`,
the public compatibility surface is experimental and changes must be documented.
Lecram V8.5 RC10 and NVIDIA runtime/DLL versions remain separate provenance.

## Milestone and release procedure

1. Review and commit the milestone with an updated version header and acceptance
   evidence. Build with `tools/build.ps1`; run affected tests and applicable
   game/visual acceptance. Record inputs, commit, tool/dependency identities,
   artifact hash, and any untested scope. Reuse valid evidence for unchanged work.
2. Run `tools/check-version.ps1` and `git diff --check`; inspect both the public
   tree and newly reachable history for secrets, private paths, proprietary
   assets, build debris, and licensing obligations. Keep machine-specific
   receipts outside the public tree.
3. Once those gates pass on the committed milestone, create a new annotated
   `v<version>` tag. Verify it with
   `tools/check-version.ps1 -ExpectedTag <tag> -VerifyTag`. Never overwrite tags.
4. Development tags may produce GitHub **Pre-releases** automatically after the
   gates pass. RC publication requires internal acceptance. Publishing a stable
   GitHub Release additionally requires explicit owner confirmation. Upload only
   qualified add-on binaries and required notices, never vendor runtime DLLs or
   models. A source push does not trigger publication or a version bump.

This migration does not install an unconditional tag-to-release workflow. Its
inherited host API dependency has not been integrated as committed code, and
the NR+FG work is outside the imported snapshot. No tag or Release is created
until the corresponding milestone meets its gates.
