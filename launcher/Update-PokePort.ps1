<#
.SYNOPSIS
  Updates a PokePort folder to a newer release, safely.

.DESCRIPTION
  Run by the launcher from a temporary copy (so it never replaces itself while running):
    1. download the release ZIP (and check its SHA-256 when GitHub publishes one);
    2. extract it to .build\updates and check it really is a PokePort release of the expected version;
    3. back up the port's own files (launcher, src, fonts, docs, root files) to .build\backup;
    4. replace them; on any failure restore the backup, so the folder is never left half-updated;
    5. keep the modification time of unchanged files (so the next build only recompiles what changed)
       and invalidate only the build steps the update affects.
  Never touched: PokePark\ (installed game, saves, settings), game\ (extracted disc), .build\toolchain,
  .build\deps, .build\native. The installed PokePark.exe keeps working until a new build succeeds.
  Output protocol as Build-PokePark.ps1: ##STEP / ##PROGRESS / ##ERROR / ##FINISHED.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Root,
    [Parameter(Mandatory)] [string]$Tag,          # e.g. v0.1.2
    [Parameter(Mandatory)] [string]$AssetUrl,
    [string]$Sha256,                              # lower-case hex, optional
    [string]$LogFile
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root = [IO.Path]::GetFullPath($Root)
$BuildRoot = Join-Path $Root '.build'
$UpdatesDir = Join-Path $BuildRoot 'updates'
$StateDir = Join-Path $BuildRoot 'state'
# What a release owns. Everything else in the folder belongs to the player.
$ManagedDirs = @('launcher', 'src', 'fonts', 'docs', 'tools')
$ManagedFiles = @('README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'PokeParkLauncher.bat', 'VERSION', '.gitignore')
# Inside src\: kept across updates (the translator refreshes it; keeping it avoids a full rebuild).
$PreservedInSrc = @('generated')

function Out-Line([string]$Text) {
    if ($LogFile) { [IO.File]::AppendAllText($LogFile, $Text + "`r`n", (New-Object Text.UTF8Encoding($false))) }
    Write-Output $Text
}
function Step([string]$Text) { Out-Line "##STEP update $Text" }
function Progress([int]$Percent) { Out-Line "##PROGRESS $Percent" }

function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

function Get-FileIndex([string]$Base, [string[]]$Dirs, [string[]]$Files) {
    # relative path -> @{ Hash; Time }
    $index = @{}
    foreach ($dir in $Dirs) {
        $full = Join-Path $Base $dir
        if (-not (Test-Path -LiteralPath $full)) { continue }
        Get-ChildItem -LiteralPath $full -Recurse -File -Force | ForEach-Object {
            $rel = $_.FullName.Substring($Base.Length).TrimStart('\')
            if ($rel -like 'src\generated\*' -or $rel -match '\\(bin|obj)\\') { return }
            $index[$rel.ToLowerInvariant()] = @{ Rel = $rel; Hash = (Get-Sha256 $_.FullName); Time = $_.LastWriteTimeUtc }
        }
    }
    foreach ($file in $Files) {
        $full = Join-Path $Base $file
        if (Test-Path -LiteralPath $full) {
            $item = Get-Item -LiteralPath $full -Force
            $index[$file.ToLowerInvariant()] = @{ Rel = $file; Hash = (Get-Sha256 $full); Time = $item.LastWriteTimeUtc }
        }
    }
    return $index
}

function Copy-Tree([string]$From, [string]$To) {
    if (-not (Test-Path -LiteralPath $From)) { return }
    [IO.Directory]::CreateDirectory($To) | Out-Null
    # Generated code and build output are never part of a release or a backup.
    & robocopy.exe $From $To /E /COPY:DT /R:2 /W:1 /NFL /NDL /NJH /NJS /NP /XD generated bin obj | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying $From failed (robocopy $LASTEXITCODE)." }
}

function Remove-Managed([string]$Base) {
    $failures = New-Object Collections.Generic.List[string]
    $targets = New-Object Collections.Generic.List[string]
    foreach ($dir in $ManagedDirs) {
        $full = Join-Path $Base $dir
        if (-not (Test-Path -LiteralPath $full)) { continue }
        if ($dir -eq 'src') {
            Get-ChildItem -LiteralPath $full -Force | Where-Object { $PreservedInSrc -notcontains $_.Name } |
                ForEach-Object { $targets.Add($_.FullName) }
        } else {
            $targets.Add($full)
        }
    }
    foreach ($file in $ManagedFiles) {
        $full = Join-Path $Base $file
        if (Test-Path -LiteralPath $full) { $targets.Add($full) }
    }
    foreach ($target in $targets) {
        try { Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop }
        catch { $failures.Add($_.Exception.Message) }
    }
    if ($failures.Count -gt 0) { throw ($failures[0] + $(if ($failures.Count -gt 1) { " (+$($failures.Count - 1) more)" } else { '' })) }
}

function Copy-Managed([string]$From, [string]$To) {
    foreach ($dir in $ManagedDirs) { Copy-Tree (Join-Path $From $dir) (Join-Path $To $dir) }
    foreach ($file in $ManagedFiles) {
        $source = Join-Path $From $file
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $To $file) -Force }
    }
}

