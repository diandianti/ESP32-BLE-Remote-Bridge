# Shared helpers for MI-RC003 Bridge build/release scripts.
# Dot-source this file from other scripts in tools/.

$Script:RepoRoot = Split-Path -Parent $PSScriptRoot
$Script:BuildDir = Join-Path $Script:RepoRoot 'build'
$Script:CacheDir = Join-Path $Script:RepoRoot '.cache'
$Script:DistDir = Join-Path $Script:RepoRoot 'dist'
$Script:WebFlashDir = Join-Path $Script:RepoRoot 'webusb-config\flash'
$Script:FirmwareOutDir = Join-Path $Script:BuildDir 'firmware'

$Script:Profiles = @('n16r8', 'n8r2', 'n4r2')
$Script:DefaultProfile = 'n16r8'
$Script:ProfileInfo = [ordered]@{
    n16r8 = [pscustomobject]@{ Profile = 'n16r8'; Label = 'N16R8'; Flash = '16MB'; Psram = '8MB Octal' }
    n8r2  = [pscustomobject]@{ Profile = 'n8r2';  Label = 'N8R2';  Flash = '8MB';  Psram = '2MB Quad' }
    n4r2  = [pscustomobject]@{ Profile = 'n4r2';  Label = 'N4R2';  Flash = '4MB';  Psram = '2MB Quad' }
}

function Get-ProfileInfo([string]$Profile) {
    $Profile = $Profile.ToLower()
    if (-not $Script:ProfileInfo.Contains($Profile)) { throw ('未知硬件配置: ' + $Profile) }
    return $Script:ProfileInfo[$Profile]
}

function Get-ProfileFlashSize([string]$Profile) { return (Get-ProfileInfo $Profile).Flash }

function Get-ProfileBuildDir([string]$Profile) { return (Join-Path $Script:RepoRoot ('build-' + $Profile)) }

function Get-ProfileSdkconfig([string]$Profile) { return (Join-Path $Script:RepoRoot ('.sdkconfig.' + $Profile)) }

function Get-ProfileDefaultsFile([string]$Profile) {
    $p = Join-Path $Script:RepoRoot ('sdkconfig.defaults.' + $Profile)
    if (-not (Test-Path $p)) { throw ('未找到硬件配置: ' + $p) }
    return $p
}

function Write-Title([string]$Text) {
    Write-Host ''
    Write-Host ('=' * 60) -ForegroundColor DarkCyan
    Write-Host ('  ' + $Text) -ForegroundColor Cyan
    Write-Host ('=' * 60) -ForegroundColor DarkCyan
}
function Write-Info([string]$Text) { Write-Host ('[*] ' + $Text) -ForegroundColor Cyan }
function Write-Ok([string]$Text) { Write-Host ('[+] ' + $Text) -ForegroundColor Green }
function Write-Warn([string]$Text) { Write-Host ('[!] ' + $Text) -ForegroundColor Yellow }
function Write-Fail([string]$Text) { Write-Host ('[x] ' + $Text) -ForegroundColor Red }

function Get-FirmwareVersion {
    $vh = Join-Path $Script:RepoRoot 'main\version.h'
    if (Test-Path $vh) {
        foreach ($line in (Get-Content $vh)) {
            if ($line -match 'FIRMWARE_VERSION\s+"([^"]+)"') { return $Matches[1] }
        }
    }
    return '0.0.0'
}

function Initialize-IdfEnv {
    if (Get-Command idf.py -ErrorAction SilentlyContinue) { return $true }
    $profiles = @()
    if ($env:IDF_TOOLS_PATH) {
        $dir = Join-Path $env:IDF_TOOLS_PATH 'tools'
        if (Test-Path $dir) {
            $profiles += Get-ChildItem $dir -Filter 'Microsoft.*.PowerShell_profile.ps1' -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | ForEach-Object { $_.FullName }
        }
    }
    foreach ($base in @('C:\Espressif\tools', (Join-Path $env:USERPROFILE '.espressif\tools'))) {
        if (Test-Path $base) {
            $profiles += Get-ChildItem $base -Filter 'Microsoft.*.PowerShell_profile.ps1' -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | ForEach-Object { $_.FullName }
        }
    }
    foreach ($p in ($profiles | Select-Object -Unique)) {
        try {
            . $p *> $null
            if (Get-Command idf.py -ErrorAction SilentlyContinue) { return $true }
        } catch { }
    }
    return $false
}

