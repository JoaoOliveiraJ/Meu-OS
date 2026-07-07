#requires -version 3
<#
  make-ntfs-disk.ps1  —  Gera build\disk.img: uma imagem de disco RAW com uma
  particao NTFS pequena (default 64 MiB) contendo arquivos de teste conhecidos:
      \hello.txt        (texto conhecido — usado para validar a leitura NTFS)
      \dir1\file.txt    (arquivo dentro de um subdiretorio)

  O kernel anexa essa imagem como disco IDE (run.ps1 -Disk) e a le via ATA PIO
  (src\hal\disk.c): primeiro o MBR (setor 0), depois o boot sector da particao
  NTFS, conferindo a assinatura "NTFS    " no offset 3 do boot sector (FASE 2).

  DOIS modos (escolhidos automaticamente):

   (A) REAL (recomendado, exige Administrador + Hyper-V): cria um VHD, particiona,
       formata como NTFS DE VERDADE (Format-Volume) e copia os arquivos. Produz um
       volume NTFS autentico do Windows; depois converte para RAW com qemu-img.
       Use isto para exercitar o leitor NTFS do kernel contra um NTFS real.

   (B) SINTETICO (fallback, NAO precisa de admin): constroi os bytes do MBR + boot
       sector NTFS + uma $MFT minima na mao (examples\make-ntfs-image.py, Python).
       Suficiente para validar o boot sector e ler \hello.txt (residente). Use
       quando nao for possivel elevar.

  Uso:
    .\examples\make-ntfs-disk.ps1                 # auto: real se admin, senao sintetico
    .\examples\make-ntfs-disk.ps1 -SizeMB 64      # tamanho da imagem
    .\examples\make-ntfs-disk.ps1 -Synthetic      # forca o modo sintetico (sem admin)
    .\examples\make-ntfs-disk.ps1 -Out C:\x.img   # caminho de saida

  Depois:  .\run.ps1 -Headless -Disk -TimeoutSec 14
