#requires -Version 7.0
param(
    [string]$BuildPath = 'cmake-build-release',
    [string]$OutputPath = '',
    [switch]$Installer,
    [string]$InnoCompiler = '',
    [string]$MsvcRuntimePath = '',
    [string]$CompilerLicensePath = '',
    [string]$QtSourceManifest = "$PSScriptRoot/qt_sources.json",
    [string]$QtSourceCache = 'build/windows-packaging-cache'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = (Resolve-Path -LiteralPath "$PSScriptRoot/..").Path
function Resolve-RepositoryPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    [IO.Path]::GetFullPath((Join-Path $repository $path))
}
$build = (Resolve-Path -LiteralPath (Resolve-RepositoryPath $BuildPath)).Path
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
function Read-Cache([string]$name, [switch]$Optional) {
    $entry = @($cache | Select-String -Pattern "^$([regex]::Escape($name)):[^=]+=(.*)$")
    if ($entry.Count -eq 0) {
        if ($Optional) { return '' }
        throw "CMake cache entry is missing: $name"
    }
    $entry[0].Matches[0].Groups[1].Value
}

if ((Read-Cache 'CMAKE_PROJECT_NAME') -ne 'XLibrary' -or
    (Resolve-RepositoryPath (Read-Cache 'CMAKE_HOME_DIRECTORY')) -ne $repository) {
    throw 'Build directory must be configured from this XLibrary checkout.'
}
$cmake = Read-Cache 'CMAKE_COMMAND'
$log = Join-Path $build 'packaging-release.log'
"Release packaging started at $([DateTime]::UtcNow.ToString('o'))" | Set-Content -LiteralPath $log -Encoding utf8
function invoke_cmake([string]$step, [string[]]$command_arguments) {
    "$step (log: $log)" | Tee-Object -FilePath $log -Append
    & $cmake @command_arguments 2>&1 | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) {
        throw "$step failed (exit $LASTEXITCODE). See $log for the compiler/linker diagnostics."
    }
}

function extract_qt_licenses([string]$archive, [string]$member, [string]$destination) {
    "Extracting Qt licenses from $archive with $cmake (timeout 120s)" | Tee-Object -FilePath $log -Append
    $start_info = [Diagnostics.ProcessStartInfo]::new()
    $start_info.FileName = $cmake
    $start_info.WorkingDirectory = $destination
    $start_info.UseShellExecute = $false
    $start_info.CreateNoWindow = $true
    $start_info.RedirectStandardOutput = $true
    $start_info.RedirectStandardError = $true
    foreach ($argument in @('-E', 'tar', 'xf', $archive, '--', $member)) {
        $start_info.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start_info
    $started = $false
    try {
        $started = $process.Start()
        if (-not $started) { throw 'Cannot start Qt license extraction.' }
        # Drain both streams while waiting so a full pipe cannot block the child.
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $finished = $process.WaitForExit(120000)
        if (-not $finished) {
            $process.Kill($true)
            $process.WaitForExit()
        }
        foreach ($output_task in @($stdout, $stderr)) {
            $output_text = $output_task.GetAwaiter().GetResult()
            if ($output_text) { $output_text | Tee-Object -FilePath $log -Append }
        }
        if (-not $finished) { throw "Qt license extraction timed out after 120 seconds. See $log. Staging: $destination" }
        if ($process.ExitCode -ne 0) { throw "Cannot extract Qt licenses (exit $($process.ExitCode)). See $log." }
        'Qt license extraction completed.' | Tee-Object -FilePath $log -Append
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill($true) }
        $process.Dispose()
    }
}

