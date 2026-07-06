#requires -version 3
<#
  build.ps1  —  Compila o kernel (build\kernel.bin + kernel.elf).
  Varre src\ recursivamente: basta criar arquivos .c/.asm que entram no build.
  Usa NASM (boot/isr) e Zig (zig cc) para o C. Rode setup.ps1 antes.
#>
[CmdletBinding()]
param([switch]$Clean)

$ErrorActionPreference = 'Stop'

# Mata QEMUs orfaos que possam estar segurando build\ (o -Clean falha se um
# QEMU estiver vivo com a pasta aberta — foi a causa de falhas de build).
Get-Process -Name 'qemu*' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

$root  = $PSScriptRoot
$src   = Join-Path $root 'src'
$out   = Join-Path $root 'build'
$tools = Join-Path $root 'tools'

if ($Clean -and (Test-Path $out)) {
    try { Remove-Item -Recurse -Force $out -ErrorAction Stop }
    catch { Write-Host "[build] aviso: build\ em uso; seguindo com overwrite." }
}
New-Item -ItemType Directory -Force -Path $out | Out-Null

# --- localizar Zig e NASM ---
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) {
    $z = Get-ChildItem -Path $tools -Recurse -Filter zig.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($z) { $zig = $z.FullName }
}
if (-not $zig)  { throw "Zig nao encontrado. Rode: .\setup.ps1" }
$nasm = (Get-Command nasm -ErrorAction SilentlyContinue).Source
if (-not $nasm) { throw "NASM nao encontrado." }

Write-Host "Zig  : $zig"
Write-Host "NASM : $nasm`n"

# Objeto unico por arquivo: caminho relativo + EXTENSAO original embutida no
# nome. Sem a extensao, isr.asm e isr.c gerariam o mesmo .o e colidiriam (foi
# o bug da FASE 8 quando movemos isr.{asm,c} pra mesma pasta ntos/ke/amd64).
function Get-ObjName($file) {
    $rel = $file.FullName.Substring($src.Length).TrimStart('\','/')
    $rel = $rel -replace '[\\/]','_'
    # Substitui '.asm' por '_asm.o' e '.c' por '_c.o'.
    $base = [IO.Path]::GetFileNameWithoutExtension($rel)
    $dir  = [IO.Path]::GetDirectoryName($rel)
    $ext  = [IO.Path]::GetExtension($rel).TrimStart('.')   # 'asm' ou 'c'
    if ($dir) { Join-Path $out ("${dir}_${base}_${ext}.o") }
    else      { Join-Path $out ("${base}_${ext}.o") }
}

$objs = @()

# Programas Windows de exemplo (em apps\) — compilados com o alvo Windows,
# NAO fazem parte do OS. Viram binarios PE soltos em build\, carregados no boot.
$sdk = Join-Path $root 'sdk'
$ddk = Join-Path $sdk 'ddk'
$ex  = Join-Path $root 'apps'

# 0a) Exemplo: um .exe Windows (PE32+ x64).
$hello = Join-Path $ex 'hello.c'
if (Test-Path $hello) {
    Write-Host "[exemplo] apps\hello.c -> build\test.exe (.exe Windows)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x800000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'test.exe') $hello -luser32 -lkernel32
    if ($LASTEXITCODE) { throw "Compilacao do exemplo hello.c falhou." }
}

# 0a2) Exemplo: um .exe Windows de 32 BITS (PE32, x86 / IMAGE_NT_HEADERS32).
#      Alvo x86-windows-gnu -> machine 0x14C, optional magic 0x10B. Base 0x1600000
#      (faixa separada da 64-bit). Autocontido (sem imports): faz int 0x80 direto.
#      Mantemos a .reloc (--dynamicbase) para exercitar as relocacoes no loader.
$hello32 = Join-Path $ex 'hello32.c'
if (Test-Path $hello32) {
    Write-Host "[exemplo] apps\hello32.c -> build\test32.exe (.exe Windows 32-bit / PE32)"
    & $zig cc -target x86-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1600000' '-Wl,--subsystem,console' '-Wl,--dynamicbase' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'test32.exe') $hello32
    if ($LASTEXITCODE) { throw "Compilacao do exemplo hello32.c falhou." }
}

# 0b) Exemplo: um driver de kernel Windows (.sys, subsystem NATIVE).
foreach ($drv in @(
        @{ src = 'mydriver.c';    out = 'mydriver.sys';    base = '0x1200000' },
        @{ src = 'ioctldriver.c'; out = 'ioctldriver.sys'; base = '0x1400000' },
        @{ src = 'calller.c';     out = 'calller.sys';     base = '0x3A00000' },
        @{ src = 'wdmdemo.c';     out = 'wdmdemo.sys';     base = '0x1600000' })) {
    $sp = Join-Path $ex $drv.src
    if (Test-Path $sp) {
        Write-Host "[exemplo] apps\$($drv.src) -> build\$($drv.out) (.sys driver)"
        & $zig cc -target x86_64-windows-gnu -nostdlib -e DriverEntry `
            '-Wl,--subsystem,native' "-Wl,--image-base=$($drv.base)" "-I$sdk", "-I$ddk" `
            -o (Join-Path $out $drv.out) $sp -lntoskrnl
        if ($LASTEXITCODE) { throw "Compilacao de $($drv.src) falhou." }
    }
}

# Aplicativo de CONSOLE (.exe): GetStdHandle + WriteFile (via NtWriteFile) +
# GetModuleHandleA/GetProcAddress + MessageBoxA. ImageBase 0x1800000 (livre).
$conhello = Join-Path $ex 'conhello.c'
if (Test-Path $conhello) {
    Write-Host "[exemplo] apps\conhello.c -> build\conhello.exe"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1800000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'conhello.exe') $conhello -lkernel32 -luser32
    if ($LASTEXITCODE) { throw "Compilacao do conhello.c falhou." }
}

