<#
.SYNOPSIS
  Builds the PokePark Wii PC port from the player's own disc image.

.DESCRIPTION
  Steps (each one is skipped when already done, unless -Clean):
    toolchain  download the compiler toolchain (LLVM-MinGW, CMake, Ninja, .NET 8 SDK) into .build\toolchain
    deps       download the pinned third-party sources (SDL, Dawn, ...) into .build\deps
    disc       validate and extract the disc image (ISO/RVZ/WBFS/...) into game\ with nodtool
    translate  translate the game's main.dol into C++ (src\generated)
    build      compile the runtime and the translated game (.build\native)
    install    assemble the playable folder PokePark\ (executable, DLLs, fonts, settings)

  Nothing from the game is shipped with this repository: the disc image is read on this machine only.
  The launcher (PokeParkLauncher.ps1) runs this script and parses its "##" progress lines.

.EXAMPLE
  .\Build-PokePark.ps1 -Disc "D:\Games\PokePark Wii (USA).rvz"
.EXAMPLE
  .\Build-PokePark.ps1 -ExtractedDir "D:\Games\PokePark_extracted"   # a folder with sys\main.dol and files\
#>
[CmdletBinding()]
param(
    [string]$Disc,
    [string]$ExtractedDir,
    # Reuse an existing toolchain folder (llvm-mingw-*\bin, cmake-*\bin, ninja.exe, dotnet\dotnet.exe).
    [string]$ToolchainDir,
    [ValidateSet('all', 'toolchain', 'deps', 'disc', 'translate', 'build', 'install')]
    [string]$Step = 'all',
    [int]$Jobs = 0,
    [switch]$Clean,
    # Also append every output line to this file (UTF-8, no BOM). The launcher follows it.
    [string]$LogFile
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# --- Pinned facts ----------------------------------------------------------------------------------
$GameId = 'R8AE01'
$DolSha256 = 'fc4da6227007b0492c6b2ebe24939c5bd530e129572d8d1f61b3f6c9fe9acc84'   # PokePark Wii (USA) Rev 0
$LlvmMingwVersion = '20261006'
$CMakeVersion = '4.4.4'
$NinjaVersion = '1.13.2'
$DotnetVersion = '8.0.428'
$NodToolVersion = 'v2.0.0-alpha.10'
$Urls = @{
    LlvmMingw = "https://github.com/mstorsjo/llvm-mingw/releases/download/$LlvmMingwVersion/llvm-mingw-$LlvmMingwVersion-ucrt-x86_64.zip"
    CMake     = "https://github.com/Kitware/CMake/releases/download/v$CMakeVersion/cmake-$CMakeVersion-windows-x86_64.zip"
    Ninja     = "https://github.com/ninja-build/ninja/releases/download/v$NinjaVersion/ninja-win.zip"
    Dotnet    = 'https://dot.net/v1/dotnet-install.ps1'
    NodTool   = "https://github.com/encounter/nod/releases/download/$NodToolVersion/nodtool-windows-x86_64.exe"
}

# --- Layout ----------------------------------------------------------------------------------------
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Src = Join-Path $Root 'src'
$BuildRoot = Join-Path $Root '.build'
$StateDir = Join-Path $BuildRoot 'state'
$Downloads = Join-Path $BuildRoot 'downloads'
$GameDir = Join-Path $Root 'game'
$InstallDir = Join-Path $Root 'PokePark'
$NativeDir = Join-Path $BuildRoot 'native'
$DepsDir = Join-Path $BuildRoot 'deps'
if (-not $ToolchainDir) { $ToolchainDir = Join-Path $BuildRoot 'toolchain' }
$ToolchainDir = [IO.Path]::GetFullPath($ToolchainDir)
if ($Jobs -le 0) { $Jobs = [Math]::Max(1, [Environment]::ProcessorCount) }
foreach ($dir in @($BuildRoot, $StateDir, $Downloads)) { [IO.Directory]::CreateDirectory($dir) | Out-Null }

# --- Output protocol (read by the launcher) ---------------------------------------------------------
function Step([string]$Id, [string]$Text) { Write-Output "##STEP $Id $Text" }
function Done([string]$Id) { Write-Output "##DONE $Id" }
function Info([string]$Text) { Write-Output $Text }
$script:lastProgress = -1
function Progress([int]$Percent) {
    if ($Percent -eq $script:lastProgress) { return }
    $script:lastProgress = $Percent
    Write-Output "##PROGRESS $Percent"
}
# UTF-8 without a byte-order mark (Windows PowerShell 5.1's Set-Content -Encoding UTF8 always writes one).
function Write-TextFile([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

function Test-StepDone([string]$Id) { (-not $Clean) -and (Test-Path -LiteralPath (Join-Path $StateDir "$Id.ok")) }
function Set-StepDone([string]$Id) { Set-Content -LiteralPath (Join-Path $StateDir "$Id.ok") -Value (Get-Date -Format o) }
function Reset-StepsFrom([string[]]$Ids) {
    foreach ($id in $Ids) { Remove-Item -LiteralPath (Join-Path $StateDir "$id.ok") -ErrorAction SilentlyContinue }
}

# Windows PowerShell 5.1 turns every stderr line of a native program into an ErrorRecord, which
# $ErrorActionPreference = 'Stop' would treat as fatal: run external tools with 'Continue' and judge
# them by their exit code only.
function Invoke-Native([scriptblock]$Command) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Command 2>&1 | ForEach-Object { "$_" } } finally { $ErrorActionPreference = $previous }
}

function Invoke-Tool([string]$File, [string[]]$Arguments, [string]$What) {
    Info "> $What"
    Invoke-Native { & $File @Arguments }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)." }
}

