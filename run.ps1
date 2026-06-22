#requires -version 3
<#
  run.ps1  —  Roda o MeuOS no QEMU, passando binarios Windows como MODULOS
              do boot (Multiboot). O kernel carrega qualquer PE que receber.

    .\run.ps1                         # roda os exemplos (test.exe + mydriver.sys)
    .\run.ps1 -Headless -TimeoutSec 8 # sem janela, captura serial, encerra so
    .\run.ps1 -Modules C:\app.exe     # roda QUALQUER .exe/.sys do Windows
    .\run.ps1 -Modules a.exe,b.sys    # varios de uma vez
#>
[CmdletBinding()]
param(
    [switch]$Headless,
    [int]$TimeoutSec = 0,
    [string[]]$Modules
)

$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$tools = Join-Path $root 'tools'
$build = Join-Path $root 'build'
$kernel = Join-Path $build 'kernel.bin'
if (-not (Test-Path $kernel)) { throw "kernel.bin nao encontrado. Rode .\build.ps1" }

# localizar QEMU
$qemu = (Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue).Source
if (-not $qemu) {
    foreach ($p in @((Join-Path $tools 'qemu\qemu-system-x86_64.exe'),
                     'C:\Program Files\qemu\qemu-system-x86_64.exe')) {
        if (Test-Path $p) { $qemu = $p; break }
    }
}
if (-not $qemu) { throw "QEMU nao encontrado. Rode .\setup.ps1" }

# Modulos a carregar. Default = os exemplos compilados em build\.
if (-not $Modules -or $Modules.Count -eq 0) {
    $Modules = @()
    foreach ($d in 'test.exe', 'mydriver.sys') {
        $p = Join-Path $build $d
        if (Test-Path $p) { $Modules += $p }
    }
}

# Copia cada modulo para build\ com nome simples (o QEMU separa modulos por
# virgula, entao evitamos espacos no caminho) e monta a lista do -initrd.
$names = @()
foreach ($m in $Modules) {
    if (-not (Test-Path $m)) { throw "Modulo nao encontrado: $m" }
    $name = Split-Path $m -Leaf
    $dest = Join-Path $build $name
    $srcFull  = (Resolve-Path $m).Path
    $destFull = if (Test-Path $dest) { (Resolve-Path $dest).Path } else { $null }
    if ($srcFull -ne $destFull) { Copy-Item $m $dest -Force }
    $names += $name
}
$initrd = $names -join ','

# Rodamos com cwd = build\, entao usamos nomes relativos (sem espacos).
$qargs = @('-kernel', 'kernel.bin', '-m', '256', '-no-reboot', '-serial', 'stdio')
if ($initrd)   { $qargs += @('-initrd', $initrd) }
if ($Headless) { $qargs += @('-display', 'none') }

Write-Host "QEMU   : $qemu"
Write-Host "Modulos: $initrd"
Write-Host "-------------------------------------------"

if ($TimeoutSec -gt 0) {
    $log    = Join-Path $build 'serial.log'
    $errlog = Join-Path $build 'qemu.err.log'
    $argStr = ($qargs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $p = Start-Process -FilePath $qemu -ArgumentList $argStr -WorkingDirectory $build `
            -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError $errlog
    Start-Sleep -Seconds $TimeoutSec
    if (-not $p.HasExited) { $p.Kill() }
    Start-Sleep -Milliseconds 300
    Write-Host "--- saida serial (stdout) ---"
    if (Test-Path $log) { Get-Content $log }
    $e = Get-Content $errlog -ErrorAction SilentlyContinue
    if ($e) { Write-Host "--- qemu stderr ---"; $e }
} else {
    Push-Location $build
    try { & $qemu @qargs } finally { Pop-Location }
}