#>
[CmdletBinding()]
param(
    [string]$Out,
    [int]$SizeMB = 64,
    [switch]$Synthetic        # forca o builder Python (sem admin)
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot   # raiz do projeto (examples\ esta dentro)
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
if (-not $Out) { $Out = Join-Path $build 'disk.img' }

# --- conteudo CONHECIDO (deve casar com make-ntfs-image.py / o teste do kernel) ---
$helloText = "Hello from MeuOS NTFS! Este arquivo foi lido do disco via IDE PIO.`r`n"
$dir1Text  = "Arquivo dentro de dir1.`r`n"

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-QemuImg {
    $c = (Get-Command qemu-img -ErrorAction SilentlyContinue).Source
    if ($c) { return $c }
    foreach ($p in @((Join-Path $root 'tools\qemu\qemu-img.exe'),
                     'C:\Program Files\qemu\qemu-img.exe')) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

function Find-Python {
    foreach ($n in @('python','py','python3')) {
        $c = (Get-Command $n -ErrorAction SilentlyContinue).Source
        if ($c) { return $c }
    }
    return $null
}

# ============================================================================
#  (B) Modo SINTETICO — Python escreve os bytes (sem admin).
# ============================================================================
function New-SyntheticImage {
    Write-Host "[make-ntfs] modo SINTETICO (sem admin): construindo os bytes do NTFS na mao." -ForegroundColor Cyan
    $py = Find-Python
    if (-not $py) { throw "Python nao encontrado (necessario para o modo sintetico). Instale o Python 3 ou rode como Admin para o modo real." }
    $script = Join-Path $PSScriptRoot 'make-ntfs-image.py'
    if (-not (Test-Path $script)) { throw "Faltando: $script" }
    # Embute build\testlib.dll como \testlib.dll (arg 3) — prova LoadLibrary de um ARQUIVO
    # no disco (Fase 3g) — e build\child.exe como \child.exe (arg 4) — prova CreateProcess
    # a partir do disco (Fase 4b). Ambos NAO-residentes. String vazia = arquivo ausente.
    $dllEmbed   = Join-Path $build 'testlib.dll'
    $childEmbed = Join-Path $build 'child.exe'
    $dllArg   = if (Test-Path $dllEmbed)   { $dllEmbed }   else { '' }
    $childArg = if (Test-Path $childEmbed) { $childEmbed } else { '' }
    & $py $script $Out $SizeMB $dllArg $childArg
    if ($LASTEXITCODE) { throw "make-ntfs-image.py falhou (exit $LASTEXITCODE)." }
}

# ============================================================================
#  (A) Modo REAL — VHD + Format-Volume NTFS + Copy-Item + qemu-img convert.
# ============================================================================
function New-RealImage {
    Write-Host "[make-ntfs] modo REAL (admin + Hyper-V): VHD -> NTFS de verdade -> RAW." -ForegroundColor Cyan
    $qimg = Find-QemuImg
    if (-not $qimg) { throw "qemu-img nao encontrado (necessario para converter VHD->RAW). Rode .\setup.ps1 ou use -Synthetic." }

    $vhd = Join-Path $build 'disk.vhd'
    if (Test-Path $vhd) { Remove-Item $vhd -Force }

    # 1) Cria um VHD de tamanho fixo (MBR) e monta.
    #    New-VHD/Mount-VHD/New-Partition/Format-Volume sao do modulo Hyper-V +
    #    Storage (exigem admin). O VHD fixo facilita a conversao para RAW.
    $sizeBytes = [int64]$SizeMB * 1MB
    Write-Host "[make-ntfs] New-VHD -Fixed $vhd ($SizeMB MiB)..."
    New-VHD -Path $vhd -SizeBytes $sizeBytes -Fixed | Out-Null

    $disk = $null
    try {
        $disk = Mount-VHD -Path $vhd -Passthru | Get-Disk
        Initialize-Disk -Number $disk.Number -PartitionStyle MBR -Confirm:$false | Out-Null
        $part = New-Partition -DiskNumber $disk.Number -UseMaximumSize -IsActive `
                    -MbrType IFS   # IFS => tipo de particao 0x07 (NTFS/exFAT)
        $vol  = Format-Volume -Partition $part -FileSystem NTFS -NewFileSystemLabel 'MEUOS' `
                    -Confirm:$false -Force
        # Atribui uma letra temporaria para copiar os arquivos.
        $part | Add-PartitionAccessPath -AssignDriveLetter
        Start-Sleep -Milliseconds 500
        $part = Get-Partition -DiskNumber $disk.Number -PartitionNumber $part.PartitionNumber
        $drv  = $part.DriveLetter
        if (-not $drv) { throw "Nao consegui obter a letra do volume formatado." }
        $rootP = "$drv`:\"

        Write-Host "[make-ntfs] copiando arquivos de teste para $rootP ..."
        Set-Content -Path (Join-Path $rootP 'hello.txt') -Value $helloText -NoNewline -Encoding Ascii
        New-Item -ItemType Directory -Force -Path (Join-Path $rootP 'dir1') | Out-Null
        Set-Content -Path (Join-Path $rootP 'dir1\file.txt') -Value $dir1Text -NoNewline -Encoding Ascii
        # Forca a descarga para o disco.
        (Get-Item (Join-Path $rootP 'hello.txt')).Length | Out-Null
    }
    finally {
        if ($disk) {
            try { Dismount-VHD -Path $vhd -ErrorAction SilentlyContinue } catch {}
        }
    }

    # 2) Converte o VHD para RAW (formato que o QEMU -drive format=raw espera).
    Write-Host "[make-ntfs] qemu-img convert VHD -> RAW ($Out) ..."
    if (Test-Path $Out) { Remove-Item $Out -Force }
    & $qimg convert -f vpc -O raw $vhd $Out
    if ($LASTEXITCODE) { throw "qemu-img convert falhou (exit $LASTEXITCODE)." }
    Remove-Item $vhd -Force -ErrorAction SilentlyContinue
    Write-Host "[make-ntfs] imagem RAW (NTFS real) gerada: $Out"
}

# ============================================================================
#  Seletor de modo.
# ============================================================================
$useReal = (-not $Synthetic) -and (Test-IsAdmin)

if ($Synthetic) {
    New-SyntheticImage
} elseif ($useReal) {
    try {
        New-RealImage
    } catch {
        Write-Host "[make-ntfs] modo real falhou ($($_.Exception.Message)); caindo no modo sintetico." -ForegroundColor Yellow
        New-SyntheticImage
    }
} else {
    Write-Host "[make-ntfs] sem privilegio de Administrador; usando o modo sintetico (sem admin)." -ForegroundColor Yellow
    Write-Host "[make-ntfs] (para um NTFS REAL do Windows, rode este script num PowerShell elevado.)"
    New-SyntheticImage
}

# --- verificacao final: confere o MBR e a assinatura NTFS no boot sector ---
$fs = [System.IO.File]::OpenRead($Out)
try {
    $br = New-Object System.IO.BinaryReader($fs)
    $mbr = $br.ReadBytes(512)
    $ptype = $mbr[0x1BE + 4]
    $plba  = [BitConverter]::ToUInt32($mbr, 0x1BE + 8)
    $sig0  = '{0:X2}{1:X2}' -f $mbr[510], $mbr[511]
    if ($plba -eq 0) { $plba = 0 }   # superfloppy: boot sector no setor 0
    $fs.Seek([int64]$plba * 512, 'Begin') | Out-Null
    $vbr = $br.ReadBytes(512)
    $oem = [System.Text.Encoding]::ASCII.GetString($vbr, 3, 8)
    Write-Host ""
    Write-Host "=== verificacao da imagem ===" -ForegroundColor Green
    Write-Host ("  MBR: assinatura @510 = 0x$sig0 ; particao0 type=0x{0:X2} LBA={1}" -f $ptype, $plba)
    Write-Host ("  Boot sector (LBA $plba): OEM @offset 3 = '$oem'")
    if ($oem.StartsWith('NTFS')) {
        Write-Host "  -> Assinatura NTFS confirmada. Imagem pronta." -ForegroundColor Green
    } else {
        Write-Host "  -> ATENCAO: assinatura NTFS nao encontrada no boot sector!" -ForegroundColor Red
    }
}
finally { $fs.Close() }

Write-Host ""
Write-Host "Pronto: $Out"
Write-Host "Teste no QEMU:  .\run.ps1 -Headless -Disk -TimeoutSec 14"
