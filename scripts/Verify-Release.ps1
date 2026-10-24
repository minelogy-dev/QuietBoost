[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [string]$OutputDirectory = 'dist'
)
$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $Executable).Path
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

# Use dumpbin from the installed C++ workload, without requiring a developer shell.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$installation) { throw 'MSVC C++ tools were not found.' }
$toolVersion = (Get-Content -LiteralPath (Join-Path $installation 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$dumpbin = Join-Path $installation "VC/Tools/MSVC/$toolVersion/bin/Hostx64/x64/dumpbin.exe"
$imports = & $dumpbin /dependents $exePath 2>&1
if ($LASTEXITCODE -ne 0) { throw 'dumpbin failed.' }
$imports | Set-Content -LiteralPath (Join-Path $outputPath 'dependencies-report.txt') -Encoding utf8
$dlls = @($imports | ForEach-Object { if ($_ -match '^\s+([\w.-]+\.dll)\s*$') { $Matches[1] } } | Sort-Object -Unique)
if ($dlls.Count -eq 0) { throw 'No DLL imports found; dependency verification did not run correctly.' }
foreach ($dll in $dlls) {
    if ($dll -match '^(msvcp|msvcr|vcruntime|ucrtbase|api-ms-win-crt|mscoree|hostfxr|hostpolicy)') {
        throw "External C/C++ or .NET runtime dependency: $dll"
    }
    if (!(Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32/$dll"))) {
        throw "Non-system dependency: $dll"
    }
}

# Run only the verification command in an isolated directory. No audio playback,
# driver installation, elevation, or default-device changes occur in this check.
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('QuietBoost-ci-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    $isolatedExe = Join-Path $scratch 'QuietBoost.exe'
    Copy-Item -LiteralPath $exePath -Destination $isolatedExe
    $report = Join-Path $scratch 'driver-report.txt'
    $state = Join-Path $scratch 'state'
    $process = Start-Process -FilePath $isolatedExe -ArgumentList @('--state-dir', "`"$state`"", '--verify-driver', "`"$report`"") -WorkingDirectory $scratch -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Embedded driver verification timed out.' }
    if ($process.ExitCode -ne 0) { throw "Embedded driver verification failed: exit $($process.ExitCode)." }
    $verification = Get-Content -LiteralPath $report -Raw
    if ($verification -notmatch 'Installer Authenticode verified successfully\.') { throw 'Driver signature verification report is missing.' }
    Copy-Item -LiteralPath $report -Destination (Join-Path $outputPath 'driver-report.txt')
} finally {
    # The absolute target was created above under the OS temporary directory.
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ([IO.Path]::GetFullPath($scratch).StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path $scratch -Leaf) -like 'QuietBoost-ci-*') {
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
}

$destination = Join-Path $outputPath 'QuietBoost.exe'
if ($exePath -ne $destination) { Copy-Item -LiteralPath $exePath -Destination $destination -Force }
$hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $outputPath 'SHA256SUMS.txt'), "$hash  QuietBoost.exe`n", [Text.UTF8Encoding]::new($false))
Write-Host "Verified single EXE, Windows-only imports and embedded driver: $hash"
