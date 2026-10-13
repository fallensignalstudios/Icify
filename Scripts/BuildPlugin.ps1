# Copyright Fallen Signal Studios. All Rights Reserved.
<#
.SYNOPSIS
    Builds and packages Icify for Fab, once per engine version.

.DESCRIPTION
    For each engine version:
      1. Finds the engine (-EngineRoots, then $env:UE_5_x_ROOT, then the Epic launcher registry entry, then
         "C:\Program Files\Epic Games\UE_5.x") and checks Engine\Build\Build.version matches.
      2. Stages a clean copy of the plugin (Source, Content, Shaders, Resources, Config, the .uplugin) and sets
         "EngineVersion": "<ver>.0" in the staged descriptor. The repository is never modified.
         With -ContentRoot, <ContentRoot>\<ver>\ replaces Content\ in the staged copy (content saved by that engine).
      3. Runs the static Fab checks (Scripts\CheckFabReadiness.py) against the staged copy, if python is available.
      4. Runs RunUAT BuildPlugin -StrictIncludes for the target platforms and fails the version if UAT fails or the
         log contains any compiler warning or error (MSVC "warning C####"/"error C####", clang ": warning:"/": error:",
         linker errors).
      5. Zips the staged copy (top-level folder "Icify") as a Fab upload: no Binaries, Intermediate, Saved,
         DerivedDataCache, .git, Scripts or Docs.
    Prints a summary table and exits non-zero if any version failed.

.PARAMETER EngineVersions
    Engine versions to build, e.g. 5.5,5.6. Default: 5.5, 5.6, 5.7, 5.8.

.PARAMETER EngineRoots
    Hashtable of version -> engine root (the folder containing Engine\), e.g. @{ '5.6' = 'D:\UE_5.6' }.

.PARAMETER EngineRoot
    Engine root for a single version (only with exactly one -EngineVersions entry).

.PARAMETER TargetPlatforms
    '+'-separated UAT platforms. Default: Win64, plus Linux when a Linux cross-compile toolchain is configured
    ($env:LINUX_MULTIARCH_ROOT_5_x for that version, else $env:LINUX_MULTIARCH_ROOT; each engine version wants
    the toolchain release Epic lists for it). Mac binaries can only be built on a Mac (use BuildPlugin.sh there).

.PARAMETER OutputDir
    Where packages, logs and zips go. Default: <plugin>\Saved\FabBuild (Saved is excluded from the zip and from git).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Scripts\BuildPlugin.ps1
.EXAMPLE
    pwsh Scripts\BuildPlugin.ps1 -EngineVersions 5.6 -EngineRoot 'D:\Engines\UE_5.6' -TargetPlatforms Win64
#>
[CmdletBinding()]
param(
    [string[]] $EngineVersions = @('5.5', '5.6', '5.7', '5.8'),
    [hashtable] $EngineRoots = @{},
    [string] $EngineRoot = '',
    [string] $TargetPlatforms = '',
    [string] $OutputDir = '',
    [string] $ContentRoot = '',
    [switch] $SkipBuild,
    [switch] $SkipReadiness,
    [switch] $KeepStaging
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Descriptor = Get-ChildItem -Path $PluginRoot -Filter '*.uplugin' -File | Select-Object -First 1
if (-not $Descriptor) { throw "No .uplugin found in $PluginRoot" }
$PluginName = [IO.Path]::GetFileNameWithoutExtension($Descriptor.Name)
$VersionName = ((Get-Content -Raw -Path $Descriptor.FullName) | ConvertFrom-Json).VersionName
if (-not $OutputDir) { $OutputDir = Join-Path $PluginRoot 'Saved\FabBuild' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)

# What ships. Everything else (Binaries, Intermediate, Saved, DerivedDataCache, .git, Scripts, Docs, README, ...) does not.
$ShippedFolders = @('Source', 'Content', 'Shaders', 'Resources', 'Config')
$ExcludedNames = @('Binaries', 'Intermediate', 'Saved', 'DerivedDataCache', '.git', '.vs', '.idea', 'Scripts', 'Docs', '__pycache__')
$ExcludedFiles = @('.gitkeep', '.gitignore', '.gitattributes', '.DS_Store', 'Thumbs.db')

$WarningPattern = '(?i)(\bwarning C\d{4}\b|: warning:|\bwarning LNK\d{4}\b)'
$ErrorPattern = '(?i)(\berror C\d{4}\b|: error:|: fatal error|\bfatal error C\d{4}\b|\berror LNK\d{4}\b)'

# "-EngineVersions 5.5,5.6" arrives as one string when the script is run with -File; accept commas and spaces.
$EngineVersions = @($EngineVersions | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ })
foreach ($v in $EngineVersions) { if ($v -notmatch '^5\.\d+$') { throw "Engine version '$v' should look like 5.6" } }
if ($EngineRoot -and $EngineVersions.Count -ne 1) { throw '-EngineRoot needs exactly one -EngineVersions entry; use -EngineRoots @{...} for several.' }
$GlobalLinuxToolchain = $env:LINUX_MULTIARCH_ROOT

