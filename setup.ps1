#requires -version 3
<#
  setup.ps1  —  Baixa as ferramentas de build/teste para a pasta tools\
                (sem precisar de admin para o Zig).

    .\setup.ps1                 # baixa Zig + tenta instalar/baixar QEMU
    .\setup.ps1 -SkipQemu       # so o compilador (Zig)
    .\setup.ps1 -QemuPortable   # forca QEMU portatil (nao usa winget)
    .\setup.ps1 -Force          # rebaixa o Zig
#>
[CmdletBinding()]
param(
    [switch]$SkipQemu,
    [switch]$QemuPortable,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$tools = Join-Path $root 'tools'
New-Item -ItemType Directory -Force -Path $tools | Out-Null

function Have($n) { [bool](Get-Command $n -ErrorAction SilentlyContinue) }

# =========================== ZIG (compilador C) ===========================
$ZIG_VER = '0.13.0'
$zigExe  = Get-ChildItem -Path $tools -Recurse -Filter zig.exe -ErrorAction SilentlyContinue | Select-Object -First 1

if (Have 'zig') {
    Write-Host "Zig ja esta no PATH." -ForegroundColor Green
} elseif ($zigExe -and -not $Force) {
    Write-Host "Zig ja em: $($zigExe.FullName)" -ForegroundColor Green
} else {
    $url = "https://ziglang.org/download/$ZIG_VER/zig-windows-x86_64-$ZIG_VER.zip"
    $zip = Join-Path $tools 'zig.zip'
    Write-Host "Baixando Zig $ZIG_VER ..."
    curl.exe -L --fail -o $zip $url
    if ($LASTEXITCODE) { throw "Falha ao baixar o Zig." }
    Write-Host "Extraindo Zig ..."
    tar -xf $zip -C $tools
    Remove-Item $zip -Force
    $zigExe = Get-ChildItem -Path $tools -Recurse -Filter zig.exe | Select-Object -First 1
    Write-Host "Zig pronto: $($zigExe.FullName)" -ForegroundColor Green
}

# =============================== NASM ===============================
if (Have 'nasm') {
    Write-Host "NASM ja presente." -ForegroundColor Green
} else {
    Write-Warning "NASM nao encontrado. Instale com:  winget install -e --id NASM.NASM"
}

# =============================== QEMU ===============================
if ($SkipQemu) { Write-Host "QEMU ignorado (-SkipQemu)."; return }

if (Have 'qemu-system-x86_64') {
    Write-Host "QEMU ja no PATH." -ForegroundColor Green
    return
}
if (Test-Path (Join-Path $tools 'qemu\qemu-system-x86_64.exe')) {
    Write-Host "QEMU ja em tools\qemu." -ForegroundColor Green
    return
}

$installed = $false
if (-not $QemuPortable -and (Have 'winget')) {
    Write-Host "Instalando QEMU via winget (pode pedir confirmacao do Windows/UAC) ..."
    try {
        winget install -e --id SoftwareFreedomConservancy.QEMU `
            --silent --accept-package-agreements --accept-source-agreements | Out-Host
    } catch { Write-Warning "winget: $_" }
    if ((Have 'qemu-system-x86_64') -or (Test-Path 'C:\Program Files\qemu\qemu-system-x86_64.exe')) {
        $installed = $true
        Write-Host "QEMU instalado via winget." -ForegroundColor Green
    }
}

if (-not $installed) {
    Write-Host "Baixando QEMU portatil (qemu.weilnetz.de) ..."
    # 7zr extrai o instalador NSIS do QEMU sem precisar instalar nada.
    $sevenz = Join-Path $tools '7zr.exe'
    if (-not (Test-Path $sevenz)) { curl.exe -L --fail -o $sevenz 'https://www.7-zip.org/a/7zr.exe' }

    $listing = curl.exe -s 'https://qemu.weilnetz.de/w64/'
    $name = [regex]::Matches($listing, 'qemu-w64-setup-\d+\.exe') |
            ForEach-Object Value | Sort-Object -Unique | Select-Object -Last 1
    if (-not $name) { throw "Nao consegui descobrir o instalador atual do QEMU." }

    $inst = Join-Path $tools $name
    Write-Host "  -> $name"
    curl.exe -L --fail -o $inst "https://qemu.weilnetz.de/w64/$name"
    Write-Host "Extraindo QEMU para tools\qemu ..."
    & $sevenz x $inst "-o$(Join-Path $tools 'qemu')" -y | Out-Null
    Remove-Item $inst -Force

    if (Test-Path (Join-Path $tools 'qemu\qemu-system-x86_64.exe')) {
        Write-Host "QEMU portatil pronto em tools\qemu." -ForegroundColor Green
    } else {
        Write-Warning "Extracao terminou mas nao achei qemu-system-x86_64.exe. Verifique tools\qemu."
    }
}

Write-Host "`nSetup concluido. Agora rode:  .\build.ps1" -ForegroundColor Green