# FRENTE 3 (teste de relocacao no caminho de USUARIO): o MESMO conhello.c compilado
# com ImageBase ALTO (0x140000000 = 5 GiB, o default de .exe reais do Windows MinGW/
# MSVC) + .reloc (--dynamicbase). Prova que o ldr_run reloca uma imagem de usuario de
# base > 1 GiB p/ RAM baixa via PMM e a roda em ring 3. Nao usa PEB/TEB (int 0x80).
$hihello = Join-Path $ex 'hihello.c'
if (Test-Path $hihello) {
    Write-Host "[exemplo] apps\hihello.c -> build\hihello.exe (ImageBase alto 0x140000000 + .reloc)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x140000000' '-Wl,--dynamicbase' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'hihello.exe') $hihello -lkernel32
    if ($LASTEXITCODE) { throw "Compilacao do hihello.exe falhou." }
}

# FRENTE 3 (Fase 3a) — prova do TEB/PEB: le gs:[0x30]/gs:[0x60]/PEB->ImageBase em
# ring 3. ImageBase baixo 0x1900000 (sem relocacao) p/ isolar do teste de reloc.
$tebtest = Join-Path $ex 'tebtest.c'
if (Test-Path $tebtest) {
    Write-Host "[exemplo] apps\tebtest.c -> build\tebtest.exe (prova TEB/PEB)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1900000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'tebtest.exe') $tebtest -lkernel32
    if ($LASTEXITCODE) { throw "Compilacao do tebtest.exe falhou." }
}

# FRENTE 3 (Fase 3b) — crthello.exe: um .exe com o CRT REAL do mingw (SEM -nostdlib).
# O startup do CRT (argv/env/_initterm/exit/__C_specific_handler) roda via a nossa
# ucrtbase.dll (apisets api-ms-win-crt-* redirecionados pelo loader); main() escreve
# via WriteFile. Roda com:
#   run.ps1 -Modules build\ntdll.dll,build\kernel32.dll,build\user32.dll,build\ucrtbase.dll,build\crthello.exe
$crthello = Join-Path $ex 'crthello.c'
if (Test-Path $crthello) {
    Write-Host "[exemplo] apps\crthello.c -> build\crthello.exe (.exe com CRT REAL do mingw)"
    & $zig cc -target x86_64-windows-gnu -o (Join-Path $out 'crthello.exe') $crthello
    if ($LASTEXITCODE) { throw "Compilacao do crthello.c falhou." }
}

# FRENTE 3 (Fase 3c) — echoin.exe: um .exe com o CRT REAL do mingw que LE do teclado
# via scanf() de verdade (entrada), alem de imprimir via printf (saida da Fase 3b). A
# ucrtbase.dll ganhou getchar/fgetc/fgets/fread/__stdio_common_vfscanf; o bloqueio da
# leitura e' feito em ring 3 (a ucrtbase gira em ReadFile ate a tecla chegar). Roda com:
#   run.ps1 -Modules build\ntdll.dll,build\kernel32.dll,build\user32.dll,build\ucrtbase.dll,build\echoin.exe -SendKeys "4 2 spc m e u o s ret"
$echoin = Join-Path $ex 'echoin.c'
if (Test-Path $echoin) {
    Write-Host "[exemplo] apps\echoin.c -> build\echoin.exe (.exe com CRT REAL: scanf le teclado)"
    & $zig cc -target x86_64-windows-gnu -o (Join-Path $out 'echoin.exe') $echoin
    if ($LASTEXITCODE) { throw "Compilacao do echoin.c falhou." }
}

# FRENTE 3 (Fase 3d) — filecat.exe: um .exe com CRT REAL que le/escreve ARQUIVOS reais
# no NTFS via fopen/fread/fwrite/fclose. A ucrtbase.dll ganhou a camada FILE (abre via
# CreateFileA -> NtCreateFile -> volume NTFS). Exige disco. Roda com:
#   run.ps1 -Disk -Modules build\ntdll.dll,build\kernel32.dll,build\user32.dll,build\ucrtbase.dll,build\filecat.exe
$filecat = Join-Path $ex 'filecat.c'
if (Test-Path $filecat) {
    Write-Host "[exemplo] apps\filecat.c -> build\filecat.exe (.exe com CRT REAL: fopen/fread/fwrite NTFS)"
    & $zig cc -target x86_64-windows-gnu -o (Join-Path $out 'filecat.exe') $filecat
    if ($LASTEXITCODE) { throw "Compilacao do filecat.c falhou." }
}

