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
    [string[]]$Modules,
    [switch]$Screendump,
    [int]$QmpPort = 4444
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
    # Ordem: DLLs (sob demanda), depois DRIVERS (criam dispositivos), depois APPS.
    # FASE 9.4: ddraw.dll + d3d9.dll entram com as demais DLLs (carregadas sob
    # demanda quando dxdemo.exe importa). dxdemo.exe vem ENTRE guiapp e desktop
    # para nao quebrar a sequencia esperada (desktop fica por ultimo e o
    # framebuffer final exibe o desktop completo, igual antes).
    # FASE 9.10 — DLLs do stack DX moderno (dxgi + d3d11 + d3d12) precisam ser
    # registradas para o loader resolver as imports da ddraw.dll (que agora
    # delega para d3d11 no compat mode Windows 10+) e do app d3d11demo.exe.
    foreach ($d in 'ntdll.dll', 'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll',
                    'dxgi.dll', 'd3d11.dll', 'd3d12.dll',
                    'd2d1.dll', 'dwrite.dll', 'dxcore.dll',
                    'ddraw.dll', 'd3d9.dll',
                    'ioctldriver.sys', 'mydriver.sys', 'calller.sys',
                    'test.exe', 'ioctlapp.exe',
                    'conhello.exe', 'test32.exe', 'sysinfo.exe', 'pipeserver.exe', 'pipeclient.exe',
                    'cmd.exe', 'guiapp.exe', 'dxdemo.exe', 'd3d11demo.exe', 'desktop.exe') {
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
# FASE 10.1: adiciona um virtio-gpu-pci secundario para o driver virtio detectar
# (vendor=0x1AF4 device=0x1050). O -vga std permanece como fallback Bochs VBE
# (vendor=0x1234 device=0x1111). Os dois coexistem na enumeracao PCI.
# FASE 10.4: damos um id ao virtio-gpu p/ o QMP screendump poder capturar o
# scanout dele (caso contrario, o screendump default captura o std-vga, que
# em headless permanece em modo texto VGA 720x400 e nao mostra nada do nosso
# desktop renderizado via virtio-gpu).
$qargs = @('-kernel', 'kernel.bin', '-m', '256', '-no-reboot', '-serial', 'stdio',
           '-cpu', 'qemu64,-hypervisor,vendor=GenuineIntel',
           '-device', 'virtio-gpu-pci,id=vgpu0')
if ($initrd)   { $qargs += @('-initrd', $initrd) }
if ($Headless) { $qargs += @('-display', 'none') }
# FASE GPU: -Screendump abre QMP via TCP e, apos o boot, manda 'screendump' p/
# capturar o framebuffer em build\screen.ppm (prova visual). Exige -Headless +
# -TimeoutSec; o screendump roda em paralelo, aguarda alguns segundos de boot
# antes do snapshot. NAO toca no caminho normal sem o switch.
if ($Screendump) {
    $qargs += @('-qmp', "tcp:127.0.0.1:$QmpPort,server,nowait")
}

Write-Host "QEMU   : $qemu"
Write-Host "Modulos: $initrd"
Write-Host "-------------------------------------------"

if ($TimeoutSec -gt 0) {
    $log    = Join-Path $build 'serial.log'
    $errlog = Join-Path $build 'qemu.err.log'
    $argStr = ($qargs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $p = Start-Process -FilePath $qemu -ArgumentList $argStr -WorkingDirectory $build `
            -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError $errlog

    if ($Screendump) {
        # Aguarda o boot subir; depois faz o handshake QMP (qmp_capabilities) +
        # 'screendump' p/ build\screen.ppm. Prova visual sem -display.
        $snapAt = [int]([Math]::Max(2, $TimeoutSec - 2))
        Start-Sleep -Seconds $snapAt
        try {
            $client = New-Object System.Net.Sockets.TcpClient
            $client.Connect('127.0.0.1', $QmpPort)
            $stream = $client.GetStream()
            $reader = New-Object System.IO.StreamReader($stream)
            $writer = New-Object System.IO.StreamWriter($stream)
            $writer.NewLine = "`r`n"; $writer.AutoFlush = $true
            [void]$reader.ReadLine()    # greeting QMP
            $writer.WriteLine('{"execute":"qmp_capabilities"}')
            [void]$reader.ReadLine()
            $ppm = Join-Path $build 'screen.ppm'
            if (Test-Path $ppm) { Remove-Item $ppm -Force -ErrorAction SilentlyContinue }
            # FASE 10.4: tenta capturar do scanout da virtio-gpu (id=vgpu0). Se
            # o QEMU recusar a arg device (versao antiga) ou nao achar, cai pro
            # screendump default (std-vga). Em ambos os casos seguimos sem
            # explodir o boot.
            $cmd = '{"execute":"screendump","arguments":{"filename":"' +
                   ($ppm -replace '\\','\\') + '","device":"vgpu0","head":0}}'
            $writer.WriteLine($cmd)
            $resp = $reader.ReadLine()
            if ($resp -match '"error"') {
                Write-Host "--- screendump (vgpu0) falhou: $resp; tentando default ---"
                $cmd2 = '{"execute":"screendump","arguments":{"filename":"' +
                        ($ppm -replace '\\','\\') + '"}}'
                $writer.WriteLine($cmd2)
                [void]$reader.ReadLine()
            }
            $client.Close()
            Write-Host "--- screendump -> $ppm ---"
        } catch {
            Write-Host "--- screendump falhou: $($_.Exception.Message) ---"
        }
        Start-Sleep -Seconds ([Math]::Max(1, $TimeoutSec - $snapAt))
    } else {
        Start-Sleep -Seconds $TimeoutSec
    }
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
