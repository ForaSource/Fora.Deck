#requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$SourceCommit
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$directory = (Resolve-Path -LiteralPath $PackageDirectory).Path
$base = "XLibrary-$Version-windows-x64-mingw"
$scratch = Join-Path ([IO.Path]::GetTempPath()) "XLibrary release $([guid]::NewGuid().ToString('N'))"
$installed = Join-Path $scratch 'Установка с пробелами'
New-Item -ItemType Directory -Path $scratch | Out-Null
function invoke_installer([string]$executable, [string]$arguments) {
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru
    try {
        if (-not $process.WaitForExit(120000)) {
            $process.Kill($true)
            throw 'Installer timed out.'
        }
        if ($process.ExitCode -ne 0) { throw "Installer failed: $($process.ExitCode)" }
    } finally { $process.Dispose() }
}
try {
    $expected = @("$base-portable.zip", "$base-setup.exe")
    $seen = @()
    foreach ($line in Get-Content -LiteralPath "$directory/SHA256SUMS.txt") {
        if ($line -cnotmatch '^([0-9a-f]{64})  ([^/\\]+)$') { throw 'Invalid SHA256SUMS.txt.' }
        $hash = $Matches[1]
        $name = $Matches[2]
        if ($name -cnotin $expected -or $name -cin $seen) { throw "Unexpected checksum entry: $name" }
        if ((Get-FileHash -LiteralPath (Join-Path $directory $name) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) {
            throw "Checksum mismatch: $name"
        }
        $seen += $name
    }
    if ($seen.Count -ne 2) { throw 'Both Windows packages are required.' }
    Expand-Archive -LiteralPath "$directory/$base-portable.zip" -DestinationPath "$scratch/portable"
    $portable = "$scratch/portable/$base-portable"
    & "$PSScriptRoot/check_package.ps1" -PackagePath $portable -Compiler mingw
    $metadata = Get-Content -LiteralPath "$portable/build-info.json" -Raw | ConvertFrom-Json
    if ($metadata.version -cne $Version -or $metadata.source_commit -cne $SourceCommit -or
        $metadata.source_dirty -isnot [bool] -or $metadata.source_dirty -or $metadata.qt_version -cne '6.8.3') {
        $actual_metadata = $metadata | ConvertTo-Json -Depth 4 -Compress
        throw "Windows package version/source does not match the validated checkout. Expected version=$Version, source_commit=$SourceCommit, source_dirty=false, qt_version=6.8.3. Actual metadata: $actual_metadata"
    }
    $setup = "$directory/$base-setup.exe"
    # Inno Setup pads the textual PE version field with spaces.
    $installer_product_version = ([string](Get-Item -LiteralPath $setup).VersionInfo.ProductVersion).TrimEnd(' ')
    if ($installer_product_version -cne $Version) {
        throw "Installer product version mismatch. Expected '$Version', actual '$installer_product_version'."
    }
    invoke_installer $setup "/VERYSILENT /SUPPRESSMSGBOXES /SP- /NORESTART /DIR=`"$installed`" /LOG=`"$scratch/install.log`""
    if (Test-Path -LiteralPath "$installed/portable.flag") { throw 'Installer enabled portable mode.' }
    if ((Get-FileHash -LiteralPath "$installed/build-info.json").Hash -cne
        (Get-FileHash -LiteralPath "$portable/build-info.json").Hash) {
        throw 'Installer and portable contain different build metadata.'
    }
    & "$PSScriptRoot/check_install.ps1" -InstallPath $installed
    invoke_installer "$installed/unins000.exe" '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
    if (Test-Path -LiteralPath "$installed/XLibrary.exe") { throw 'Uninstall did not remove XLibrary.' }
    Copy-Item -LiteralPath "$portable/build-info.json" -Destination "$directory/build-info.json"
    Write-Output 'Windows ZIP, installer, metadata, checksums, startup and uninstall verified.'
} finally {
    # Preserve logs and the test install after failure for diagnosis on the runner.
    Write-Output "Windows release check directory: $scratch"
}