# FRENTE 3 (Fase 3e) — guihello.exe: uma app GRAFICA "de terceiro" ESTRUTURA PADRAO do
# Windows (WinMain + RegisterClass + CreateWindowEx + loop de msg + WM_PAINT), compilada
# com o CRT REAL do mingw no subsistema WINDOWS (WinMainCRTStartup chama WinMain). Roda no
# win32k proprio do MeuOS. user32 ganhou UpdateWindow; kernel32 GetStartupInfoA; ucrtbase
# __p__acmdln/_ismbblead. Prova com screendump. Roda com:
#   run.ps1 -Screendump -Modules build\ntdll.dll,build\kernel32.dll,build\user32.dll,build\gdi32.dll,build\ucrtbase.dll,build\guihello.exe
$guihello = Join-Path $ex 'guihello.c'
if (Test-Path $guihello) {
    Write-Host "[exemplo] apps\guihello.c -> build\guihello.exe (.exe GUI com CRT REAL, WinMain)"
    & $zig cc -target x86_64-windows-gnu '-Wl,--subsystem,windows' `
        -o (Join-Path $out 'guihello.exe') $guihello -luser32 -lgdi32
    if ($LASTEXITCODE) { throw "Compilacao do guihello.c falhou." }
}

# FRENTE 3 (Fase 3f) — loadlib.exe: um .exe com CRT REAL que carrega a testlib.dll EM
# RUNTIME (LoadLibraryA + GetProcAddress) e chama POR PONTEIRO. kernel32 ganhou LoadLibraryA
# -> ntdll LdrLoadDll -> syscall SYS_LOADLIBRARY -> ldr_load. Roda com:
#   run.ps1 -Modules build\ntdll.dll,build\kernel32.dll,build\ucrtbase.dll,build\testlib.dll,build\loadlib.exe
$loadlib = Join-Path $ex 'loadlib.c'
if (Test-Path $loadlib) {
    Write-Host "[exemplo] apps\loadlib.c -> build\loadlib.exe (.exe CRT REAL: LoadLibrary runtime)"
    & $zig cc -target x86_64-windows-gnu -o (Join-Path $out 'loadlib.exe') $loadlib -lkernel32
    if ($LASTEXITCODE) { throw "Compilacao do loadlib.c falhou." }
}

# FASE 3 — DEMO Named Pipes (IPC): servidor cria \Pipe\Nome e escreve; cliente
# abre pelo nome e le os mesmos bytes. Importam kernel32 + user32 (GetStdHandle).
# ImageBases livres e fora do heap (0x2000000..0x3000000) e da regiao do PMM
# (>=0x4000000): servidor 0x1E00000; cliente 0x3000000 (zona morta 48-64 MiB).
$pipesrv = Join-Path $ex 'pipeserver.c'
if (Test-Path $pipesrv) {
    Write-Host "[exemplo] apps\pipeserver.c -> build\pipeserver.exe (.exe Named Pipe servidor)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1E00000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'pipeserver.exe') $pipesrv -lkernel32 -luser32
    if ($LASTEXITCODE) { throw "Compilacao do pipeserver.c falhou." }
}
$pipecli = Join-Path $ex 'pipeclient.c'
if (Test-Path $pipecli) {
    Write-Host "[exemplo] apps\pipeclient.c -> build\pipeclient.exe (.exe Named Pipe cliente)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x3000000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'pipeclient.exe') $pipecli -lkernel32 -luser32
    if ($LASTEXITCODE) { throw "Compilacao do pipeclient.c falhou." }
}

# FASE 2 — DEMO GUI (.exe): RegisterClass + CreateWindowEx + loop de mensagens;
# no WM_PAINT desenha (FillRect + TextOut). Importa kernel32 + user32 + gdi32.
# ImageBase 0x1C00000 (livre). Sera carregado por ULTIMO (a janela fica na tela).
$guiapp = Join-Path $ex 'guiapp.c'
if (Test-Path $guiapp) {
    Write-Host "[exemplo] apps\guiapp.c -> build\guiapp.exe (.exe GUI win32k)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1C00000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'guiapp.exe') $guiapp -lkernel32 -luser32 -lgdi32
    if ($LASTEXITCODE) { throw "Compilacao do guiapp.c falhou." }
}

# Aplicativo de IOCTL (.exe): abre o dispositivo e faz DeviceIoControl.
$ioapp = Join-Path $ex 'ioctlapp.c'
if (Test-Path $ioapp) {
    Write-Host "[exemplo] apps\ioctlapp.c -> build\ioctlapp.exe"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1000000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'ioctlapp.exe') $ioapp -lkernel32 -luser32
    if ($LASTEXITCODE) { throw "Compilacao do ioctlapp.c falhou." }
}

# 0c) DLLs do sistema (PE reais COM export table), igual ntdll/kernel32/user32.
#     ImageBases distintos (sem relocacao); ntdll gera a import-lib usada pelas outras.
$dll = Join-Path $root 'dll'
# FASE 8 (organizacao NT-style): cada DLL agora vive em sua propria subpasta
# (dll/ntdll/ntdll.c, dll/win32/kernel32/kernel32.c, ...).
$ntdllSrc    = Join-Path $dll 'ntdll\ntdll.c'
$kernel32Src = Join-Path $dll 'win32\kernel32\kernel32.c'
$user32Src   = Join-Path $dll 'win32\user32\user32.c'
$gdi32Src    = Join-Path $dll 'win32\gdi32\gdi32.c'
$advapi32Src = Join-Path $dll 'win32\advapi32\advapi32.c'
$ucrtbaseSrc = Join-Path $dll 'win32\ucrtbase\ucrtbase.c'   # FASE 3b: CRT minimo
$ddrawSrc    = Join-Path $dll 'win32\ddraw\ddraw.c'
$d3dSrc      = Join-Path $dll 'win32\d3d9\d3d9.c'
$dxgiSrc     = Join-Path $dll 'win32\dxgi\dxgi.c'
$d3d11Src    = Join-Path $dll 'win32\d3d11\d3d11.c'
$d3d12Src    = Join-Path $dll 'win32\d3d12\d3d12.c'
$d2d1Src     = Join-Path $dll 'win32\d2d1\d2d1.c'
$dwriteSrc   = Join-Path $dll 'win32\dwrite\dwrite.c'
$dxcoreSrc   = Join-Path $dll 'win32\dxcore\dxcore.c'
# FASE 11 (audio stack) — DLLs novas: mmdevapi/Audioses/dsound/winmm.
$mmdevapiSrc = Join-Path $dll 'win32\mmdevapi\mmdevapi.c'
$audiosesSrc = Join-Path $dll 'win32\audioses\audioses.c'
$dsoundSrc   = Join-Path $dll 'win32\dsound\dsound.c'
$winmmSrc    = Join-Path $dll 'win32\winmm\winmm.c'
# FASE 12 (network stack) — Winsock 2.2 ring 3 (ws2_32).
$ws232Src    = Join-Path $dll 'win32\ws2_32\ws2_32.c'
# RODADA FINAL — secur32 (SSPI + LSA Logon API) + credui (Credential UI).
$secur32Src  = Join-Path $dll 'win32\secur32\secur32.c'
$creduiSrc   = Join-Path $dll 'win32\credui\credui.c'
# FASE 3f — testlib: DLL de teste carregada em RUNTIME (LoadLibrary + GetProcAddress).
$testlibSrc  = Join-Path $dll 'win32\testlib\testlib.c'
if (Test-Path $ntdllSrc) {
    $dc = @('-target','x86_64-windows-gnu','-shared','-nostdlib','-e','DllMain')
    Write-Host "[dll] ntdll.dll + kernel32.dll + user32.dll + gdi32.dll + advapi32.dll + ddraw.dll + d3d9.dll + dxgi.dll + d3d11.dll + d3d12.dll + d2d1.dll + dwrite.dll + dxcore.dll + mmdevapi.dll + Audioses.dll + dsound.dll + winmm.dll + ws2_32.dll"
    & $zig cc @dc '-Wl,--image-base=0xA00000' "-Wl,--out-implib,$(Join-Path $out 'libntdll.a')" `
        -o (Join-Path $out 'ntdll.dll') $ntdllSrc
    if ($LASTEXITCODE) { throw "ntdll.dll falhou." }
    & $zig cc @dc '-Wl,--image-base=0xC00000' "-Wl,--out-implib,$(Join-Path $out 'libkernel32.a')" `
        -o (Join-Path $out 'kernel32.dll') $kernel32Src (Join-Path $out 'libntdll.a')
    if ($LASTEXITCODE) { throw "kernel32.dll falhou." }
    & $zig cc @dc '-Wl,--image-base=0xE00000' "-Wl,--out-implib,$(Join-Path $out 'libuser32.a')" `
        -o (Join-Path $out 'user32.dll') $user32Src (Join-Path $out 'libntdll.a')
    if ($LASTEXITCODE) { throw "user32.dll falhou." }
    # gdi32.dll: API GDI (TextOutA/FillRect/GetStockObject/CreateSolidBrush) -> ntdll.
    #   ImageBase 0x1A00000 (livre; ver a lista de bases ja usadas no README).
    & $zig cc @dc '-Wl,--image-base=0x1A00000' "-Wl,--out-implib,$(Join-Path $out 'libgdi32.a')" `
        -o (Join-Path $out 'gdi32.dll') $gdi32Src (Join-Path $out 'libntdll.a')
    if ($LASTEXITCODE) { throw "gdi32.dll falhou." }
    # advapi32.dll (FASE 4): Service Control Manager (stubs) + registro (Reg*) -> ntdll.
    #   ImageBase 0x3200000 (zona morta 48-64 MiB, fora do heap [0x2000000,0x3000000)
    #   e da regiao do PMM >=0x4000000; nao colide com nenhum modulo).
    & $zig cc @dc '-Wl,--image-base=0x3200000' "-Wl,--out-implib,$(Join-Path $out 'libadvapi32.a')" `
        -o (Join-Path $out 'advapi32.dll') $advapi32Src (Join-Path $out 'libntdll.a')
    if ($LASTEXITCODE) { throw "advapi32.dll falhou." }
    # FASE 3b (Frente 3) — ucrtbase.dll: CRT minimo (startup do mingw/UCRT). O loader
    #   redireciona os apisets api-ms-win-crt-* p/ ca. Importa ExitProcess do kernel32.
    #   ImageBase 0x3300000 (livre, zona morta 48-64 MiB; nao colide com nenhum modulo).
    if (Test-Path $ucrtbaseSrc) {
        Write-Host "[dll] ucrtbase.dll (CRT minimo p/ .exe real)"
        & $zig cc @dc '-Wl,--image-base=0x3300000' "-Wl,--out-implib,$(Join-Path $out 'libucrtbase.a')" `
            -o (Join-Path $out 'ucrtbase.dll') $ucrtbaseSrc (Join-Path $out 'libkernel32.a')
        if ($LASTEXITCODE) { throw "ucrtbase.dll falhou." }
    }
    # ddraw.dll: pulado na 1a passada — sera construido APOS d3d11.dll (que
    # produz libd3d11.a). Antes esta secao tentava construir ddraw aqui mesmo;
    # em -Clean builds o link falhava porque D3D11CreateDevice e dllimport.
    # Reorganizado: d3d11 primeiro, ddraw depois.

    # d3d9.dll (FASE 9.4): Direct3D 7/8/9 stub. Acoplado a ddraw.dll: cria objetos
    #   COM falsos (IDirect3D7 e IDirect3DDevice7) que devolvem D3D_OK em quase
    #   tudo. Sem rasterizador real (o win32k ja e dono do FB). Nao depende de
    #   ntdll (so usa _tls_index + DllMain), entao linka sozinho.
    #   ImageBase 0x3C00000 (60 MiB; livre entre calller.sys 0x3A00000 e
    #   ddraw 0x3E00000 — ainda dentro da zona morta 48-64 MiB, fora da regiao
    #   do PMM >=0x4000000). Pode ser carregado por apps que importem d3d.
    if (Test-Path $d3dSrc) {
        & $zig cc @dc '-Wl,--image-base=0x3C00000' "-Wl,--out-implib,$(Join-Path $out 'libd3d9.a')" `
            -o (Join-Path $out 'd3d9.dll') $d3dSrc
        if ($LASTEXITCODE) { throw "d3d9.dll falhou." }
    }
    # dxgi.dll (FASE 9.7+): DXGI 1.x stub. E a infraestrutura comum por baixo de
    #   D3D10/11/12 do Windows moderno: enumera adapters/outputs, cria swap chains
    #   e expoe vtable COM completa para apps DXGI. ImageBase 0x3F00000 (entre
    #   ddraw 0x3E00000 e o PMM 0x4000000 — ddraw so ocupa ~140 KiB a partir de
    #   0x3E00000, entao 0x3F00000 nao colide). Nao depende de ntdll (so usa
    #   _tls_index + DllMain), linka sozinho.
    if (Test-Path $dxgiSrc) {
        & $zig cc @dc '-Wl,--image-base=0x3F00000' "-Wl,--out-implib,$(Join-Path $out 'libdxgi.a')" `
            -o (Join-Path $out 'dxgi.dll') $dxgiSrc
        if ($LASTEXITCODE) { throw "dxgi.dll falhou." }
    }
    # d3d11.dll (FASE 9.8): Direct3D 11 stub. API grafica PRIMARIA do Win8+; vtable
    #   COM completa de ID3D11Device + ID3D11DeviceContext + recursos. ImageBase
    #   0x4500000 — colide com PMM_BASE 0x4000000, ENTAO usamos --dynamicbase para
    #   gerar .reloc e deixar o loader realocar para um endereco virtual livre
    #   (mesmo mecanismo de drivers .sys e hello32.exe).
    if (Test-Path $d3d11Src) {
        & $zig cc @dc '-Wl,--image-base=0x4500000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libd3d11.a')" `
            -o (Join-Path $out 'd3d11.dll') $d3d11Src
        if ($LASTEXITCODE) { throw "d3d11.dll falhou." }
    }
    # FASE 9.10 — re-linkagem do ddraw.dll: agora que libd3d11.a existe, refazemos
    # o link do ddraw para PUXAR D3D11CreateDevice como import. Sem isto a 1a
    # passada teria caido no ramo "sem libd3d11" e o shim nao funcionaria. A
    # segunda passada sobrescreve build\ddraw.dll com a versao definitiva
    # (compat mode Win10+: ddraw -> d3d11).
    if ((Test-Path $ddrawSrc) -and (Test-Path (Join-Path $out 'libd3d11.a'))) {
        Write-Host "[dll] ddraw.dll re-link (compat shim -> d3d11.dll)"
        & $zig cc @dc '-Wl,--image-base=0x3E00000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libddraw.a')" `
            -o (Join-Path $out 'ddraw.dll') $ddrawSrc (Join-Path $out 'libd3d11.a')
        if ($LASTEXITCODE) { throw "ddraw.dll re-link falhou." }
    }
    # d3d12.dll (FASE 9.8): Direct3D 12 stub. API grafica PRIMARIA do Win10+; vtable
    #   COM completa de ID3D12Device + ID3D12CommandQueue + ID3D12CommandList +
    #   recursos + fences. ImageBase 0x4600000 — tambem colide com PMM, mesma
    #   estrategia (--dynamicbase + .reloc) que d3d11.
    if (Test-Path $d3d12Src) {
        & $zig cc @dc '-Wl,--image-base=0x4600000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libd3d12.a')" `
            -o (Join-Path $out 'd3d12.dll') $d3d12Src
        if ($LASTEXITCODE) { throw "d3d12.dll falhou." }
    }
    # d2d1.dll (FASE 9.9): Direct2D stub. API 2D moderna do Win7+ (acima de D3D10/11);
    #   vtable COM completa de ID2D1Factory + ID2D1RenderTarget + ID2D1Brush +
    #   ID2D1Bitmap + ID2D1Geometry. Sem rasterizador real (BasicDisplay e dono do FB),
    #   so ABI completo. ImageBase 0x4700000 — colide com PMM 0x4000000, mesma
    #   estrategia (--dynamicbase + .reloc) de d3d11/d3d12.
    if (Test-Path $d2d1Src) {
        & $zig cc @dc '-Wl,--image-base=0x4700000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libd2d1.a')" `
            -o (Join-Path $out 'd2d1.dll') $d2d1Src
        if ($LASTEXITCODE) { throw "d2d1.dll falhou." }
    }
    # dwrite.dll (FASE 9.9): DirectWrite stub. API moderna de texto do Win7+ (acima
    #   do GDI/Uniscribe); vtable COM completa de IDWriteFactory + IDWriteTextFormat
    #   + IDWriteTextLayout + IDWriteFontCollection + IDWriteFontFamily + IDWriteFont.
    #   Sem fontes reais (font8x8 do bitmap em modo texto e o suficiente), so metricas
    #   estimadas. ImageBase 0x4800000 — colide com PMM, mesma estrategia (--dynamicbase
    #   + .reloc) de d3d11/d3d12/d2d1.
    if (Test-Path $dwriteSrc) {
        & $zig cc @dc '-Wl,--image-base=0x4800000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libdwrite.a')" `
            -o (Join-Path $out 'dwrite.dll') $dwriteSrc
        if ($LASTEXITCODE) { throw "dwrite.dll falhou." }
    }
    # dxcore.dll (FASE 9.9): DXCore stub. Alternativa leve ao DXGI introduzida no
    #   Windows 11 para enumeracao de adapters (sem swap chains, sem outputs). vtable
    #   COM completa de IDXCoreAdapterFactory + IDXCoreAdapterList + IDXCoreAdapter
    #   com GetProperty para HardwareID/DriverDescription/luid/memorias/etc. Ideal
    #   para apps DirectML/D3D12 compute headless. ImageBase 0x4900000 — colide com
    #   PMM, mesma estrategia (--dynamicbase + .reloc) das outras DLLs DX desta fase.
    if (Test-Path $dxcoreSrc) {
        & $zig cc @dc '-Wl,--image-base=0x4900000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libdxcore.a')" `
            -o (Join-Path $out 'dxcore.dll') $dxcoreSrc
        if ($LASTEXITCODE) { throw "dxcore.dll falhou." }
    }
    # FASE 11 (audio stack) — mmdevapi.dll (Multimedia Device API; Win Vista+).
    #   IMMDeviceEnumerator + IMMDevice + IMMDeviceCollection (apenas ABI COM
    #   completo; sem PCM real). ImageBase 0x4A00000 (zona livre apos dxcore).
    if (Test-Path $mmdevapiSrc) {
        & $zig cc @dc '-Wl,--image-base=0x4A00000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libmmdevapi.a')" `
            -o (Join-Path $out 'mmdevapi.dll') $mmdevapiSrc
        if ($LASTEXITCODE) { throw "mmdevapi.dll falhou." }
    }
    # FASE 11 — Audioses.dll (Audio Session API / WASAPI; Win Vista+).
    #   IAudioClient + IAudioRenderClient + IAudioCaptureClient + IAudioClock
    #   + IAudioStreamVolume + ISimpleAudioVolume. ImageBase 0x4B00000.
    if (Test-Path $audiosesSrc) {
        & $zig cc @dc '-Wl,--image-base=0x4B00000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libaudioses.a')" `
            -o (Join-Path $out 'Audioses.dll') $audiosesSrc
        if ($LASTEXITCODE) { throw "Audioses.dll falhou." }
    }
    # FASE 11 — dsound.dll (DirectSound; DX1+). IDirectSound8 + IDirectSoundBuffer8.
    #   ImageBase 0x4C00000.
    if (Test-Path $dsoundSrc) {
        & $zig cc @dc '-Wl,--image-base=0x4C00000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libdsound.a')" `
            -o (Join-Path $out 'dsound.dll') $dsoundSrc
        if ($LASTEXITCODE) { throw "dsound.dll falhou." }
    }
    # FASE 11 — winmm.dll (Windows Multimedia API legada). PlaySoundA/W,
    #   waveOut*, timeGetTime, mciSendString. ImageBase 0x4D00000. Nao linka
    #   contra ntdll (autocontido — para evitar imports circulares).
    if (Test-Path $winmmSrc) {
        & $zig cc @dc '-Wl,--image-base=0x4D00000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libwinmm.a')" `
            -o (Join-Path $out 'winmm.dll') $winmmSrc
        if ($LASTEXITCODE) { throw "winmm.dll falhou." }
    }
    # FASE 12 (network stack) — ws2_32.dll (Winsock 2.2). socket/bind/listen/
    #   accept/connect/send/recv/select + DNS stubs. ImageBase 0x4E00000 (zona
    #   livre apos winmm 0x4D00000). Autocontido (sem ntdll para evitar imports
    #   circulares). --dynamicbase + .reloc (mesma estrategia das outras DLLs
    #   >= PMM_BASE 0x4000000).
    if (Test-Path $ws232Src) {
        & $zig cc @dc '-Wl,--image-base=0x4E00000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libws2_32.a')" `
            -o (Join-Path $out 'ws2_32.dll') $ws232Src
        if ($LASTEXITCODE) { throw "ws2_32.dll falhou." }
    }
    # RODADA FINAL — secur32.dll (SSPI + LSA Logon API). Stubs AcquireCredentials/
    #   InitializeSecurityContext/AcceptSecurityContext/Encrypt/DecryptMessage +
    #   LsaConnectUntrusted/LsaLookupAuthenticationPackage/LsaCallAuthenticationPackage.
    #   ImageBase 0x4F00000 (zona livre apos ws2_32). Autocontido (sem ntdll).
    if (Test-Path $secur32Src) {
        & $zig cc @dc '-Wl,--image-base=0x4F00000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libsecur32.a')" `
            -o (Join-Path $out 'secur32.dll') $secur32Src
        if ($LASTEXITCODE) { throw "secur32.dll falhou." }
    }
    # RODADA FINAL — credui.dll (Credential UI helper). Stubs
    #   CredUIPromptForCredentialsA/W + CredRead/CredWrite/CredEnumerate.
    #   ImageBase 0x5000000 (zona livre apos secur32). Autocontido (sem ntdll).
    if (Test-Path $creduiSrc) {
        & $zig cc @dc '-Wl,--image-base=0x5000000' '-Wl,--dynamicbase' `
            "-Wl,--out-implib,$(Join-Path $out 'libcredui.a')" `
            -o (Join-Path $out 'credui.dll') $creduiSrc
        if ($LASTEXITCODE) { throw "credui.dll falhou." }
    }
    # FASE 3f — testlib.dll: DLL de teste carregada EM RUNTIME por loadlib.exe (LoadLibrary
    #   + GetProcAddress + chamada por ponteiro). ImageBase 0x5100000 (livre apos credui) +
    #   --dynamicbase (>= PMM_BASE). Sem out-implib: NAO e' linkada estaticamente.
    if (Test-Path $testlibSrc) {
        Write-Host "[dll] testlib.dll (DLL de teste p/ LoadLibrary runtime)"
        & $zig cc @dc '-Wl,--image-base=0x5100000' '-Wl,--dynamicbase' `
            -o (Join-Path $out 'testlib.dll') $testlibSrc
        if ($LASTEXITCODE) { throw "testlib.dll falhou." }
    }
}

# FASE 9.4 — DEMO Direct3D 7 (.exe): GDI (CreateWindow + FillRect + TextOut) +
# DirectDraw (DirectDrawCreate7 + CreateSurface) + Direct3D (Direct3DCreate7 +
# CreateDevice + BeginScene/Clear/DrawPrimitive/EndScene). Compilado DEPOIS das
# DLLs e linkado contra TODAS as nossas import-libs (libntdll/kernel32/user32/
# gdi32/ddraw/d3d). ImageBase 0x400000 (4 MiB): faixa livre entre o kernel
# (0x100000-~0x1D5000) e o USTACK (0x600000). NAO usa 0x4200000 (PMM_BASE 64
# MiB; qualquer base >= 0x4000000 colide com frames gerenciados pelo PMM,
# corrompendo o bitmap). pd[2] do PD: livre, sem nenhum outro modulo.
$dxdemoApp = Join-Path $ex 'dxdemo\dxdemo.c'
if ((Test-Path $dxdemoApp) -and (Test-Path (Join-Path $out 'libd3d9.a')) -and `
    (Test-Path (Join-Path $out 'libddraw.a'))) {
    Write-Host "[exemplo] apps\dxdemo\dxdemo.c -> build\dxdemo.exe (.exe GDI + DirectDraw + D3D)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x400000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'dxdemo.exe') $dxdemoApp `
        (Join-Path $out 'libkernel32.a') (Join-Path $out 'libuser32.a') `
        (Join-Path $out 'libgdi32.a')    (Join-Path $out 'libntdll.a') `
        (Join-Path $out 'libddraw.a')    (Join-Path $out 'libd3d9.a')
    if ($LASTEXITCODE) { throw "Compilacao do dxdemo\dxdemo.c falhou." }
}

