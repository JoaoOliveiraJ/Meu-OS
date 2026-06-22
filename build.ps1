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

# Programas Windows de exemplo (em examples\) — compilados com o alvo Windows,
# NAO fazem parte do OS. Viram binarios PE soltos em build\, carregados no boot.
$sdk = Join-Path $root 'sdk'
$ex  = Join-Path $root 'examples'

# 0a) Exemplo: um .exe Windows (PE32+ x64).
$hello = Join-Path $ex 'hello.c'
if (Test-Path $hello) {
    Write-Host "[exemplo] examples\hello.c -> build\test.exe (.exe Windows)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x800000' '-Wl,--subsystem,console' "-I$sdk" `
        -o (Join-Path $out 'test.exe') $hello -luser32 -lkernel32
    if ($LASTEXITCODE) { throw "Compilacao do exemplo hello.c falhou." }
}

# 0a2) Exemplo: um .exe Windows de 32 BITS (PE32, x86 / IMAGE_NT_HEADERS32).
#      Alvo x86-windows-gnu -> machine 0x14C, optional magic 0x10B. Base 0x1600000
#      (faixa separada da 64-bit). Autocontido (sem imports): faz int 0x80 direto.
#      Mantemos a .reloc (--dynamicbase) para exercitar as relocacoes no loader.
$hello32 = Join-Path $ex 'hello32.c'
if (Test-Path $hello32) {
    Write-Host "[exemplo] examples\hello32.c -> build\test32.exe (.exe Windows 32-bit / PE32)"
    & $zig cc -target x86-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1600000' '-Wl,--subsystem,console' '-Wl,--dynamicbase' "-I$sdk" `
        -o (Join-Path $out 'test32.exe') $hello32
    if ($LASTEXITCODE) { throw "Compilacao do exemplo hello32.c falhou." }
}

# 0b) Exemplo: um driver de kernel Windows (.sys, subsystem NATIVE).
foreach ($drv in @(
        @{ src = 'mydriver.c';    out = 'mydriver.sys';    base = '0x1200000' },
        @{ src = 'ioctldriver.c'; out = 'ioctldriver.sys'; base = '0x1400000' },
        @{ src = 'calller.c';     out = 'calller.sys';     base = '0x3A00000' })) {
    $sp = Join-Path $ex $drv.src
    if (Test-Path $sp) {
        Write-Host "[exemplo] examples\$($drv.src) -> build\$($drv.out) (.sys driver)"
        & $zig cc -target x86_64-windows-gnu -nostdlib -e DriverEntry `
            '-Wl,--subsystem,native' "-Wl,--image-base=$($drv.base)" "-I$sdk" `
            -o (Join-Path $out $drv.out) $sp -lntoskrnl
        if ($LASTEXITCODE) { throw "Compilacao de $($drv.src) falhou." }
    }
}

# Aplicativo de CONSOLE (.exe): GetStdHandle + WriteFile (via NtWriteFile) +
# GetModuleHandleA/GetProcAddress + MessageBoxA. ImageBase 0x1800000 (livre).
$conhello = Join-Path $ex 'conhello.c'
if (Test-Path $conhello) {
    Write-Host "[exemplo] examples\conhello.c -> build\conhello.exe"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1800000' '-Wl,--subsystem,console' "-I$sdk" `
        -o (Join-Path $out 'conhello.exe') $conhello -lkernel32 -luser32
    if ($LASTEXITCODE) { throw "Compilacao do conhello.c falhou." }
}

