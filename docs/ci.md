# Continuous integration

This guide describes the checked-in workflow definitions. Actual GitHub runs and server-side branch rules must be inspected separately when assessing a particular PR.

## Events and coverage

Ordinary workflows run for pull requests targeting `main`, `merge_group` and manual `workflow_dispatch`. They do not run on push, so merging a checked PR does not trigger a duplicate push build. Draft PRs are checked. A newer run cancels an older run for the same workflow/event/PR or ref; PR, manual and merge-queue runs have separate concurrency keys.

PR events are the default `opened`, `synchronize` and `reopened`. Title/description edits do not trigger a run. If an existing PR's base is changed to main, reopen it or add a commit to trigger checks. Merge queue checks the combined source tree before merging.

| Workflow | Coverage |
| --- | --- |
| [Windows Build](../.github/workflows/windows-build.yml) | Windows 2022 x64, Qt 6.8.3, MSVC Debug/Release and MinGW 13.1 Debug/Release; CTest in all four configurations |
| Windows MSVC Debug | clang-tidy during compilation with Ninja/MSVC environment |
| Windows MSVC Release | Syntax parsing of tracked PowerShell scripts; CMake install, Qt DLL/plugin checks and isolated startup from a Unicode/space path |
| [Linux Build](../.github/workflows/linux-build.yml) | Ubuntu 24.04 runner with Qt SDK 6.8.3; GCC/Clang Debug/Release, CTest, X11/Xvfb startup, install and desktop-entry validation, installed offscreen startup |
| Linux Clang Debug | clang-tidy during compilation |
| Linux package job | Debian 13 container with system Qt 6.8.2; AppImage/DEB, offscreen/X11 startup, SHA256 verification, DEB installation/removal |
| [Workflow Lint](../.github/workflows/workflow-lint.yml) | Hash-verified actionlint, packaging Python syntax, CMake presets and tracked JSON parsing |
| [Clang Format](../.github/workflows/clang-format.yml) | All tracked C/C++ files with clang-format 18.1.8 |
| [Prepare Release Draft](../.github/workflows/release.yml) | Separate manual main-only release packaging, metadata/hash checks and unpublished Draft; see [releasing](releasing.md#github-release-draft) |

Ordinary CI creates no tags, release Drafts or deployments. Its Linux packages are retained for **14 days** as `XLibrary-linux-x86_64` artifacts.

## Diagnostics and limits

Ordinary build/package jobs have **10-minute** timeouts; lint/format jobs have **5 minutes**. Release workflow limits are 5 minutes for validation, 45 for each platform and 15 for Draft assembly. A timeout fails the job.

Qt SDK downloads are cached. Linux builds use ccache, capped at 200 MiB per configuration, with keys based on preset, Qt and CMake inputs. Install trees are not cached. Linux build jobs use available runner cores; Windows and packagers use up to four parallel build tasks. Release Windows jobs cache verified Qt source archives, rechecking hashes after restoration.

External startup checks normally have a **45-second** timeout; Linux packager startup checks use **60 seconds**. The Windows MSVC Release install step is limited to two minutes. Failed ordinary builds upload CMake cache/configuration logs and CTest `LastTest.log` for **7 days**. Windows install diagnostics also include stdout/stderr and Qt plugin logging under `build/install-check/`. Local Windows packager output is saved in `<BuildPath>/packaging-release.log`; local package checks use `build/package-check/`.

Successful compilation, CTest and `--check-startup` establish different things. Startup uses a temporary profile and does not prove interactive imports, scanning, updates, hotkeys, Wayland behavior or profile preservation after upgrades. MSVC's ordinary install check uses the runner's CRT; standalone packaging checks app-local CRT separately. DEB installation in a build container does not replace the required clean Debian 13 validation. See [testing](testing.md).

## Branch rules template

[.github/rulesets/main.json](../.github/rulesets/main.json) is an import template, not an automatically applied workflow or evidence of current server settings. It defines `main-ci` with PR review, one code-owner approval, dismissal of stale approvals, resolved review threads, strict up-to-date checks and these **11** contexts:

| Group | Contexts |
| --- | --- |
| Lint | `clang-format`, `actionlint` |
| Windows | `windows-msvc-Release`, `windows-msvc-Debug`, `windows-mingw-Release`, `windows-mingw-Debug` |
| Linux | `linux-debug`, `linux-release`, `linux-clang-debug`, `linux-clang-release`, `linux-AppImage-and-DEB` |

The template retains an always-on bypass for the Repository admin role. [CODEOWNERS](../.github/CODEOWNERS) assigns `@lappland-open` to all paths, including CI and the ownership file. GitHub uses CODEOWNERS from the target branch. A PR author cannot approve their own PR; delegating code-owner review requires another maintainer with write access in CODEOWNERS.

An administrator applies/updates rules through **Settings → Rules → Rulesets** or the GitHub API. Verify successful hosted runs with the intended names before requiring new checks. When renaming jobs, update both the template and server settings. Remove obsolete required contexts from the server before merging their workflow removal, or GitHub may wait indefinitely for them.

Workflows use minimal permissions, checkout without persisted credentials and actions pinned to commit SHAs. Only the release Draft job gets `contents: write`. [Dependabot](../.github/dependabot.yml) groups weekly GitHub Actions updates. No workflow executes PR code through `pull_request_target`.