# FASE 9.10 — DEMO Direct3D 11 / WDDM 2.x (.exe): CreateWindow + DXGI factory
# + EnumAdapters + D3D11CreateDeviceAndSwapChain + ClearRenderTargetView +
# Present. Linka contra libdxgi.a + libd3d11.a + libkernel32/libuser32/libgdi32/
# libntdll. ImageBase 0x420000: faixa baixa livre entre o kernel
# (0x100000-~0x1D5000) e o USTACK (0x600000); 128 KiB depois de 0x400000 (dxdemo)
# — distancia segura para evitar colisao.
$d3d11demoApp = Join-Path $ex 'd3d11demo\d3d11demo.c'
if ((Test-Path $d3d11demoApp) -and (Test-Path (Join-Path $out 'libdxgi.a')) -and `
    (Test-Path (Join-Path $out 'libd3d11.a'))) {
    Write-Host "[exemplo] apps\d3d11demo\d3d11demo.c -> build\d3d11demo.exe (.exe DXGI + D3D11)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x420000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'd3d11demo.exe') $d3d11demoApp `
        (Join-Path $out 'libkernel32.a') (Join-Path $out 'libuser32.a') `
        (Join-Path $out 'libgdi32.a')    (Join-Path $out 'libntdll.a') `
        (Join-Path $out 'libdxgi.a')     (Join-Path $out 'libd3d11.a')
    if ($LASTEXITCODE) { throw "Compilacao do d3d11demo\d3d11demo.c falhou." }
}

