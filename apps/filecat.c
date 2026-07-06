// FRENTE 3 (Fase 3d) — ALVO: um .exe com CRT REAL do mingw que mexe em ARQUIVOS
// REAIS no NTFS via fopen/fread/fwrite/fclose. Caminho:
//   fopen  -> CreateFileA -> NtCreateFile -> volume NTFS (\Device\Harddisk0\Partition1)
//   fread  -> ReadFile    -> NtReadFile   -> IRP_MJ_READ  -> ntfs_read_file
//   fwrite -> WriteFile   -> NtWriteFile  -> IRP_MJ_WRITE -> ntfs_write_file
// Exige um disco: run.ps1 -Disk (build\disk.img, gerado por make-ntfs-disk.ps1).
// Compilado por `zig cc -target x86_64-windows-gnu apps/filecat.c` (com o CRT).
#include <stdio.h>

int main(void) {
    // (1) LEITURA: abre C:\hello.txt e le o conteudo real do disco NTFS.
    FILE* f = fopen("C:\\hello.txt", "rb");
    if (!f) { printf("  [filecat] ERRO: fopen(C:\\hello.txt) falhou\n"); return 1; }
    char buf[160];
    unsigned long long n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    printf("  [filecat] li %llu bytes de C:\\hello.txt:\n    \"%s\"\n", n, buf);

    // (2) ESCRITA round-trip: sobrescreve C:\dir1\file.txt com um marcador conhecido,
    //     fecha, reabre e le de volta EXATAMENTE o tamanho escrito -> prova ida-e-volta
    //     fopen/fwrite/fclose/fread num arquivo REAL do disco.
    char marker[] = "MEUOS-FILEIO-OK-123";
    int  mlen = (int)(sizeof(marker) - 1);
    FILE* w = fopen("C:\\dir1\\file.txt", "wb");
    if (!w) { printf("  [filecat] ERRO: fopen(C:\\dir1\\file.txt, wb) falhou\n"); return 1; }
    unsigned long long wn = fwrite(marker, 1, (unsigned long long)mlen, w);
    fclose(w);

    FILE* r = fopen("C:\\dir1\\file.txt", "rb");
    if (!r) { printf("  [filecat] ERRO: reabrir C:\\dir1\\file.txt falhou\n"); return 1; }
    char rb[64];
    unsigned long long m = fread(rb, 1, (unsigned long long)mlen, r);
    rb[m] = 0;
    fclose(r);
    printf("  [filecat] escrevi %llu B \"%s\"; reli %llu B \"%s\"\n", wn, marker, m, rb);
    return 0;
}