function Get-Download([string]$Url, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) { return }
    Info "Downloading $Url"
    $temporary = "$Destination.part"
    Invoke-WebRequest -Uri $Url -OutFile $temporary -UseBasicParsing
    Move-Item -LiteralPath $temporary -Destination $Destination -Force
}

function Expand-Zip([string]$Zip, [string]$Destination) {
    Info "Extracting $(Split-Path -Leaf $Zip)"
    Expand-Archive -LiteralPath $Zip -DestinationPath $Destination -Force
}

# --- Toolchain ---------------------------------------------------------------------------------------
function Get-Toolchain {
    $llvm = Get-ChildItem -LiteralPath $ToolchainDir -Directory -Filter 'llvm-mingw-*' -ErrorAction SilentlyContinue | Select-Object -First 1
    $cmake = Get-ChildItem -LiteralPath $ToolchainDir -Directory -Filter 'cmake-*' -ErrorAction SilentlyContinue | Select-Object -First 1
    $result = [pscustomobject]@{
        LlvmBin = $(if ($llvm) { Join-Path $llvm.FullName 'bin' } else { $null })
        CMake   = $(if ($cmake) { Join-Path $cmake.FullName 'bin\cmake.exe' } else { $null })
        Ninja   = Join-Path $ToolchainDir 'ninja.exe'
        Dotnet  = Join-Path $ToolchainDir 'dotnet\dotnet.exe'
    }
    return $result
}

function Test-Toolchain($tc) {
    return ($tc.LlvmBin -and (Test-Path (Join-Path $tc.LlvmBin 'x86_64-w64-mingw32-clang++.exe')) -and
            $tc.CMake -and (Test-Path $tc.CMake) -and (Test-Path $tc.Ninja) -and (Test-Path $tc.Dotnet))
}