# Makes the managed files of $To identical to $From, file by file: identical files are not touched (no
# rewrite, no lock conflict, timestamps kept so the next build only recompiles what changed), changed files
# are overwritten and dated now, files that no longer exist are deleted. Returns the number of changed files.
function Sync-Managed([string]$From, [string]$To) {
    $source = Get-FileIndex $From $ManagedDirs $ManagedFiles
    $target = Get-FileIndex $To $ManagedDirs $ManagedFiles
    $now = (Get-Date).ToUniversalTime()
    $changed = 0
    foreach ($key in $source.Keys) {
        $entry = $source[$key]
        $current = $target[$key]
        if ($current -and $current.Hash -eq $entry.Hash) { continue }
        $destination = Join-Path $To $entry.Rel
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $From $entry.Rel) -Destination $destination -Force -ErrorAction Stop
        [IO.File]::SetLastWriteTimeUtc($destination, $now)
        $changed++
    }
    foreach ($key in $target.Keys) {
        if ($source.ContainsKey($key)) { continue }
        Remove-Item -LiteralPath (Join-Path $To $target[$key].Rel) -Force -ErrorAction Stop
        $changed++
    }
    # Directories left empty by deleted files (never src\generated).
    foreach ($dir in $ManagedDirs) {
        $full = Join-Path $To $dir
        if (-not (Test-Path -LiteralPath $full)) { continue }
        Get-ChildItem -LiteralPath $full -Recurse -Directory -Force | Sort-Object { $_.FullName.Length } -Descending |
            Where-Object { $_.FullName -notmatch '\\src\\generated(\\|$)' -and -not (Get-ChildItem -LiteralPath $_.FullName -Force) } |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
    }
    return $changed
}

function Get-PinnedLines([string]$File, [string]$Pattern) {
    if (-not (Test-Path -LiteralPath $File)) { return '' }
    return ((Get-Content -LiteralPath $File) | Where-Object { $_ -match $Pattern }) -join "`n"
}