# Each engine version expects its own Linux cross-compile toolchain; LINUX_MULTIARCH_ROOT_5_6 (etc.) overrides
# LINUX_MULTIARCH_ROOT for that version.
function Get-Platforms([string] $Version) {
    $toolchain = [Environment]::GetEnvironmentVariable('LINUX_MULTIARCH_ROOT_' + $Version.Replace('.', '_'))
    if (-not $toolchain) { $toolchain = $GlobalLinuxToolchain }
    if ($toolchain) { $env:LINUX_MULTIARCH_ROOT = $toolchain }
    if ($TargetPlatforms) { return $TargetPlatforms }
    if ($toolchain) { return 'Win64+Linux' }
    return 'Win64'
}

function Find-EngineRoot([string] $Version) {
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($EngineRoot) { $candidates.Add($EngineRoot) }
    if ($EngineRoots.ContainsKey($Version)) { $candidates.Add([string]$EngineRoots[$Version]) }
    $envName = 'UE_' + $Version.Replace('.', '_') + '_ROOT'
    $envValue = [Environment]::GetEnvironmentVariable($envName)
    if ($envValue) { $candidates.Add($envValue) }
    # Launcher installs register themselves here (Windows only).
    foreach ($hive in @('HKLM:\SOFTWARE\EpicGames\Unreal Engine', 'HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine')) {
        try {
            $key = "$hive\$Version"
            if (Test-Path $key) {
                $dir = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).InstalledDirectory
                if ($dir) { $candidates.Add($dir) }
            }
        } catch { }
    }
    if (${env:ProgramFiles}) {
        $candidates.Add((Join-Path ${env:ProgramFiles} "Epic Games\UE_$Version"))
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'Engine\Build\BatchFiles\RunUAT.bat'))) {
            return (Resolve-Path $candidate).Path
        }
    }
    return $null
}

function Test-EngineVersion([string] $Root, [string] $Version) {
    $buildVersion = Join-Path $Root 'Engine\Build\Build.version'
    if (-not (Test-Path $buildVersion)) { return "missing Engine\Build\Build.version" }
    $info = Get-Content -Raw $buildVersion | ConvertFrom-Json
    $found = "$($info.MajorVersion).$($info.MinorVersion)"
    if ($found -ne $Version) { return "engine at $Root is $found, not $Version" }
    return $null
}

function Copy-Filtered([string] $Source, [string] $Destination) {
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        if ($item.PSIsContainer) {
            if ($ExcludedNames -contains $item.Name) { continue }
            Copy-Filtered $item.FullName (Join-Path $Destination $item.Name)
        } else {
            if ($ExcludedFiles -contains $item.Name) { continue }
            Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $Destination $item.Name) -Force
        }
    }
}

