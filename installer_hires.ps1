# HiRes mod for Prototype 2 (resolutions above 1920x1200) - installer / uninstaller.
# Separate from the Dual Input mod. Run through install_hires.bat or uninstall_hires.bat.
param(
    [switch]$Uninstall,
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$AppId      = '115320'
$GameFolder = 'Prototype 2'
$ExeName    = 'prototype2.exe'
$EngineName = 'prototype2engine.dll'
$ModFiles   = @('winmm.dll')
$IniName    = 'p2_hires.ini'
$LogNames   = @('p2_hires.log')
$BackupRoot = 'DualInput_backup'
$BackupDir  = Join-Path $BackupRoot 'hires'
# SHA-256 of prototype2engine.dll from the Steam build the mod was made and tested on.
$KnownEngine = 'C3D51D6355387963A63CBAC058381B8FD1CE39F51088FA50276A49285EE94174'
$ModDir     = Join-Path $PSScriptRoot 'mod_hires'

function Resolve-Target($dir) {
    if (-not $dir) { return $null }
    if (Test-Path (Join-Path $dir $ExeName)) { return $dir }
    return $null
}

function Find-GameDir {
    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $p = Get-ItemProperty $key -ErrorAction Stop
            $steam = if ($p.SteamPath) { $p.SteamPath } else { $p.InstallPath }
            if ($steam) { break }
        } catch {}
    }
    if (-not $steam) { return $null }

    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libs += $m.Groups[1].Value -replace '\\\\', '\'
        }
    }
    foreach ($lib in $libs | Select-Object -Unique) {
        $apps = Join-Path $lib 'steamapps'
        # Prefer the folder named in the game's Steam manifest, fall back to the default name.
        $names = @()
        $acf = Join-Path $apps "appmanifest_$AppId.acf"
        if (Test-Path $acf) {
            $m = [regex]::Match((Get-Content $acf -Raw), '"installdir"\s+"([^"]+)"')
            if ($m.Success) { $names += $m.Groups[1].Value }
        }
        $names += $GameFolder
        foreach ($n in $names | Select-Object -Unique) {
            $t = Resolve-Target (Join-Path $apps "common\$n")
            if ($t) { return $t }
        }
    }
    return $null
}

Write-Host ''
Write-Host '=== HiRes mod for Prototype 2 ===' -ForegroundColor Cyan
Write-Host ''

$GameDir = Resolve-Target $GameDir
if (-not $GameDir) { $GameDir = Find-GameDir }
while (-not $GameDir) {
    Write-Host 'The game was not found automatically (or the folder is wrong).' -ForegroundColor Yellow
    $in = (Read-Host "Paste the game folder (the one with $ExeName), or leave empty to cancel").Trim('" ')
    if (-not $in) { Write-Host 'Cancelled.'; exit 1 }
    $GameDir = Resolve-Target $in
    if (-not $GameDir) { Write-Host "$ExeName not found in: $in" -ForegroundColor Yellow }
}
Write-Host "Game folder: $GameDir"

$backup = Join-Path $GameDir $BackupDir

if ($Uninstall) {
    foreach ($f in $ModFiles + $LogNames) {
        $path = Join-Path $GameDir $f
        if (Test-Path $path) { Remove-Item $path -Force; Write-Host "  removed  $f" }
    }
    if (Test-Path $backup) {
        Get-ChildItem $backup | ForEach-Object {
            Move-Item $_.FullName (Join-Path $GameDir $_.Name) -Force
            Write-Host "  restored $($_.Name) (from before the mod)"
        }
        Remove-Item $backup -Force -Recurse
        # Remove the shared backup folder only if the Dual Input mod does not use it.
        $root = Join-Path $GameDir $BackupRoot
        if ((Test-Path $root) -and -not (Get-ChildItem $root -Force)) { Remove-Item $root -Force }
    }
    $ini = Join-Path $GameDir $IniName
    if (Test-Path $ini) {
        $a = Read-Host "Delete your settings file $IniName too? (y/N)"
        if ($a -match '^[yY]') { Remove-Item $ini -Force; Write-Host "  removed  $IniName" }
        else { Write-Host "  kept     $IniName" }
    }
    Write-Host ''
    Write-Host 'HiRes mod uninstalled. If the game starts with a wrong resolution, pick one again in Options.' -ForegroundColor Green
    exit 0
}

$engine = Join-Path $GameDir $EngineName
$hash = if (Test-Path $engine) { (Get-FileHash $engine -Algorithm SHA256).Hash } else { $null }
if ($hash -ne $KnownEngine) {
    Write-Host ''
    Write-Host "WARNING: your $EngineName is a different version than the one the mod" -ForegroundColor Yellow
    Write-Host 'was tested on (current Steam build). The mod checks the resolution table' -ForegroundColor Yellow
    Write-Host 'before changing it and skips it if it does not match (see p2_hires.log), so' -ForegroundColor Yellow
    Write-Host 'the game then just behaves as without the mod.' -ForegroundColor Yellow
    $a = Read-Host 'Install anyway? (y/N)'
    if ($a -notmatch '^[yY]') { Write-Host 'Cancelled.'; exit 1 }
}

foreach ($f in $ModFiles) {
    $src = Join-Path $ModDir $f
    $dst = Join-Path $GameDir $f
    if ((Test-Path $dst) -and (Get-FileHash $dst).Hash -ne (Get-FileHash $src).Hash) {
        # Another mod (or an older copy) uses the same file name: keep it, unless it is
        # an earlier version of this mod (it contains the name of its ini file).
        $isOurs = Select-String -Path $dst -Pattern $IniName -SimpleMatch -Quiet
        if (-not $isOurs -and -not (Test-Path (Join-Path $backup $f))) {
            New-Item -ItemType Directory -Path $backup -Force | Out-Null
            Copy-Item $dst (Join-Path $backup $f)
            Write-Host "  backed up existing $f to $BackupDir\" -ForegroundColor Yellow
        }
    }
    Copy-Item $src $dst -Force
    Write-Host "  installed $f"
}

$ini = Join-Path $GameDir $IniName
if (Test-Path $ini) {
    Write-Host "  kept your existing $IniName (settings unchanged)"
} else {
    Copy-Item (Join-Path $ModDir $IniName) $ini
    Write-Host "  installed $IniName"
}

Write-Host ''
Write-Host 'Done. Next steps:' -ForegroundColor Green
Write-Host '  1. Start the game and select your resolution in Options again (the saved'
Write-Host '     setting is a position in the resolution list, which the mod changed).'
Write-Host '  2. 4K / ultrawide appear only if your display or driver offers them. On a'
Write-Host '     1080p/1200p monitor enable NVIDIA DLDSR (or AMD VSR), see README.'
Write-Host "     Resolution list: $ini"
Write-Host '  3. To remove the mod, run uninstall_hires.bat.'