function Get-FlashPlan {
    param([string]$BuildDir = $Script:BuildDir)
    $fa = Join-Path $BuildDir 'flasher_args.json'
    if (-not (Test-Path $fa)) { throw ('未找到 ' + $fa + '，请先编译固件。') }
    $j = Get-Content $fa -Raw -Encoding UTF8 | ConvertFrom-Json
    $files = @()
    foreach ($prop in $j.flash_files.PSObject.Properties) {
        $full = Join-Path $BuildDir $prop.Value
        if (-not (Test-Path $full)) { throw ('缺少固件文件: ' + $full) }
        $files += [pscustomobject]@{ Offset = $prop.Name; File = $full; Rel = $prop.Value }
    }
    $files = @($files | Sort-Object { [Convert]::ToInt64($_.Offset.Substring(2), 16) })
    return [pscustomobject]@{ Settings = $j.flash_settings; Files = $files }
}

function Get-Esptool {
    $c = Get-Command esptool.exe -ErrorAction SilentlyContinue
    if ($c) { return [pscustomobject]@{ File = $c.Source; Prefix = @() } }
    $roots = @()
    if ($env:IDF_PYTHON_ENV_PATH) { $roots += $env:IDF_PYTHON_ENV_PATH }
    if ($env:IDF_TOOLS_PATH) { $roots += (Join-Path $env:IDF_TOOLS_PATH 'python') }
    $roots += 'C:\Espressif\tools\python'
    $roots += (Join-Path $env:USERPROFILE '.espressif\python_env')
    foreach ($r in ($roots | Select-Object -Unique)) {
        if (-not (Test-Path $r)) { continue }
        if (Test-Path (Join-Path $r 'Scripts\esptool.exe')) {
            return [pscustomobject]@{ File = (Join-Path $r 'Scripts\esptool.exe'); Prefix = @() }
        }
        $hit = Get-ChildItem $r -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'venv\Scripts\esptool.exe' } |
            Where-Object { Test-Path $_ } |
            Select-Object -First 1
        if ($hit) { return [pscustomobject]@{ File = $hit; Prefix = @() } }
    }
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) { return [pscustomobject]@{ File = $py.Source; Prefix = @('-m', 'esptool') } }
    return $null
}

function Get-EsptoolZip([string]$Version) {
    New-Item -ItemType Directory -Force -Path $Script:CacheDir | Out-Null
    $zip = Join-Path $Script:CacheDir ('esptool-v{0}-windows-amd64.zip' -f $Version)
    if (-not (Test-Path $zip)) {
        $url = 'https://github.com/espressif/esptool/releases/download/v{0}/esptool-v{0}-windows-amd64.zip' -f $Version
        Write-Info ('下载 esptool v' + $Version + ' (约 66 MB，仅首次)')
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    }
    return $zip
}

function Expand-Esptool([string]$Zip, [string]$DestDir) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $map = [ordered]@{ 'esptool.exe' = 'esptool.exe'; 'LICENSE' = 'LICENSE-esptool.txt' }
        foreach ($key in $map.Keys) {
            $entry = $archive.Entries |
                Where-Object { $_.FullName -eq ('esptool-windows-amd64/' + $key) } |
                Select-Object -First 1
            if (-not $entry) { throw ('压缩包中缺少 ' + $key) }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $DestDir $map[$key]), $true)
        }
    } finally {
        $archive.Dispose()
    }
}