function Set-EngineVersion([string] $UpluginPath, [string] $Version) {
    # Text edit, not ConvertTo-Json, so the descriptor keeps its formatting and field order.
    $text = [IO.File]::ReadAllText($UpluginPath)
    $value = "$Version.0"
    if ($text -match '"EngineVersion"\s*:') {
        $text = [regex]::Replace($text, '"EngineVersion"\s*:\s*"[^"]*"', "`"EngineVersion`": `"$value`"")
    } else {
        $text = [regex]::Replace($text, '("VersionName"\s*:\s*"[^"]*",)(\r?\n)(\s*)', "`$1`$2`$3`"EngineVersion`": `"$value`",`$2`$3", 1)
    }
    if ($text -notmatch [regex]::Escape("`"EngineVersion`": `"$value`"")) { throw "Could not set EngineVersion in $UpluginPath" }
    [IO.File]::WriteAllText($UpluginPath, $text, (New-Object System.Text.UTF8Encoding($false)))
    $null = $text | ConvertFrom-Json  # still valid JSON
}

function New-FabZip([string] $StagedPlugin, [string] $ZipPath) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
    $zip = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $base = (Resolve-Path $StagedPlugin).Path.TrimEnd('\') + '\'
        foreach ($file in Get-ChildItem -LiteralPath $StagedPlugin -Recurse -File -Force) {
            # Forward slashes so the archive extracts correctly on Mac and Linux too.
            $entry = $PluginName + '/' + $file.FullName.Substring($base.Length).Replace('\', '/')
            $null = [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $entry, [System.IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $zip.Dispose()
    }
    # Double-check nothing excluded slipped in.
    $check = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        foreach ($entry in $check.Entries) {
            $parts = $entry.FullName.Split('/')
            foreach ($part in $parts) {
                if ($ExcludedNames -contains $part) { throw "Zip contains excluded path $($entry.FullName)" }
            }
        }
        return $check.Entries.Count
    } finally {
        $check.Dispose()
    }
}

function Find-Python {
    foreach ($name in @('python3', 'python', 'py')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    return $null
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$Python = Find-Python
$Results = New-Object System.Collections.Generic.List[object]

foreach ($Version in $EngineVersions) {
    $result = [ordered]@{
        Version = $Version; Engine = '-'; Platforms = (Get-Platforms $Version); Readiness = '-'; Build = '-'
        Warnings = 0; Errors = 0; Zip = '-'; Ok = $false; Note = ''
    }
    Write-Host ""
    Write-Host "=== $PluginName $VersionName for UE $Version ($($result.Platforms)) ===" -ForegroundColor Cyan
    try {
        $root = Find-EngineRoot $Version
        if (-not $root) { throw "UE $Version not found (pass -EngineRoots @{'$Version'='<dir>'} or set UE_$($Version.Replace('.', '_'))_ROOT)" }
        $mismatch = Test-EngineVersion $root $Version
        if ($mismatch) { throw $mismatch }
        $result.Engine = $root

        $versionDir = Join-Path $OutputDir $Version
        $staging = Join-Path $OutputDir "Staging\$Version\$PluginName"
        $packageDir = Join-Path $versionDir 'Package'
        $logPath = Join-Path $versionDir 'BuildPlugin.log'
        foreach ($dir in @($staging, $packageDir)) { if (Test-Path $dir) { Remove-Item -Recurse -Force $dir } }
        New-Item -ItemType Directory -Force -Path $versionDir, $staging | Out-Null

        # Stage
        foreach ($folder in $ShippedFolders) {
            $src = Join-Path $PluginRoot $folder
            if (Test-Path $src) { Copy-Filtered $src (Join-Path $staging $folder) }
        }
        if ($ContentRoot) {
            $override = Join-Path $ContentRoot $Version
            if (-not (Test-Path $override)) { throw "-ContentRoot given but $override does not exist" }
            if (Test-Path (Join-Path $staging 'Content')) { Remove-Item -Recurse -Force (Join-Path $staging 'Content') }
            Copy-Filtered $override (Join-Path $staging 'Content')
            $result.Note = "content from $override"
        }
        $stagedUplugin = Join-Path $staging $Descriptor.Name
        Copy-Item -LiteralPath $Descriptor.FullName -Destination $stagedUplugin
        Set-EngineVersion $stagedUplugin $Version

        # Static checks
        if ($SkipReadiness) {
            $result.Readiness = 'skipped'
        } elseif (-not $Python) {
            $result.Readiness = 'no python'
        } else {
            $readinessLog = Join-Path $versionDir 'FabReadiness.txt'
            $ErrorActionPreference = 'Continue'
            & $Python (Join-Path $PSScriptRoot 'CheckFabReadiness.py') --root $staging --engine-version $Version --max-content-engine $Version `
                --only 'copyright,descriptor,content,content-version,icon,hygiene,buildcs' *> $readinessLog
            $ErrorActionPreference = 'Stop'
            $result.Readiness = if ($LASTEXITCODE -eq 0) { 'pass' } else { 'FAIL' }
            if ($LASTEXITCODE -ne 0) { Write-Host "Fab readiness failed; see $readinessLog" -ForegroundColor Yellow }
        }

        # Build
        if ($SkipBuild) {
            $result.Build = 'skipped'
        } else {
            $uat = Join-Path $root 'Engine\Build\BatchFiles\RunUAT.bat'
            $uatArgs = @('BuildPlugin', "-Plugin=$stagedUplugin", "-Package=$packageDir", "-TargetPlatforms=$($result.Platforms)", '-StrictIncludes', '-NoP4')
            Write-Host "$uat $($uatArgs -join ' ')"
            # 'Continue' while the native tool runs: Windows PowerShell 5.1 turns redirected stderr lines into
            # terminating errors under 'Stop'.
            $ErrorActionPreference = 'Continue'
            & $uat @uatArgs 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $logPath | Out-Host
            $exitCode = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            $log = Get-Content -LiteralPath $logPath
            $warnings = @($log | Select-String -Pattern $WarningPattern | ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique)
            $errors = @($log | Select-String -Pattern $ErrorPattern | ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique)
            $result.Warnings = $warnings.Count
            $result.Errors = $errors.Count
            if ($warnings.Count) { $warnings | Set-Content (Join-Path $versionDir 'CompilerWarnings.txt') }
            if ($errors.Count) { $errors | Set-Content (Join-Path $versionDir 'CompilerErrors.txt') }
            if ($exitCode -ne 0) { $result.Build = "FAIL (UAT $exitCode)" }
            elseif ($errors.Count) { $result.Build = 'FAIL (errors)' }
            elseif ($warnings.Count) { $result.Build = 'FAIL (warnings)' }
            else { $result.Build = 'pass' }
        }

        # Zip (only when the build passed or was skipped on purpose)
        if ($result.Build -eq 'pass' -or $result.Build -eq 'skipped') {
            $zipPath = Join-Path $OutputDir ("{0}_{1}_UE{2}.zip" -f $PluginName, $VersionName, $Version)
            $count = New-FabZip $staging $zipPath
            $result.Zip = "$zipPath ($count files)"
        }
        $result.Ok = ($result.Build -eq 'pass' -or $result.Build -eq 'skipped') -and ($result.Readiness -ne 'FAIL')
        if (-not $KeepStaging -and $result.Ok) { Remove-Item -Recurse -Force (Split-Path $staging -Parent) }
    } catch {
        $result.Note = $_.Exception.Message
        $result.Build = if ($result.Build -eq '-') { 'not run' } else { $result.Build }
        Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    }
    $Results.Add([pscustomobject]$result)
}

Write-Host ""
Write-Host "=== Summary ($PluginName $VersionName) ===" -ForegroundColor Cyan
Write-Host ($Results | Format-Table -AutoSize -Wrap Version, Engine, Platforms, Readiness, Build, Warnings, Errors, Zip, Note | Out-String -Width 400)
$failed = @($Results | Where-Object { -not $_.Ok })
if ($failed.Count) {
    Write-Host "$($failed.Count) of $($Results.Count) version(s) are not ready. Logs: $OutputDir\<version>\" -ForegroundColor Red
    exit 1
}
Write-Host "All versions built cleanly. Upload the zips in $OutputDir to Fab." -ForegroundColor Green
exit 0