$backupDir = $null
$replacing = $false
try {
    foreach ($dir in @($UpdatesDir, $StateDir)) { [IO.Directory]::CreateDirectory($dir) | Out-Null }

    # 1. Download
    Step "Downloading $Tag"
    $zip = Join-Path $UpdatesDir "PokePort-$Tag.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Invoke-WebRequest -Uri $AssetUrl -OutFile "$zip.part" -UseBasicParsing -Headers @{ 'User-Agent' = 'PokePort-launcher' }
    Move-Item -LiteralPath "$zip.part" -Destination $zip -Force
    if ($Sha256) {
        $actual = Get-Sha256 $zip
        if ($actual -ne $Sha256.ToLowerInvariant()) { throw "The download is corrupt (SHA-256 $actual, expected $Sha256)." }
        Out-Line 'Download verified (SHA-256).'
    }
    Progress 30

    # 2. Extract and validate
    Step 'Checking the new version'
    $extract = Join-Path $UpdatesDir "extract-$Tag"
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
    $newRoot = Get-ChildItem -LiteralPath $extract -Directory | Where-Object {
        (Test-Path (Join-Path $_.FullName 'launcher\PokeParkLauncher.ps1')) -and (Test-Path (Join-Path $_.FullName 'VERSION'))
    } | Select-Object -First 1
    if (-not $newRoot) {
        if ((Test-Path (Join-Path $extract 'launcher\PokeParkLauncher.ps1')) -and (Test-Path (Join-Path $extract 'VERSION'))) {
            $newRoot = Get-Item -LiteralPath $extract
        } else {
            throw 'The downloaded file is not a PokePort release.'
        }
    }
    $newVersion = (Get-Content -LiteralPath (Join-Path $newRoot.FullName 'VERSION') -Raw).Trim()
    if (('v' + $newVersion) -ne $Tag -and $newVersion -ne $Tag) { throw "The download is version $newVersion, expected $Tag." }
    foreach ($required in @('launcher\Build-PokePark.ps1', 'src\runtime\CMakeLists.txt', 'src\projects\pokepark\recomp.yml')) {
        if (-not (Test-Path (Join-Path $newRoot.FullName $required))) { throw "The release is incomplete (missing $required)." }
    }
    Progress 45

    # 3. Back up the current release files
    Step 'Backing up the current version'
    $oldVersion = 'unknown'
    if (Test-Path (Join-Path $Root 'VERSION')) { $oldVersion = (Get-Content -LiteralPath (Join-Path $Root 'VERSION') -Raw).Trim() }
    $backupDir = Join-Path $BuildRoot ("backup\v$oldVersion-{0:yyyyMMdd_HHmmss}" -f (Get-Date))
    Copy-Managed $Root $backupDir
    $oldDeps = Get-PinnedLines (Join-Path $Root 'src\Launcher\Prepare-Dependencies.ps1') 'https://|Uris|Pins'
    $oldToolchain = Get-PinnedLines (Join-Path $Root 'launcher\Build-PokePark.ps1') '^\$(LlvmMingw|CMake|Ninja|Dotnet)Version'
    Progress 60

    # 4. Replace, file by file
    Step "Installing $Tag"
    $replacing = $true
    $changed = Sync-Managed $newRoot.FullName $Root
    $replacing = $false
    Progress 80
    Out-Line "$changed file(s) changed."

    # The game is rebuilt from the new sources; the disc stays extracted.
    foreach ($id in @('translate', 'build', 'install')) { Remove-Item -LiteralPath (Join-Path $StateDir "$id.ok") -ErrorAction SilentlyContinue }
    if ($oldDeps -ne (Get-PinnedLines (Join-Path $Root 'src\Launcher\Prepare-Dependencies.ps1') 'https://|Uris|Pins')) {
        Remove-Item -LiteralPath (Join-Path $StateDir 'deps.ok') -ErrorAction SilentlyContinue
        Out-Line 'The third-party libraries changed and will be downloaded again.'
    }
    if ($oldToolchain -ne (Get-PinnedLines (Join-Path $Root 'launcher\Build-PokePark.ps1') '^\$(LlvmMingw|CMake|Ninja|Dotnet)Version')) {
        Remove-Item -LiteralPath (Join-Path $StateDir 'toolchain.ok') -ErrorAction SilentlyContinue
        Out-Line 'The compiler toolchain changed and will be downloaded again.'
    }
    Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue
    Progress 100
    Out-Line "Updated from v$oldVersion to $Tag. Backup: $backupDir"
    Out-Line '##FINISHED'
    exit 0
} catch {
    $message = $_.Exception.Message
    if ($replacing -and $backupDir -and (Test-Path -LiteralPath $backupDir)) {
        # Sync the folder back to the backup (only files that differ are touched).
        try {
            Sync-Managed $backupDir $Root | Out-Null
            $message += ' The previous version was restored.'
        } catch {
            $message += " Restoring the backup also failed: $($_.Exception.Message). Backup kept in $backupDir."
        }
    }
    Out-Line "##ERROR $message"
    exit 1
}