# FASE 3 — DEMO Named Pipes (IPC): servidor cria \Pipe\Nome e escreve; cliente
# abre pelo nome e le os mesmos bytes. Importam kernel32 + user32 (GetStdHandle).
# ImageBases livres e fora do heap (0x2000000..0x3000000) e da regiao do PMM
# (>=0x4000000): servidor 0x1E00000; cliente 0x3000000 (zona morta 48-64 MiB).
$pipesrv = Join-Path $ex 'pipeserver.c'
if (Test-Path $pipesrv) {
    Write-Host "[exemplo] examples\pipeserver.c -> build\pipeserver.exe (.exe Named Pipe servidor)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1E00000' '-Wl,--subsystem,console' "-I$sdk" `
        -o (Join-Path $out 'pipeserver.exe') $pipesrv -lkernel32 -luser32
    if ($LASTEXITCODE) { throw "Compilacao do pipeserver.c falhou." }
}
$pipecli = Join-Path $ex 'pipeclient.c'
if (Test-Path $pipecli) {
    Write-Host "[exemplo] examples\pipeclient.c -> build\pipeclient.exe (.exe Named Pipe cliente)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x3000000' '-Wl,--subsystem,console' "-I$sdk" `
        -o (Join-Path $out 'pipeclient.exe') $pipecli -lkernel32 -luser32
    if ($LASTEXITCODE) { throw "Compilacao do pipeclient.c falhou." }
}

# FASE 2 — DEMO GUI (.exe): RegisterClass + CreateWindowEx + loop de mensagens;
# no WM_PAINT desenha (FillRect + TextOut). Importa kernel32 + user32 + gdi32.
# ImageBase 0x1C00000 (livre). Sera carregado por ULTIMO (a janela fica na tela).
$guiapp = Join-Path $ex 'guiapp.c'
if (Test-Path $guiapp) {
    Write-Host "[exemplo] examples\guiapp.c -> build\guiapp.exe (.exe GUI win32k)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1C00000' '-Wl,--subsystem,console' "-I$sdk" `
        -o (Join-Path $out 'guiapp.exe') $guiapp -lkernel32 -luser32 -lgdi32
    if ($LASTEXITCODE) { throw "Compilacao do guiapp.c falhou." }
}

# Aplicativo de IOCTL (.exe): abre o dispositivo e faz DeviceIoControl.
$ioapp = Join-Path $ex 'ioctlapp.c'
if (Test-Path $ioapp) {
    Write-Host "[exemplo] examples\ioctlapp.c -> build\ioctlapp.exe"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x1000000' '-Wl,--subsystem,console' "-I$sdk" `
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
if (Test-Path $ntdllSrc) {
    $dc = @('-target','x86_64-windows-gnu','-shared','-nostdlib','-e','DllMain')
    Write-Host "[dll] ntdll.dll + kernel32.dll + user32.dll + gdi32.dll + advapi32.dll"
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
}

# FASE 4 — DEMO informacao do sistema (.exe): NtQuerySystemInformation (versao do
# SO + num de CPUs) + NtQueryInformationProcess (pid/base) + advapi32 (registro
# Reg* e stubs do SCM). Compilado DEPOIS das DLLs para linkar contra as NOSSAS
# import-libs (libntdll.a/libkernel32.a/libadvapi32.a) — assim os nomes batem
# exatamente com os exports das nossas DLLs (o loader as resolve em runtime).
# ImageBase 0x3400000 (zona morta 48-64 MiB; ao lado do advapi32, sem colidir).
$sysinfo = Join-Path $ex 'sysinfo.c'
if ((Test-Path $sysinfo) -and (Test-Path (Join-Path $out 'libadvapi32.a'))) {
    Write-Host "[exemplo] examples\sysinfo.c -> build\sysinfo.exe (.exe NtQuery* + advapi32)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x3400000' '-Wl,--subsystem,console' "-I$sdk" `
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
    Write-Host "[exemplo] examples\cmd.c -> build\cmd.exe (.exe shell FASE 5)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x3600000' '-Wl,--subsystem,console' "-I$sdk" `
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
    Write-Host "[exemplo] examples\desktop.c -> build\desktop.exe (.exe desktop FASE 6)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e _start `
        '-Wl,--image-base=0x3800000' '-Wl,--subsystem,console' "-I$sdk" `
        -o (Join-Path $out 'desktop.exe') $desktop `
        (Join-Path $out 'libkernel32.a') (Join-Path $out 'libuser32.a') `
        (Join-Path $out 'libgdi32.a') (Join-Path $out 'libntdll.a')
    if ($LASTEXITCODE) { throw "Compilacao do desktop.c falhou." }
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
    "-I$sdk",
    '-std=c11','-Wall','-Wextra','-O2','-c'
)
foreach ($f in (Get-ChildItem -Path $src -Recurse -Filter *.c)) {
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
