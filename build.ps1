#requires -version 3
<#
  build.ps1  —  Compila o kernel (build\kernel.bin + kernel.elf).
  Varre src\ recursivamente: basta criar arquivos .c/.asm que entram no build.
  Usa NASM (boot/isr) e Zig (zig cc) para o C. Rode setup.ps1 antes.
#>
[CmdletBinding()]
param([switch]$Clean)

$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$src   = Join-Path $root 'src'
$out   = Join-Path $root 'build'
$tools = Join-Path $root 'tools'

if ($Clean -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }
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

# objeto unico por arquivo, nome derivado do caminho relativo (evita colisoes)
function Get-ObjName($file) {
    $rel = $file.FullName.Substring($src.Length).TrimStart('\','/')
    $rel = $rel -replace '[\\/]','_'
    Join-Path $out ([IO.Path]::ChangeExtension($rel, '.o'))
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

# 0b) Exemplo: um driver de kernel Windows (.sys, subsystem NATIVE).
$mydrv = Join-Path $ex 'mydriver.c'
if (Test-Path $mydrv) {
    Write-Host "[exemplo] examples\mydriver.c -> build\mydriver.sys (.sys driver)"
    & $zig cc -target x86_64-windows-gnu -nostdlib -e DriverEntry `
        '-Wl,--subsystem,native' '-Wl,--image-base=0x900000' "-I$sdk" `
        -o (Join-Path $out 'mydriver.sys') $mydrv -lntoskrnl
    if ($LASTEXITCODE) { throw "Compilacao do exemplo mydriver.c falhou." }
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
$cflags = @(
    '-target','x86_64-freestanding-none','-ffreestanding','-nostdlib',
    '-fno-stack-protector','-fno-pic','-fno-pie','-mno-red-zone',
    "-I$src", "-I$(Join-Path $src 'include')", "-I$sdk",
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