function Invoke-ToolchainStep {
    Step 'toolchain' 'Preparing the compiler toolchain'
    [IO.Directory]::CreateDirectory($ToolchainDir) | Out-Null
    $tc = Get-Toolchain
    if (-not ($tc.LlvmBin -and (Test-Path (Join-Path $tc.LlvmBin 'x86_64-w64-mingw32-clang++.exe')))) {
        $zip = Join-Path $Downloads "llvm-mingw-$LlvmMingwVersion.zip"
        Get-Download $Urls.LlvmMingw $zip
        Expand-Zip $zip $ToolchainDir
    }
    Progress 4
    if (-not ($tc.CMake -and (Test-Path $tc.CMake))) {
        $zip = Join-Path $Downloads "cmake-$CMakeVersion.zip"
        Get-Download $Urls.CMake $zip
        Expand-Zip $zip $ToolchainDir
    }
    if (-not (Test-Path $tc.Ninja)) {
        $zip = Join-Path $Downloads "ninja-$NinjaVersion.zip"
        Get-Download $Urls.Ninja $zip
        Expand-Zip $zip $ToolchainDir
    }
    Progress 6
    if (-not (Test-Path $tc.Dotnet)) {
        $script = Join-Path $Downloads 'dotnet-install.ps1'
        Get-Download $Urls.Dotnet $script
        Info "Installing the .NET $DotnetVersion SDK (portable, inside this folder)"
        Invoke-Native { & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Version $DotnetVersion -InstallDir (Join-Path $ToolchainDir 'dotnet') -NoPath }
    }
    $tc = Get-Toolchain
    if (-not (Test-Toolchain $tc)) { throw "The toolchain in $ToolchainDir is incomplete." }
    Progress 8
    Done 'toolchain'
}

function Use-Toolchain {
    $tc = Get-Toolchain
    if (-not (Test-Toolchain $tc)) { throw "The toolchain is missing; run the 'toolchain' step first." }
    $env:PATH = "$($tc.LlvmBin);$(Split-Path -Parent $tc.CMake);$ToolchainDir;$(Split-Path -Parent $tc.Dotnet);$env:PATH"
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $env:DOTNET_NOLOGO = '1'
    $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
    return $tc
}

# --- Third-party sources --------------------------------------------------------------------------
function Invoke-DepsStep {
    Step 'deps' 'Downloading third-party libraries (SDL, Dawn, ...)'
    Use-Toolchain | Out-Null
    Invoke-Native { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Src 'Launcher\Prepare-Dependencies.ps1') -Destination $DepsDir }
    if ($LASTEXITCODE -ne 0) { throw "Preparing the third-party libraries failed (exit code $LASTEXITCODE)." }
    Progress 12
    Done 'deps'
}

# --- Disc --------------------------------------------------------------------------------------------
function Get-FileSha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# Removes game\. When it is a junction to the player's own extracted folder, only the link is removed:
# a recursive delete would follow it and erase the player's files.
function Remove-GameDir {
    if (-not (Test-Path -LiteralPath $GameDir)) { return }
    $item = Get-Item -LiteralPath $GameDir -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        [IO.Directory]::Delete($GameDir, $false)
    } else {
        Remove-Item -LiteralPath $GameDir -Recurse -Force
    }
}

function Invoke-DiscStep {
    Step 'disc' 'Reading your PokePark disc image'
    if ($ExtractedDir) {
        $source = [IO.Path]::GetFullPath($ExtractedDir)
        if (-not (Test-Path (Join-Path $source 'sys\main.dol'))) { throw "No sys\main.dol in $source." }
        Remove-GameDir
        New-Item -ItemType Junction -Path $GameDir -Target $source | Out-Null
        Info "Using the extracted folder $source"
    } else {
        if (-not $Disc) { throw 'Choose your PokePark disc image (ISO, RVZ, WBFS, ...).' }
        $image = [IO.Path]::GetFullPath($Disc)
        if (-not (Test-Path -LiteralPath $image)) { throw "Disc image not found: $image" }
        $nodtool = Join-Path $BuildRoot 'tools\nodtool.exe'
        if (-not (Test-Path $nodtool)) {
            [IO.Directory]::CreateDirectory((Split-Path -Parent $nodtool)) | Out-Null
            Get-Download $Urls.NodTool $nodtool
        }
        $info = Invoke-Native { & $nodtool info $image } | Out-String
        $match = [regex]::Match($info, '(?m)^Game ID: (\S+)')
        if (-not $match.Success) { throw "This file is not a readable Wii disc image.`n$info" }
        if ($match.Groups[1].Value -ne $GameId) {
            throw "This disc is $($match.Groups[1].Value); the port needs PokePark Wii: Pikachu's Adventure (USA, $GameId)."
        }
        Info "Disc: PokePark Wii (USA) $GameId"
        Remove-GameDir
        Info 'Extracting the disc (a few minutes)...'
        Invoke-Tool $nodtool @('extract', $image, $GameDir, '-q') 'nodtool extract'
    }
    $dol = Join-Path $GameDir 'sys\main.dol'
    if (-not (Test-Path $dol)) { throw "The extracted disc has no sys\main.dol." }
    $hash = Get-FileSha256 $dol
    if ($hash -ne $DolSha256) {
        throw "Unsupported game version (main.dol SHA-256 $hash). The port supports PokePark Wii (USA, Rev 0)."
    }
    Info 'Game version verified (USA, Rev 0).'
    $sourceRecord = $(if ($ExtractedDir) { 'dir:' + [IO.Path]::GetFullPath($ExtractedDir) } else { 'img:' + [IO.Path]::GetFullPath($Disc) })
    Write-TextFile (Join-Path $StateDir 'disc_source.txt') $sourceRecord
    Reset-StepsFrom @('translate', 'build', 'install')
    Progress 20
    Done 'disc'
}