# FASE 4 — DEMO informacao do sistema (.exe): NtQuerySystemInformation (versao do
# SO + num de CPUs) + NtQueryInformationProcess (pid/base) + advapi32 (registro
# Reg* e stubs do SCM). Compilado DEPOIS das DLLs para linkar contra as NOSSAS
# import-libs (libntdll.a/libkernel32.a/libadvapi32.a) — assim os nomes batem
# exatamente com os exports das nossas DLLs (o loader as resolve em runtime).
# ImageBase 0x3400000 (zona morta 48-64 MiB; ao lado do advapi32, sem colidir).
$sysinfo = Join-Path $ex 'sysinfo.c'
if ((Test-Path $sysinfo) -and (Test-Path (Join-Path $out 'libadvapi32.a'))) {
    Write-Host "[exemplo] apps\sysinfo.c -> build\sysinfo.exe (.exe NtQuery* + advapi32)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x3400000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'sysinfo.exe') $sysinfo `
        (Join-Path $out 'libkernel32.a') (Join-Path $out 'libntdll.a') (Join-Path $out 'libadvapi32.a')
    if ($LASTEXITCODE) { throw "Compilacao do sysinfo.c falhou." }
}

# FASE 5 — SHELL cmd.exe (.exe ring 3): help / tasklist / sc query / sc start /
# sc stop / dir. Usa exports PROPRIOS do nosso kernel32 (EnumProcessesEx,
# EnumDriversEx, StartDriverServiceA, StopDriverServiceA), entao e compilado
# DEPOIS das DLLs e linkado contra a NOSSA libkernel32.a (os nomes batem com os
# exports da nossa kernel32.dll; o loader os resolve em runtime pela export table).
# ImageBase 0x3600000 (zona morta 48-64 MiB; ao lado de advapi32/sysinfo, sem colidir).
$cmd = Join-Path $ex 'cmd.c'
if ((Test-Path $cmd) -and (Test-Path (Join-Path $out 'libkernel32.a'))) {
    Write-Host "[exemplo] apps\cmd.c -> build\cmd.exe (.exe shell FASE 5)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x3600000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'cmd.exe') $cmd `
        (Join-Path $out 'libkernel32.a') (Join-Path $out 'libntdll.a')
    if ($LASTEXITCODE) { throw "Compilacao do cmd.c falhou." }
}

