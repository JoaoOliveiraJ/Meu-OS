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
    # Cenario nomeado (mapa nome->modulos abaixo). Ex.: -Scenario desktop | pintok.
    # -Modules tem prioridade se passado. 'full' (ou sem -Scenario) = lista completa.
    [string]$Scenario,
    [switch]$Screendump,
    [int]$QmpPort = 4444,
    # FASE 3c — injeta teclas via QMP 'send-key' apos o boot (prova de entrada de
    # teclado headless). Lista de qcodes separados por espaco: ex. "4 2 spc m e u o s ret".
    [string]$SendKeys,
    # FASE 2 — anexa build\disk.img como disco IDE (canal primario master, 0x1F0)
    # p/ o hal/disk.c detectar via ATA PIO e o NTFS montar. Gere com make-ntfs-disk.ps1.
    [switch]$Disk,
    # FASE 14 — usa o display GTK com OpenGL (-display gtk,gl=on). O caminho de
    # compose GL do QEMU desenha o cursor de HW do virtio-gpu como um OVERLAY de
    # textura, contornando o limite/nao-render do "window cursor" do GTK 2D no
    # host Windows. Use se o cursor de HW nao aparecer no display 2D padrao.
    [switch]$Gl
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

# Cenarios nomeados: mapa nome -> lista de modulos (leaf names em build\). ADITIVO;
# nao toca no caminho default (sem -Scenario e sem -Modules = lista completa "full").
#   .\run.ps1 -Scenario desktop                          # login -> desktop, SEM o desfile de testes
#   .\run.ps1 -Scenario pintok -Headless -TimeoutSec 40  # baseline anti-cheat (Riot Vanguard)
$scenarios = @{
    # Desktop enxuto: so as DLLs que csrss/winlogon/explorer precisam + o caminho
    # login -> shell persistente. SEM apps de teste (test/sysinfo/pipe*/dx*/cmd/...)
    # e SEM os draw-once redundantes (win10ui/desktop). O virtio-gpu/tablet/teclado
    # sao drivers do KERNEL (ja embutidos), nao modulos de boot.
    'desktop' = @('ntdll.dll', 'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll',
                  'secur32.dll', 'credui.dll',
                  'csrss.exe', 'winlogon.exe', 'explorer.exe')
    # Baseline do pintok.sys (Riot Vanguard): so o driver ring-0.
    'pintok'  = @('pintok.sys')
    # BRING-UP DO EXPLORER REAL (C:\Windows\explorer.exe -> build\explorerreal.exe).
    # Os 16 modulos que o binario real exige hoje (ver PROMPT-PROXIMA-SESSAO.md). Um
    # unico TOKEN -Scenario evita o problema de passar a lista separada por virgula a
    # partir do bash (o comma-split so acontece quando o PowerShell parseia a linha; via
    # bash->'-File' a lista chega como 1 string so). Use:
    #   .\run.ps1 -Scenario explorerreal -Headless -TimeoutSec 50
    'explorerreal' = @('ntdll.dll', 'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll',
                       'ucrtbase.dll', 'combase.dll', 'msvcp_win.dll', 'shell32.dll', 'shcore.dll',
                       'dxgi.dll', 'uxtheme.dll', 'comctl32.dll', 'dui70.dll', 'dwmapi.dll',
                       'explorerreal.exe')
}
if ($Scenario -and $Scenario -ne 'full') {
    if (-not $scenarios.ContainsKey($Scenario)) {
        throw "Cenario '$Scenario' desconhecido. Opcoes: $(($scenarios.Keys | Sort-Object) -join ', '), full"
    }
    if (-not $Modules -or $Modules.Count -eq 0) {
        $Modules = @()
        foreach ($d in $scenarios[$Scenario]) {
            $p = Join-Path $build $d
            if (Test-Path $p) { $Modules += $p }
        }
        if (-not $Modules -or $Modules.Count -eq 0) {
            throw "Cenario '$Scenario': nenhum modulo encontrado em build\. Rode .\build.ps1"
        }
    }
}

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
                    'mmdevapi.dll', 'Audioses.dll', 'dsound.dll', 'winmm.dll',
                    'ws2_32.dll',
                    'secur32.dll', 'credui.dll',
                    'ioctldriver.sys', 'mydriver.sys', 'calller.sys',
                    'test.exe', 'ioctlapp.exe',
                    'conhello.exe', 'test32.exe', 'sysinfo.exe', 'pipeserver.exe', 'pipeclient.exe',
                    'cmd.exe', 'guiapp.exe', 'dxdemo.exe', 'd3d11demo.exe',
                    'csrss.exe', 'winlogon.exe', 'win10ui.exe',
                    'desktop.exe',
                    'explorer.exe') {   # POR ULTIMO: shell ring-3 persistente
                                        # (nao sai; segura o boot no seu loop)
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
           # Pilar 3 (NT foundation): roda com 2 cores para o SMP (MADT mostra
           # 2 Local APICs e INIT-SIPI-SIPI levanta o segundo). Pilar 4 usa
           # ambos para preempcao MP.
           '-smp', '2',
           # TCG multi-thread (1 host thread por vCPU). Sem isso o TCG e' single-
           # threaded round-robin: um vCPU em loop tight faz o outro starvado.
           # Necessario para SMP em qualquer host sem KVM (Windows headless).
           '-accel', 'tcg,thread=multi',
           # v0.9.0+ fix: -vga none remove o std-vga (Bochs) primario para que
           # o virtio-gpu seja o UNICO display PCI — assim a janela do QEMU
           # mostra o desktop renderizado via virtio-gpu (era invisivel antes
           # porque QEMU mostrava o std-vga primario que ficou em modo texto).
           # Bochs VBE permanece disponivel como classe (em hardware real); aqui
           # so removemos a instancia QEMU duplicada.
           '-vga', 'none',
           '-device', 'virtio-gpu-pci,id=vgpu0',
           # FASE 11 (audio stack): expoe um controlador HD Audio Intel ICH9
           # no PCI. O driver hda_init() detecta vendor=0x8086 class=0x04
           # subclass=0x03 e loga BAR0. Sem '-audiodev' o stream e silencioso —
           # suficiente para a deteccao do KMD.
           '-device', 'intel-hda', '-device', 'hda-output',
           # FASE 12 (network stack): expoe um Intel 82540EM-A (NIC E1000) no
           # PCI. e1000_init() detecta vendor=0x8086 device=0x100E class=0x02
           # subclass=0x00 e loga BAR0. -netdev user fornece NAT user-mode (sem
           # acesso externo, mas o controller fica visivel no PCI).
           '-netdev', 'user,id=net0', '-device', 'e1000,netdev=net0',
           # FASE 13 (USB stack): expoe um xHCI USB 3.0 controller no PCI.
           # xhci_init() detecta class=0x0C subclass=0x03 prog_if=0x30
           # (vendor=0x1B36 device=0x000D no QEMU). Sem dispositivos USB
           # ligados (-usbdevice ... seria opcional); o controller em si
           # ja basta para o stub registrar 1 HCD via usbport.
           '-device', 'qemu-xhci,id=xhci0',
           # FASE 14 (cursor absoluto): um virtio-tablet-pci (virtio-input,
           # vendor=0x1AF4 device=0x1052). O driver virtio_input.c o leva a
           # DRIVER_OK; ai o QEMU passa a tratar a tablet como ponteiro ATIVO
           # (modo ABSOLUTO) e PARA de capturar (grab) o mouse. Sem isso o
           # cursor de HW do virtio-gpu fica invisivel (o grab esconde o cursor
           # da janela) e o eixo X satura (dx=-127). e' como uma VM Windows real
           # roda no QEMU: HID absoluto -> cursor visivel + posicao p/ cliques.
           '-device', 'virtio-tablet-pci')