# --- Translate ---------------------------------------------------------------------------------------
function Invoke-TranslateStep {
    Step 'translate' 'Translating the game code to C++'
    $tc = Use-Toolchain
    $cli = Join-Path $Src 'translator\src\Translator.Cli\Translator.Cli.csproj'
    Invoke-Tool $tc.Dotnet @('build', $cli, '-c', 'Release', '-v', 'quiet', '-nologo') 'Building the translator'
    $dll = Join-Path $Src 'translator\src\Translator.Cli\bin\Release\net8.0\Translator.Cli.dll'
    Progress 26
    $project = 'projects/pokepark/recomp.yml'
    Push-Location $Src
    try {
        Invoke-Tool $tc.Dotnet @($dll, 'translate-recursive', '0x80004050', '--project', $project, '--outdir', 'generated/functions',
            '--output-metadata', 'generated/base_translation_output.json', '--production-source-bundle', 'generated/base_translation_sources.bin',
            '--no-function-files', '--prune-stale') 'Translating main.dol'
        Progress 34
        Invoke-Tool $tc.Dotnet @($dll, 'generate-data-init', '--project', $project) 'Generating the data initializer'
        Invoke-Tool $tc.Dotnet @($dll, 'emit-build-shards', '--project', $project, '--base-metadata', 'generated/base_translation_output.json',
            '--base-functions-dir', 'generated/functions', '--native-source-dir', 'runtime/src', '--out', 'generated/build_shards') 'Splitting the build'
    } finally {
        Pop-Location
    }
    Reset-StepsFrom @('build', 'install')
    Progress 38
    Done 'translate'
}

