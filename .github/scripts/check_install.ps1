#requires -Version 7.0
param(
    [Parameter(Mandatory)]
    [string]$InstallPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$install_path = (Resolve-Path -LiteralPath $InstallPath).Path
$required_files = @('foradeck.exe', 'qt.conf', 'Qt6Core.dll', 'Qt6Gui.dll',
    'Qt6Widgets.dll', 'Qt6Network.dll', 'Qt6Concurrent.dll',
    'plugins/platforms/qwindows.dll', 'LICENSE', 'licenses/QtKeychain-BSD-3-Clause.txt')
foreach ($file in $required_files) {
    if (-not (Test-Path -LiteralPath (Join-Path $install_path $file) -PathType Leaf)) {
        throw "Missing installed dependency: $file"
    }
}

$diagnostics = Join-Path $PSScriptRoot '../../build/install-check'
New-Item -ItemType Directory -Force -Path $diagnostics | Out-Null
$start_info = [System.Diagnostics.ProcessStartInfo]::new()
$start_info.FileName = Join-Path $install_path 'foradeck.exe'
$start_info.Arguments = '--check-startup'
$start_info.WorkingDirectory = [IO.Path]::GetTempPath()
$start_info.UseShellExecute = $false
$start_info.RedirectStandardOutput = $true
$start_info.RedirectStandardError = $true
# Qt must load from the install tree, not from the SDK or build directory.
$start_info.Environment['PATH'] = "$env:SystemRoot/System32;$env:SystemRoot"
foreach ($name in @($start_info.Environment.Keys)) {
    if ($name -match '^(QT_|QML|QTDIR$)') { $start_info.Environment.Remove($name) | Out-Null }
}
$start_info.Environment['QT_DEBUG_PLUGINS'] = '1'
$start_info.Environment['QT_FORCE_STDERR_LOGGING'] = '1'
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $start_info
$started = $false
try {
    $started = $process.Start()
    if (-not $started) { throw 'Failed to start the installed application.' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(45000)) {
        throw 'Installed application did not finish its startup check within 45 seconds.'
    }
    if ($process.ExitCode -ne 0) {
        throw "Installed application startup check failed with exit code $($process.ExitCode)."
    }
    Write-Output 'MSVC Release install dependencies and isolated startup check passed.'
} finally {
    if ($started) {
        if (-not $process.HasExited) {
            $process.Kill($true)
            $process.WaitForExit()
        }
        $stdout.GetAwaiter().GetResult() | Set-Content (Join-Path $diagnostics 'stdout.log') -Encoding utf8
        $stderr.GetAwaiter().GetResult() | Set-Content (Join-Path $diagnostics 'stderr.log') -Encoding utf8
    }
    $process.Dispose()
}