if ($initrd)   { $qargs += @('-initrd', $initrd) }
if ($Headless) { $qargs += @('-display', 'none') }
elseif ($Gl)   { $qargs += @('-display', 'gtk,gl=on') }   # cursor de HW via overlay GL
# FASE 2 (disco): anexa build\disk.img como IDE primario master (if=ide,index=0),
# exatamente o canal 0x1F0 que o hal/disk.c le por ATA PIO. cwd=build\ no run, entao
# usamos o nome relativo 'disk.img' (sem espacos). Sem o switch, nada muda (o boot
# roda sem disco e o teste de NTFS e' pulado — baseline do pintok intacto).
if ($Disk) {
    if (-not (Test-Path (Join-Path $build 'disk.img'))) {
        throw "build\disk.img nao encontrado. Gere com: .\apps\make-ntfs-disk.ps1"
    }
    $qargs += @('-drive', 'file=disk.img,format=raw,if=ide,index=0,media=disk')
}
# FASE GPU: -Screendump abre QMP via TCP e, apos o boot, manda 'screendump' p/
# capturar o framebuffer em build\screen.ppm (prova visual). Exige -Headless +
# -TimeoutSec; o screendump roda em paralelo, aguarda alguns segundos de boot
# antes do snapshot. NAO toca no caminho normal sem o switch.
if ($Screendump -or $SendKeys) {
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
    } elseif ($SendKeys) {
        # FASE 3c: injeta teclas via QMP 'send-key' depois do boot subir — prova de
        # entrada de teclado SEM display (mesmo caminho do PS/2 real -> IRQ1 -> fila
        # de stdin). NAO toca no caminho sem o switch (pintok/baseline intactos).
        $sendAt = [int]([Math]::Max(2, [Math]::Min(5, $TimeoutSec - 3)))
        Start-Sleep -Seconds $sendAt
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
            foreach ($k in ($SendKeys -split '\s+' | Where-Object { $_ })) {
                $cmd = '{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"' + $k + '"}]}}'
                $writer.WriteLine($cmd)
                [void]$reader.ReadLine()
                Start-Sleep -Milliseconds 120
            }
            $client.Close()
            Write-Host "--- send-key: $SendKeys ---"
        } catch {
            Write-Host "--- send-key falhou: $($_.Exception.Message) ---"
        }
        Start-Sleep -Seconds ([Math]::Max(1, $TimeoutSec - $sendAt))
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
