/*
 * Disassemble SM1-3 shader blobs with D3DXDisassembleShader.
 *
 *   fxdis.exe <path to D3DX9_42.dll> <blob.bin>...
 *
 * Writes <blob>.asm next to each input. Needs no D3D device, so it runs under
 * plain Wine. Pass "d3dx9_43.dll" (Wine's built-in, vkd3d-shader based): the
 * game's own D3DX9_42.dll loads but returns S_OK with no buffer under Wine,
 * because it forwards to a d3dcompiler_42 that is not there.
 *
 * Built by the Makefile as build/fxdis.exe; run with
 *   toolbox run -c dev bash -lc 'WINEPREFIX=... wine build/fxdis.exe d3dx9_43.dll <blobs>'  
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct ID3DXBufferVtbl {
    HRESULT (__stdcall *QueryInterface)(void *, const void *, void **);
    ULONG   (__stdcall *AddRef)(void *);
    ULONG   (__stdcall *Release)(void *);
    void   *(__stdcall *GetBufferPointer)(void *);
    DWORD   (__stdcall *GetBufferSize)(void *);
} ID3DXBufferVtbl;
typedef struct { const ID3DXBufferVtbl *lpVtbl; } ID3DXBuffer;

typedef HRESULT (WINAPI *disasm_fn)(const DWORD *, BOOL, LPCSTR, ID3DXBuffer **);

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: fxdis <d3dx9_42.dll> <blob.bin>...\n"); return 2; }
    HMODULE dx = LoadLibraryA(argv[1]);
    if (!dx) { fprintf(stderr, "cannot load %s (err %lu)\n", argv[1], GetLastError()); return 1; }
    char loaded[MAX_PATH]; GetModuleFileNameA(dx, loaded, sizeof loaded);
    fprintf(stderr, "using %s\n", loaded);
    disasm_fn dis = (disasm_fn)GetProcAddress(dx, "D3DXDisassembleShader");
    if (!dis) { fprintf(stderr, "no D3DXDisassembleShader export\n"); return 1; }

    int bad = 0;
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "%s: cannot open\n", argv[i]); bad++; continue; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        DWORD *blob = (DWORD *)malloc(n + 4);
        fread(blob, 1, n, f); fclose(f);
        ID3DXBuffer *out = NULL;
        HRESULT hr = dis(blob, FALSE, NULL, &out);
        if (FAILED(hr) || !out) { fprintf(stderr, "%s: hr=0x%08lx out=%p\n", argv[i], hr, (void *)out); bad++; free(blob); continue; }
        char name[MAX_PATH];
        snprintf(name, sizeof name, "%s.asm", argv[i]);
        char *dot = strrchr(name, '.'); /* replace ".bin.asm" with ".asm" when possible */
        if (dot && dot - name > 4 && !strncmp(dot - 4, ".bin", 4)) memmove(dot - 4, dot, strlen(dot) + 1);
        FILE *o = fopen(name, "wb");
        if (o) {
            fwrite(out->lpVtbl->GetBufferPointer(out), 1, out->lpVtbl->GetBufferSize(out) - 1, o);
            fclose(o);
        }
        out->lpVtbl->Release(out);
        free(blob);
    }
    return bad ? 1 : 0;
}