function New-MergedBin {
    param(
        [Parameter(Mandatory)] $Esptool,
        [Parameter(Mandatory)] $Plan,
        [Parameter(Mandatory)] [string]$OutFile,
        [string]$FlashMode = 'dio',
        [string]$FlashFreq = '80m',
        [string]$FlashSize = '16MB'
    )
    $a = @($Esptool.Prefix)
    $a += '--chip'; $a += 'esp32s3'
    $a += 'merge-bin'; $a += '-o'; $a += $OutFile
    $a += '--flash-mode'; $a += $FlashMode
    $a += '--flash-freq'; $a += $FlashFreq
    $a += '--flash-size'; $a += $FlashSize
    foreach ($f in $Plan.Files) { $a += $f.Offset; $a += $f.File }
    Write-Info '合并镜像 (bootloader + 分区表 + 应用)'
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Esptool.File @a
        $mergeCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }
    if ($mergeCode -ne 0) { throw 'merge-bin 失败' }
    if (-not (Test-Path $OutFile)) { throw ('未生成 ' + $OutFile) }
}

function Write-TextFile([string]$Path, [string]$Text) {
    $utf8 = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllText($Path, $Text, $utf8)
}

function New-ZipFromDirectory([string]$SourceDir, [string]$ZipPath) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    $zip = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $base = (Resolve-Path $SourceDir).Path.TrimEnd('\')
        Get-ChildItem $SourceDir -Recurse -File | ForEach-Object {
            $rel = ($_.FullName.Substring($base.Length + 1)) -replace '\\', '/'
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $_.FullName, $rel, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {
        $zip.Dispose()
    }
}

function Write-JsonFile([string]$Path, $Object) {
    $json = $Object | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($Path, $json, (New-Object System.Text.UTF8Encoding($false)))
}

function Copy-BinFile([string]$Source, [string]$Destination) {
    $src = (Resolve-Path $Source).Path
    if (Test-Path $Destination) {
        if ($src -ieq (Resolve-Path $Destination).Path) { return }
    }
    Copy-Item $src $Destination -Force
}

function New-WebFlashFiles([string]$Version, [System.Collections.IDictionary]$MergedBins) {
    $fwDir = Join-Path $Script:WebFlashDir 'firmware'
    New-Item -ItemType Directory -Force -Path $fwDir | Out-Null

    $profiles = @($Script:Profiles | Where-Object { $MergedBins.Contains($_) })
    if ($profiles.Count -eq 0) { throw '没有可用的硬件固件。' }

    $boards = @()
    foreach ($prof in $profiles) {
        $info = Get-ProfileInfo $prof
        $binName = 'merged-flash-' + $prof + '.bin'
        Copy-BinFile $MergedBins[$prof] (Join-Path $fwDir $binName)

        $manifest = [ordered]@{
            name                     = 'ESP32 BLE Remote Bridge ' + $info.Label
            version                  = $Version
            new_install_prompt_erase = $true
            builds                   = @(
                [ordered]@{
                    chipFamily = 'ESP32-S3'
                    parts      = @(
                        [ordered]@{ path = 'firmware/' + $binName; offset = 0 }
                    )
                }
            )
        }
        Write-JsonFile (Join-Path $Script:WebFlashDir ('manifest-' + $prof + '.json')) $manifest

        $boards += [ordered]@{
            profile  = $prof
            label    = $info.Label
            flash    = $info.Flash
            psram    = $info.Psram
            manifest = 'manifest-' + $prof + '.json'
        }
    }

    $primary = $profiles[0]
    Copy-BinFile $MergedBins[$primary] (Join-Path $fwDir 'merged-flash.bin')
    $primaryInfo = Get-ProfileInfo $primary
    $defaultManifest = [ordered]@{
        name                     = 'ESP32 BLE Remote Bridge ' + $primaryInfo.Label
        version                  = $Version
        new_install_prompt_erase = $true
        builds                   = @(
            [ordered]@{
                chipFamily = 'ESP32-S3'
                parts      = @(
                    [ordered]@{ path = 'firmware/merged-flash.bin'; offset = 0 }
                )
            }
        )
    }
    Write-JsonFile (Join-Path $Script:WebFlashDir 'manifest.json') $defaultManifest

    Write-JsonFile (Join-Path $Script:WebFlashDir 'boards.json') ([ordered]@{
        version        = $Version
        defaultProfile = $primary
        boards         = $boards
    })
}