# --- Build -------------------------------------------------------------------------------------------
function Invoke-BuildStep {
    Step 'build' 'Compiling the PC port (the longest step)'
    $tc = Use-Toolchain
    . (Join-Path $Src 'Launcher\NativeBuildFlags.ps1')
    if (-not (Test-Path (Join-Path $NativeDir 'build.ninja'))) {
        $arguments = Get-MkwNativeConfigureArguments -SourceDirectory (Join-Path $Src 'runtime') -BuildDirectory $NativeDir `
            -Ninja $tc.Ninja -CCompiler (Join-Path $tc.LlvmBin 'x86_64-w64-mingw32-clang.exe') `
            -CxxCompiler (Join-Path $tc.LlvmBin 'x86_64-w64-mingw32-clang++.exe') `
            -ResourceCompiler (Join-Path $tc.LlvmBin 'x86_64-w64-mingw32-windres.exe') -DependenciesDirectory $DepsDir
        Invoke-Tool $tc.CMake $arguments 'Configuring the build'
    }
    Progress 42
    Info "> Compiling with $Jobs parallel jobs"
    # Ninja prints "[done/total]"; map it onto 42..95 % for the launcher.
    Invoke-Native { & $tc.CMake --build $NativeDir --target WiiCompiled --parallel $Jobs } | ForEach-Object {
        $line = "$_"
        $m = [regex]::Match($line, '^\[(\d+)/(\d+)\]')
        if ($m.Success) {
            $done = [int]$m.Groups[1].Value; $total = [int]$m.Groups[2].Value
            if ($total -gt 0) { Progress (42 + [int](53 * $done / $total)) }
            if (($done % 50) -eq 0 -or $done -eq $total) { Info "Compiled $done / $total" }
        } elseif ($line -match 'error|FAILED|warning: unused') {
            Info $line
        }
    }
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed (exit code $LASTEXITCODE). See the log above." }
    Reset-StepsFrom @('install')
    Progress 95
    Done 'build'
}

# --- Install -----------------------------------------------------------------------------------------
function Write-DefaultConfig([string]$ConfigPath) {
    $dvdRoot = ($GameDir -replace '\\', '/')
    if (Test-Path -LiteralPath $ConfigPath) {
        # Keep the player's settings; only point the game data at this installation.
        $text = Get-Content -LiteralPath $ConfigPath -Raw
        if ($text -match '(?m)^dvd_root\s*=') {
            $text = [regex]::Replace($text, '(?m)^dvd_root\s*=.*$', "dvd_root = `"$dvdRoot`"")
        } else {
            $text = $text.TrimEnd() + "`r`n`r`n[paths]`r`ndvd_root = `"$dvdRoot`"`r`n"
        }
        Write-TextFile $ConfigPath $text
        return
    }
    $config = @"
[network]
enabled = false

[discord]
enabled = false

[paths]
dvd_root = "$dvdRoot"

[video]
native_60fps = true
force_aspect = true
resolution_multiplier = 0
frame_interpolation_fps = 0
"@
    Write-TextFile $ConfigPath ($config -replace "`r?`n", "`r`n")
}

function Invoke-InstallStep {
    Step 'install' 'Assembling the game folder'
    [IO.Directory]::CreateDirectory($InstallDir) | Out-Null
    $exe = Join-Path $NativeDir 'WiiCompiled.exe'
    if (-not (Test-Path $exe)) { throw 'The executable was not built.' }
    # Stage everything first, then rename each file over the old one: a failure (for example the game
    # still running) leaves the previous, working PokePark.exe untouched.
    $staging = Join-Path $InstallDir '.staging'
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    [IO.Directory]::CreateDirectory($staging) | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $staging 'PokePark.exe') -Force
    Get-ChildItem -LiteralPath $NativeDir -Filter '*.dll' | Copy-Item -Destination $staging -Force
    # Audio DSP coefficient ROM (Dolphin's free replacement) and aurora's initial pipeline cache.
    foreach ($file in @('dsp_coef.bin', 'initial_pipeline_cache.db')) {
        $path = Join-Path $NativeDir $file
        if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $staging -Force }
    }
    if (-not (Test-Path (Join-Path $staging 'dsp_coef.bin'))) {
        Copy-Item -LiteralPath (Join-Path $Src 'runtime\assets\dsp\dsp_coef.bin') -Destination $staging -Force
    }
    $running = Get-Process -Name 'PokePark' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) }
    if ($running) { throw 'PokePark is running. Close the game and try again.' }
    foreach ($file in Get-ChildItem -LiteralPath $staging -File) {
        Move-Item -LiteralPath $file.FullName -Destination (Join-Path $InstallDir $file.Name) -Force
    }
    Remove-Item -LiteralPath $staging -Recurse -Force
    # Copy folder contents (copying a folder onto an existing one would nest it: fonts\fonts).
    $folders = @(@{ From = (Join-Path $NativeDir 'wii_bootstrap'); To = (Join-Path $InstallDir 'wii_bootstrap') },
                 @{ From = (Join-Path $Root 'fonts'); To = (Join-Path $InstallDir 'fonts') })
    foreach ($folder in $folders) {
        if (-not (Test-Path -LiteralPath $folder.From)) { continue }
        [IO.Directory]::CreateDirectory($folder.To) | Out-Null
        Copy-Item -Path (Join-Path $folder.From '*') -Destination $folder.To -Recurse -Force
    }
    Set-Content -LiteralPath (Join-Path $InstallDir 'portable.txt') -Value 'Settings, saves and caches live in UserData next to this file.'
    $userData = Join-Path $InstallDir 'UserData'
    [IO.Directory]::CreateDirectory($userData) | Out-Null
    Write-DefaultConfig (Join-Path $userData 'Config.toml')
    # Versions up to 0.1.2 merged Mario Kart Wii's 1101-entry shader seed into the player's pipeline cache, which
    # then compiled in the background at every start. Drop that cache once (only shader descriptions: PokePark's own
    # seed refills it); saves and settings are elsewhere and untouched.
    $cacheDir = Join-Path $userData 'Cache'
    $seedMarker = Join-Path $cacheDir 'pokeport-seed.txt'
    if ((Test-Path -LiteralPath $cacheDir) -and -not (Test-Path -LiteralPath $seedMarker)) {
        Get-ChildItem -LiteralPath $cacheDir -Filter 'pipeline_cache.db*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
        Info 'Shader cache reset (old Mario Kart Wii seed removed).'
    }
    [IO.Directory]::CreateDirectory($cacheDir) | Out-Null
    Write-TextFile $seedMarker 'pokepark-r8ae01 seed v1'
    $versionFile = Join-Path $Root 'VERSION'
    if (Test-Path -LiteralPath $versionFile) {
        Write-TextFile (Join-Path $StateDir 'built_version.txt') ((Get-Content -LiteralPath $versionFile -Raw).Trim())
    }
    Progress 100
    Done 'install'
}

# --- Main --------------------------------------------------------------------------------------------
$script:exitCode = 0
$main = {
    $steps = @('toolchain', 'deps', 'disc', 'translate', 'build', 'install')
    try {
        if ($Clean) { Reset-StepsFrom $steps }
        # Sources of another version (an update, or files copied by hand): translate, build and install again.
        $versionFile = Join-Path $Root 'VERSION'
        $currentVersion = $(if (Test-Path -LiteralPath $versionFile) { (Get-Content -LiteralPath $versionFile -Raw).Trim() } else { '' })
        $builtFile = Join-Path $StateDir 'built_version.txt'
        $builtVersion = $(if (Test-Path -LiteralPath $builtFile) { (Get-Content -LiteralPath $builtFile -Raw).Trim() } else { '' })
        if ($currentVersion -ne $builtVersion) {
            Reset-StepsFrom @('translate', 'build', 'install')
            Info "Version $currentVersion differs from the built version '$builtVersion': rebuilding."
        }
        # A different image (or a missing extraction) is read again; the same one is not re-extracted.
        $source = $(if ($ExtractedDir) { 'dir:' + [IO.Path]::GetFullPath($ExtractedDir) } elseif ($Disc) { 'img:' + [IO.Path]::GetFullPath($Disc) } else { $null })
        $recorded = Join-Path $StateDir 'disc_source.txt'
        $previous = $(if (Test-Path -LiteralPath $recorded) { (Get-Content -LiteralPath $recorded -Raw).Trim() } else { $null })
        if ($source -and ($source -ne $previous -or -not (Test-Path (Join-Path $GameDir 'sys\main.dol')))) {
            Reset-StepsFrom @('disc')
        }
        foreach ($id in $steps) {
            if ($Step -ne 'all' -and $Step -ne $id) { continue }
            if ($Step -eq 'all' -and (Test-StepDone $id)) { Info "Step '$id' already done."; Done $id; continue }
            switch ($id) {
                'toolchain' { Invoke-ToolchainStep }
                'deps'      { Invoke-DepsStep }
                'disc'      { Invoke-DiscStep }
                'translate' { Invoke-TranslateStep }
                'build'     { Invoke-BuildStep }
                'install'   { Invoke-InstallStep }
            }
            Set-StepDone $id
        }
        Write-Output '##FINISHED'
    } catch {
        Write-Output "##ERROR $($_.Exception.Message)"
        $script:exitCode = 1
    }
}
if ($LogFile) {
    $logEncoding = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($LogFile, "PokePort build log $(Get-Date -Format o)`r`n", $logEncoding)
    & $main *>&1 | ForEach-Object {
        $line = "$_"
        [IO.File]::AppendAllText($LogFile, $line + "`r`n", $logEncoding)
        $line
    }
} else {
    & $main
}
exit $script:exitCode