# FASE 6 — DESKTOP (.exe ring 3): papel de parede + barra de tarefas + cmd numa
# janela (multiplas janelas de console). Usa kernel32 (Enum*/Start/StopDriver),
# user32 (janelas + SetFocus + CreateConsoleWindow + CreateDesktopWindow) e gdi32
# (TextOutA + SetTextColor). Compilado DEPOIS das DLLs e linkado contra as NOSSAS
# import-libs (os nomes batem com os exports proprios; o loader os resolve em
# runtime). ImageBase 0x3800000 (zona morta 48-64 MiB; ao lado de cmd, sem colidir).
$desktop = Join-Path $ex 'desktop.c'
if ((Test-Path $desktop) -and (Test-Path (Join-Path $out 'libgdi32.a'))) {
    Write-Host "[exemplo] apps\desktop.c -> build\desktop.exe (.exe desktop FASE 6)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x3800000' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'desktop.exe') $desktop `
        (Join-Path $out 'libkernel32.a') (Join-Path $out 'libuser32.a') `
        (Join-Path $out 'libgdi32.a') (Join-Path $out 'libntdll.a')
    if ($LASTEXITCODE) { throw "Compilacao do desktop.c falhou." }
}

# RODADA FINAL — csrss.exe (Client/Server Runtime Subsystem stub).
# Apenas loga init/exit. ImageBase 0x5200000 (entre winlogon e win10ui).
$csrss = Join-Path $ex 'csrss\csrss.c'
if ((Test-Path $csrss) -and (Test-Path (Join-Path $out 'libkernel32.a'))) {
    Write-Host "[exemplo] apps\csrss\csrss.c -> build\csrss.exe (RODADA FINAL)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x5200000' '-Wl,--dynamicbase' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'csrss.exe') $csrss `
        (Join-Path $out 'libkernel32.a') (Join-Path $out 'libntdll.a')
    if ($LASTEXITCODE) { throw "Compilacao do csrss.c falhou." }
}

