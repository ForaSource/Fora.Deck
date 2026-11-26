#requires -Version 7.0
param(
    [Parameter(Mandatory)]
    [string]$PackagePath,
    [ValidateSet('msvc', 'mingw')]
    [string]$Compiler = 'msvc'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$package = (Resolve-Path -LiteralPath $PackagePath).Path
$required_files = @('foradeck.exe', 'qt.conf', 'Qt6Core.dll', 'Qt6Gui.dll',
    'Qt6Widgets.dll', 'Qt6Network.dll', 'Qt6Concurrent.dll', 'plugins/platforms/qwindows.dll',
    'LICENSE', 'THIRD_PARTY_NOTICES.txt', 'build-info.json', 'portable.flag',
    'licenses/qtbase/LGPL-3.0-only.txt', 'licenses/qtbase/GPL-3.0-only.txt')
if ($Compiler -eq 'msvc') {
    $required_files += @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
} else {
    $required_files += @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')
}
foreach ($file in $required_files) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $file) -PathType Leaf)) {
        throw "Missing package dependency: $file"
    }
}
$info = Get-Content -LiteralPath "$package/build-info.json" -Raw | ConvertFrom-Json
if ($info.compiler -ne $Compiler -or $info.configuration -ne 'Release' -or $info.architecture -ne 'x64' -or
    $info.source_commit -notmatch '^[0-9a-f]{40}$' -or $info.qt_version -notmatch '^6\.\d+\.\d+$') {
    throw 'Invalid release build-info.json.'
}
if (-not (Test-Path -LiteralPath "$package/sources/qtbase-everywhere-src-$($info.qt_version).tar.xz" -PathType Leaf)) {
    throw 'Corresponding Qt source is missing.'
}
if ($Compiler -eq 'mingw') {
    foreach ($name in @('COPYING3', 'COPYING.RUNTIME')) {
        if (@(Get-ChildItem -LiteralPath "$package/licenses/mingw" -Filter $name -File -Recurse).Count -eq 0) {
            throw "Missing MinGW license: $name"
        }
    }
}

$diagnostics = Join-Path $PSScriptRoot '../../build/package-check'
New-Item -ItemType Directory -Force -Path $diagnostics | Out-Null
$diagnostics = (Resolve-Path -LiteralPath $diagnostics).Path
# Use a copy to protect the user's portable profile and check Unicode/spaces in paths.
$scratch = Join-Path ([IO.Path]::GetTempPath()) "Fora.Deck проверка $([guid]::NewGuid().ToString('N'))"
$test_package = Join-Path $scratch 'Пакет с пробелами'
New-Item -ItemType Directory -Path $scratch | Out-Null
Copy-Item -LiteralPath $package -Destination $test_package -Recurse
# Verify that --check-startup neither reads nor modifies the existing portable profile.
New-Item -ItemType Directory -Path "$test_package/data" -Force | Out-Null
$sentinel = [Text.Encoding]::UTF8.GetBytes('startup must not touch this database')
[IO.File]::WriteAllBytes("$test_package/data/foradeck.db", $sentinel)
$settings = "[behavior]`nstartup=tray"
Set-Content -LiteralPath "$test_package/data/settings.ini" -Value $settings -NoNewline
try {
    foreach ($mode in @('portable', 'installed')) {
        if ($mode -eq 'installed') { Remove-Item -LiteralPath "$test_package/portable.flag" }
        $start_info = [System.Diagnostics.ProcessStartInfo]::new()
        $start_info.FileName = Join-Path $test_package 'foradeck.exe'
        $start_info.Arguments = '--check-startup'
        $start_info.WorkingDirectory = $scratch
        $start_info.UseShellExecute = $false
        $start_info.RedirectStandardOutput = $true
        $start_info.RedirectStandardError = $true
        # Exclude installed Qt and toolchains from the launch environment.
        $start_info.Environment['PATH'] = "$env:SystemRoot/System32;$env:SystemRoot"
        foreach ($name in @($start_info.Environment.Keys)) {
            if ($name -match '^(QT_|QML|QTDIR$)') { $start_info.Environment.Remove($name) | Out-Null }
        }
        $start_info.Environment['QT_DEBUG_PLUGINS'] = '1'
        $start_info.Environment['QT_FORCE_STDERR_LOGGING'] = '1'
        # An invalid installed profile verifies that the temporary startup-check profile takes precedence.
        $start_info.Environment['LOCALAPPDATA'] = 'invalid-relative-profile'
        $process = [System.Diagnostics.Process]::new()
        $process.StartInfo = $start_info
        $started = $false
        try {
            $started = $process.Start()
            if (-not $started) { throw 'Failed to start packaged application.' }
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(45000)) {
                $process.Kill($true)
                $process.WaitForExit()
                throw 'Packaged application did not finish its startup check within 45 seconds.'
            }
            if ($process.ExitCode -ne 0) {
                throw "Packaged application $mode startup check failed with exit code $($process.ExitCode)."
            }
        } finally {
            if ($started) {
                if (-not $process.HasExited) {
                    $process.Kill($true)
                    $process.WaitForExit()
                }
                $stdout.GetAwaiter().GetResult() | Set-Content (Join-Path $diagnostics "$mode-stdout.log") -Encoding utf8
                $stderr.GetAwaiter().GetResult() | Set-Content (Join-Path $diagnostics "$mode-stderr.log") -Encoding utf8
            }
            $process.Dispose()
        }
        if ([Convert]::ToBase64String([IO.File]::ReadAllBytes("$test_package/data/foradeck.db")) -ne
            [Convert]::ToBase64String($sentinel) -or
            (Get-Content -LiteralPath "$test_package/data/settings.ini" -Raw) -cne $settings -or
            @(Get-ChildItem -LiteralPath "$test_package/data" -File).Count -ne 2) {
            throw 'Startup check changed the portable user profile.'
        }
    }
    Write-Output 'Dependencies, metadata, licenses, Unicode paths and isolated startup checks passed.'
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