# Check EXE access before rebuilding: a running application prevents the linker from replacing it.
$runtime_directory = Read-Cache 'CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE' -Optional
if (-not $runtime_directory) {
    $runtime_directory = Read-Cache 'CMAKE_RUNTIME_OUTPUT_DIRECTORY' -Optional
    if ($runtime_directory -and (Read-Cache 'CMAKE_CONFIGURATION_TYPES' -Optional)) {
        $runtime_directory = Join-Path $runtime_directory 'Release'
    }
}
if (-not $runtime_directory) {
    $runtime_directory = if (Read-Cache 'CMAKE_CONFIGURATION_TYPES' -Optional) { 'Release' } else { '.' }
}
$executable = Join-Path ([IO.Path]::GetFullPath($runtime_directory, $build)) 'XLibrary.exe'
if (Test-Path -LiteralPath $executable -PathType Leaf) {
    try {
        $executable_stream = [IO.File]::Open($executable, [IO.FileMode]::Open, [IO.FileAccess]::Write,
            [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        $executable_stream.Dispose()
    } catch {
        throw "Cannot replace $executable. Close XLibrary launched from this build directory, including its tray icon, and retry. If it is already closed, check file permissions. $($_.Exception.Message)"
    }
}

$saved_path = $env:PATH
$success = $false
$stage = ''

try {
    # The IPO/LTO check during configure also runs the compiler and needs its DLLs in PATH.
    $config = Get-Content -LiteralPath "$build/packaging-config.json" -Raw | ConvertFrom-Json
    $compiler_bin = Split-Path $config.compiler -Parent
    $qt = [IO.Path]::GetFullPath((Join-Path (Read-Cache 'Qt6_DIR') '../../..'))
    $cmake_bin = Split-Path $cmake -Parent
    $build_paths = @($compiler_bin, "$qt/bin", $cmake_bin)
    # Visual Studio selects MSBuild during the build and may not retain its path in the cache.
    $make_program = Read-Cache 'CMAKE_MAKE_PROGRAM' -Optional
    if ($make_program) {
        $generator_bin = Split-Path $make_program -Parent
        if ($generator_bin) { $build_paths += $generator_bin }
    }
    $env:PATH = ($build_paths + $saved_path) -join ';'
    # The build may rerun configure; refresh the version before choosing the package name.
    invoke_cmake -step 'Release configuration' -command_arguments @('-S', $repository, '-B', $build)
    $cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
    $config = Get-Content -LiteralPath "$build/packaging-config.json" -Raw | ConvertFrom-Json
    $compiler = switch ($config.compiler_id) {
        'GNU' { 'mingw' }
        'MSVC' { 'msvc' }
        default { throw "Unsupported release compiler: $($config.compiler_id)" }
    }
    if ($config.pointer_size -ne '8') { throw 'Distribution requires a Windows x64 build.' }
    if ($compiler -eq 'msvc' -and $config.compiler_architecture -ne 'x64') {
        throw 'Distribution requires an x64 MSVC target.'
    }
    if ($compiler -eq 'mingw') {
        $target_machine = & $config.compiler -dumpmachine
        if ($LASTEXITCODE -ne 0 -or $target_machine -notmatch '^x86_64-.*mingw32$') {
            throw 'Distribution requires an x86_64 MinGW target.'
        }
    }
    $build_type = Read-Cache 'CMAKE_BUILD_TYPE' -Optional
    $configurations = (Read-Cache 'CMAKE_CONFIGURATION_TYPES' -Optional) -split ';'
    if ($build_type -ne 'Release' -and 'Release' -notin $configurations) {
        throw 'Use a Release build directory or a multi-config generator with Release.'
    }
    if ((Read-Cache 'XLIBRARY_NATIVE_CPU') -ne 'OFF') { throw 'Distribution requires XLIBRARY_NATIVE_CPU=OFF.' }
    $qt_version = $config.qt_version
    $version = $config.version
    $channel = if ($config.prerelease -eq 'TRUE') { 'pre-release' } else { 'stable' }
    $source_manifest = Get-Content -LiteralPath (Resolve-RepositoryPath $QtSourceManifest) -Raw |
        ConvertFrom-Json -AsHashtable
    if (-not $source_manifest.ContainsKey($qt_version)) {
        throw "Add verified Qt $qt_version source archive SHA256 values to -QtSourceManifest before packaging."
    }
    if ($Installer) {
        if (-not $InnoCompiler) {
            $iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue
            if ($iscc) { $InnoCompiler = $iscc.Source }
        }
        if (-not $InnoCompiler -or -not (Test-Path -LiteralPath $InnoCompiler -PathType Leaf)) {
            throw 'Install Inno Setup 6.3+ and pass -InnoCompiler or put ISCC.exe on PATH.'
        }
    }
    if (-not $OutputPath) { $OutputPath = "release/v$version-$compiler" }
    $output = Resolve-RepositoryPath $OutputPath
    if (Test-Path -LiteralPath $output) { throw "Output already exists; choose a new -OutputPath: $output" }
    $package_name = "XLibrary-$version-windows-x64-$compiler-portable"
    $stage = Join-Path $repository "build/release-$([guid]::NewGuid().ToString('N'))"
    $package = Join-Path $stage $package_name
    invoke_cmake -step 'Release build' -command_arguments @('--build', $build, '--config', 'Release',
        '--target', 'XLibrary', '--parallel', '4')
    invoke_cmake -step 'Qt deployment' -command_arguments @('--install', $build, '--config', 'Release',
        '--prefix', $package)

    $licenses = Join-Path $package 'licenses'
    New-Item -ItemType Directory -Path $licenses -Force | Out-Null
    if ($compiler -eq 'mingw') {
        foreach ($dll in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
            $runtime = & $config.compiler "-print-file-name=$dll"
            if ($LASTEXITCODE -eq 0 -and $runtime -eq $dll) {
                $runtime = Join-Path $compiler_bin $dll
            }
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
                throw "Configured MinGW cannot locate $dll"
            }
            Copy-Item -LiteralPath $runtime -Destination $package
        }
        $toolchain = Split-Path $compiler_bin -Parent
        $license_roots = if ($CompilerLicensePath) { @($CompilerLicensePath) } else {
            @("$toolchain/licenses", "$toolchain/share/doc", "$toolchain/share/licenses")
        }
        $gcc_licenses = Join-Path $licenses 'mingw'
        New-Item -ItemType Directory -Path $gcc_licenses | Out-Null
        $index = 0
        foreach ($root in $license_roots) {
            if (Test-Path -LiteralPath $root -PathType Container) {
                Copy-Item -LiteralPath $root -Destination "$gcc_licenses/$index" -Recurse
                ++$index
            }
        }
        foreach ($required in @('COPYING3', 'COPYING.RUNTIME')) {
            if (@(Get-ChildItem -LiteralPath $gcc_licenses -Recurse -File -Filter $required).Count -eq 0) {
                throw "MinGW license $required is missing; pass -CompilerLicensePath containing toolchain notices."
            }
        }
    } else {
        if (-not $MsvcRuntimePath) {
            if (-not $env:VCToolsRedistDir) { throw 'Use an x64 VS developer shell or pass -MsvcRuntimePath.' }
            $MsvcRuntimePath = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'
        }
        $runtime_files = @(Get-ChildItem -LiteralPath $MsvcRuntimePath -Filter '*.dll' -File)
        if ($runtime_files.Count -eq 0) { throw "No MSVC CRT DLLs in $MsvcRuntimePath" }
        $runtime_files | Copy-Item -Destination $package
    }

    # Keep sources for all distributed modules with their licenses, including third-party code.
    $modules = [Collections.Generic.HashSet[string]]::new()
    [void]$modules.Add('qtbase')
    $base_libraries = @('Core', 'Gui', 'Widgets', 'Network', 'Concurrent', 'OpenGL', 'OpenGLWidgets',
        'PrintSupport', 'Sql', 'Xml', 'DBus')
    foreach ($dll in Get-ChildItem -LiteralPath $package -Filter 'Qt6*.dll' -File) {
        $module = $dll.BaseName.Substring(3)
        if ($module -in @('Svg', 'SvgWidgets')) { [void]$modules.Add('qtsvg') }
        elseif ($module -notin $base_libraries) { throw "Add source/licensing support for $($dll.Name)." }
    }
    if (@(Get-ChildItem -LiteralPath "$package/plugins/imageformats" -Filter '*.dll' -ErrorAction SilentlyContinue |
            Where-Object { $_.BaseName -in @('qicns', 'qmng', 'qtga', 'qtiff', 'qwbmp', 'qwebp', 'qjp2') }).Count -gt 0) {
        [void]$modules.Add('qtimageformats')
    }
    $sources = Join-Path $package 'sources'
    New-Item -ItemType Directory -Path $sources | Out-Null
    $source_cache = Resolve-RepositoryPath $QtSourceCache
    New-Item -ItemType Directory -Path $source_cache -Force | Out-Null
    $notices = [Collections.Generic.List[string]]::new()
    $notices.Add("Qt $qt_version is dynamically linked under LGPLv3; see licenses/qtbase and sources/.")
    $notices.Add('You may replace the Qt DLLs/plugins with compatible modified builds. Reverse engineering for debugging those modifications is permitted.')
    $notices.Add('Qt licensing: https://www.qt.io/licensing/open-source-lgpl-obligations')
    foreach ($module in ($modules | Sort-Object)) {
        $hash = $source_manifest[$qt_version][$module]
        if ($hash -notmatch '^[0-9a-fA-F]{64}$') { throw "Missing verified SHA256 for $module $qt_version" }
        $archive_name = "$module-everywhere-src-$qt_version.tar.xz"
        $url = "https://download.qt.io/archive/qt/$($qt_version -replace '\.\d+$','')/$qt_version/submodules/$archive_name"
        $archive = Join-Path $sources $archive_name
        $cached_archive = Join-Path $source_cache $archive_name
        if (Test-Path -LiteralPath $cached_archive -PathType Leaf) {
            "Verifying cached Qt source SHA256: $archive_name" | Tee-Object -FilePath $log -Append
            if ((Get-FileHash -LiteralPath $cached_archive -Algorithm SHA256).Hash -ne $hash) {
                throw "Qt source cache checksum mismatch. Remove or replace this file: $cached_archive"
            }
            Write-Output "Using verified Qt source cache: $cached_archive"
        } else {
            # Use the Windows executable, not a PowerShell alias. curl bounds the entire transfer.
            $curl = (Get-Command curl.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
            # An incomplete download must not become a reusable archive.
            $download_path = "$cached_archive.$([guid]::NewGuid().ToString('N')).download"
            try {
                for ($attempt = 1; $attempt -le 3; ++$attempt) {
                    try {
                        Write-Output "Downloading Qt source ($attempt/3, timeout 300s): $url"
                        & $curl --fail --location --proto '=https' --proto-redir '=https' `
                            --connect-timeout 20 --max-time 300 --speed-limit 1024 --speed-time 30 `
                            --silent --show-error --output $download_path `
                            --write-out 'Qt source transfer: %{size_download} bytes in %{time_total}s (%{speed_download} bytes/s)\n' $url
                        if ($LASTEXITCODE -ne 0) { throw "curl failed with exit code $LASTEXITCODE." }
                        break
                    } catch {
                        if ($attempt -eq 3) {
                            throw "Cannot download $url. You can place the archive in $cached_archive. $($_.Exception.Message)"
                        }
                        Write-Warning "Qt source download interrupted; retrying: $($_.Exception.Message)"
                        Start-Sleep -Seconds (2 * $attempt)
                    }
                }
                "Verifying downloaded Qt source SHA256: $archive_name" | Tee-Object -FilePath $log -Append
                if ((Get-FileHash -LiteralPath $download_path -Algorithm SHA256).Hash -ne $hash) {
                    throw "Qt source checksum mismatch: $archive_name"
                }
                Move-Item -LiteralPath $download_path -Destination $cached_archive -Force
            } finally {
                if (Test-Path -LiteralPath $download_path) { Remove-Item -LiteralPath $download_path -Force }
            }
        }
        "Copying verified Qt source into package: $archive_name" | Tee-Object -FilePath $log -Append
        Copy-Item -LiteralPath $cached_archive -Destination $archive
        extract_qt_licenses -archive $archive -member "$module-everywhere-src-$qt_version/LICENSES" -destination $stage
        Copy-Item -LiteralPath "$stage/$module-everywhere-src-$qt_version/LICENSES" -Destination "$licenses/$module" -Recurse
        $sbom_files = @(Get-ChildItem -Path "$qt/sbom/$module*.spdx*" -File -ErrorAction SilentlyContinue)
        foreach ($sbom in $sbom_files) { Copy-Item -LiteralPath $sbom.FullName -Destination $licenses }
        $notices.Add("`n$module corresponding source: sources/$archive_name`nUpstream: $url`nSHA256: $hash")
    }
    $notices.Add("`nSQLite is statically linked and in the public domain: https://sqlite.org/copyright.html")
    if ($compiler -eq 'mingw') {
        $notices.Add('MinGW runtime: GCC GPLv3 with GCC Runtime Library Exception; see licenses/mingw (including MinGW-w64/winpthreads notices).')
    } else {
        $notices.Add('Microsoft Visual C++ runtime: https://visualstudio.microsoft.com/license-terms/ ; redistribution requires the applicable Visual Studio license.')
    }
    $notices | Set-Content -LiteralPath "$package/THIRD_PARTY_NOTICES.txt" -Encoding utf8

    $commit = git -C $repository rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the source commit.' }
    $changes = @(git -C $repository status --short)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source changes.' }
    @{
        version = $version
        channel = $channel
        architecture = 'x64'
        configuration = 'Release'
        qt_version = $qt_version
        compiler = $compiler
        compiler_version = $config.compiler_version
        source_commit = $commit
        source_dirty = $changes.Count -gt 0
        source_changes = $changes
        built_at_utc = [DateTime]::UtcNow.ToString('o')
    } | ConvertTo-Json | Set-Content -LiteralPath "$package/build-info.json" -Encoding utf8

    New-Item -ItemType File -Path "$package/portable.flag" | Out-Null
    @"
XLibrary $version - Windows x64 ($compiler)

Run XLibrary.exe. Keep the DLLs and plugins beside it.
No Qt installation or administrator rights are required.
portable.flag enables portable mode: database and settings use data/.
Keep this folder writable. Move the whole folder to move the program and data.
The installer uses the Windows user profile and preserves data on uninstall.
This build has no digital signature. See licenses/, sources/ and THIRD_PARTY_NOTICES.txt.
"@ | Set-Content -LiteralPath "$package/README.txt" -Encoding utf8
    & "$repository/.github/scripts/check_package.ps1" -PackagePath $package -Compiler $compiler
    New-Item -ItemType Directory -Path $output | Out-Null
    if ($Installer) {
        & $InnoCompiler /Qp "/DAppVersion=$version" "/DNumericVersion=$($config.windows_numeric_version)" "/DCompiler=$compiler" "/DPackageDir=$package" "/DOutputDir=$output" "$PSScriptRoot/xlibrary.iss"
        if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
    }
    Compress-Archive -LiteralPath $package -DestinationPath "$output/$package_name.zip" -CompressionLevel Optimal
    Get-ChildItem -LiteralPath $output -File | Where-Object { $_.Extension -in '.exe', '.zip' } |
        Sort-Object Name | ForEach-Object {
            "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $($_.Name)"
        } | Set-Content -LiteralPath "$output/SHA256SUMS.txt" -Encoding ascii
    $success = $true
    Write-Output "Release prepared: $output"
} finally {
    $env:PATH = $saved_path
    if ($stage -and (Test-Path -LiteralPath $stage -PathType Container)) {
        if ($success) {
            $resolved_stage = (Resolve-Path -LiteralPath $stage).Path
            $stage_root = (Resolve-Path -LiteralPath "$repository/build").Path + [IO.Path]::DirectorySeparatorChar
            if (-not $resolved_stage.StartsWith($stage_root, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to remove staging outside $stage_root"
            }
            Remove-Item -LiteralPath $resolved_stage -Recurse -Force
        } else { Write-Warning "Packaging failed. Staging files remain at $stage" }
    }
}