# RODADA FINAL — winlogon.exe (Logon Process: renderiza tela de logon).
# Importa kernel32 + user32 + gdi32 + ntdll + secur32 + credui.
# ImageBase 0x5100000 (zona livre apos credui 0x5000000).
$winlogon = Join-Path $ex 'winlogon\winlogon.c'
if ((Test-Path $winlogon) -and (Test-Path (Join-Path $out 'libsecur32.a')) -and `
    (Test-Path (Join-Path $out 'libcredui.a'))) {
    Write-Host "[exemplo] apps\winlogon\winlogon.c -> build\winlogon.exe (RODADA FINAL)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x5100000' '-Wl,--dynamicbase' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'winlogon.exe') $winlogon `
        (Join-Path $out 'libkernel32.a') (Join-Path $out 'libuser32.a') `
        (Join-Path $out 'libgdi32.a')    (Join-Path $out 'libntdll.a') `
        (Join-Path $out 'libsecur32.a')  (Join-Path $out 'libcredui.a')
    if ($LASTEXITCODE) { throw "Compilacao do winlogon.c falhou." }
}

# RODADA FINAL — win10ui.exe (app de teste final): janela Windows 10 mock
# com Audio/Network/Security/CredUI/FltMgr status. ImageBase 0x5300000.
$win10ui = Join-Path $ex 'win10ui\win10ui.c'
if ((Test-Path $win10ui) -and (Test-Path (Join-Path $out 'libdsound.a')) -and `
    (Test-Path (Join-Path $out 'libws2_32.a')) -and `
    (Test-Path (Join-Path $out 'libsecur32.a')) -and `
    (Test-Path (Join-Path $out 'libcredui.a'))) {
    Write-Host "[exemplo] apps\win10ui\win10ui.c -> build\win10ui.exe (RODADA FINAL teste final)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x5300000' '-Wl,--dynamicbase' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'win10ui.exe') $win10ui `
        (Join-Path $out 'libkernel32.a') (Join-Path $out 'libuser32.a') `
        (Join-Path $out 'libgdi32.a')    (Join-Path $out 'libntdll.a') `
        (Join-Path $out 'libdsound.a')   (Join-Path $out 'libws2_32.a') `
        (Join-Path $out 'libsecur32.a')  (Join-Path $out 'libcredui.a')
    if ($LASTEXITCODE) { throw "Compilacao do win10ui.c falhou." }
}

# explorer.exe — SHELL ring-3 PERSISTENTE (1o incremento do explorer.exe real:
# janela viva que nao e' reaped). ImageBase 0x5400000. Importa user32+gdi32.
$explorer = Join-Path $ex 'explorer\explorer.c'
if ((Test-Path $explorer) -and (Test-Path (Join-Path $out 'libuser32.a'))) {
    Write-Host "[exemplo] apps\explorer\explorer.c -> build\explorer.exe (shell ring-3)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x5400000' '-Wl,--dynamicbase' '-Wl,--subsystem,console' "-I$sdk", "-I$ddk" `
        -o (Join-Path $out 'explorer.exe') $explorer `
        (Join-Path $out 'libkernel32.a') (Join-Path $out 'libuser32.a') `
        (Join-Path $out 'libgdi32.a')    (Join-Path $out 'libntdll.a')
    if ($LASTEXITCODE) { throw "Compilacao do explorer.c falhou." }
}

# 1) Assembly do kernel (NASM -> elf64)
foreach ($a in Get-ChildItem -Path $src -Recurse -Filter *.asm) {
    $o = Get-ObjName $a
    Write-Host "[asm] $($a.Name)"
    & $nasm -f elf64 $a.FullName -o $o
    if ($LASTEXITCODE) { throw "NASM falhou em $($a.Name)." }
    $objs += $o
}

# 2) C do kernel (zig cc, freestanding x86-64). -I sdk: tipos do DDK.
# FASE 8: reorganizacao estilo WRK/ReactOS — agora o include path resolve
# "ke/sync.h" em src/ntos/ke/sync.h, "ob/object.h" em src/ntos/ob/, etc.
# Drivers em src/drivers/ ficam acessiveis via "input/keyboard.h" etc.
$cflags = @(
    '-target','x86_64-freestanding-none','-ffreestanding','-nostdlib',
    '-fno-stack-protector','-fno-pic','-fno-pie','-mno-red-zone',
    "-I$src",
    "-I$(Join-Path $src 'ntos')",          # ke/, mm/, ob/, io/, ps/, cm/, ex/, lpc/, ldr/
    "-I$(Join-Path $src 'ntos\inc')",      # headers privados (io.h)
    "-I$(Join-Path $src 'drivers')",       # input/, video/, serial/, filesystems/
    "-I$(Join-Path $src 'subsystems')",    # win32/
    "-I$sdk", "-I$ddk",
    '-std=c11','-Wall','-Wextra','-O2','-c'
)
foreach ($f in (Get-ChildItem -Path $src -Recurse -Filter *.c)) {
    # FASE 9.10 — modelo NT 6.4+ de split do win32k: o subsistema agora vive em
    # 3 arquivos (win32kbase.c, win32kfull.c, win32k.c). Apenas win32k.c e
    # compilado; ele faz #include dos outros dois para formar UM unico TU
    # (mesmo modelo Reactos quando precisa preservar nomes de simbolos com
    # storage 'static'). Pular os parciais aqui evita duplicidade.
    if ($f.Name -eq 'win32kbase.c' -or $f.Name -eq 'win32kfull.c' -or $f.Name -eq 'win32kshell.c') { continue }
    $o = Get-ObjName $f
    Write-Host "[cc ] $($f.FullName.Substring($src.Length+1))"
    & $zig cc @cflags $f.FullName -o $o
    if ($LASTEXITCODE) { throw "Compilacao de $($f.Name) falhou." }
    $objs += $o
}

# 3) Link
Write-Host "`n[link]    kernel.elf"
$ldflags = @(
    '-target','x86_64-freestanding-none','-nostdlib','-static','-no-pie',
    "-Wl,-T,$(Join-Path $root 'linker.ld')",
    '-Wl,--build-id=none','-Wl,-z,noexecstack'
)
& $zig cc @ldflags @objs -o (Join-Path $out 'kernel.elf')
if ($LASTEXITCODE) { throw "Link falhou." }

# 4) Binario plano para o "-kernel" do QEMU
Write-Host "[objcopy] kernel.bin"
& $zig objcopy -O binary (Join-Path $out 'kernel.elf') (Join-Path $out 'kernel.bin')
if ($LASTEXITCODE) { throw "objcopy falhou." }

Write-Host "`nOK -> build\kernel.bin  (e kernel.elf com simbolos)" -ForegroundColor Green
Write-Host "Rode no QEMU com:  .\run.ps1"
