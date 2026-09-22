/*
 * HLSL compiler front end for the shader work, run under Wine.
 *
 *   fxcc.exe <source.hlsl> <entry> <profile> <out.bin> [NAME=VALUE ...]
 *
 * Uses D3DCompile from Wine's built-in d3dcompiler_47 (vkd3d-shader's HLSL
 * compiler), which emits SM1-3 bytecode for vs_3_0 / ps_3_0 profiles. The
 * result is a plain shader blob that tools/fx/hgfx.py splices into a cloned
 * .fxo; no D3DX effect compiler is involved, so nothing here depends on the
 * missing Microsoft toolchain. Extra arguments become preprocessor defines,
 * which is how the technique variants are produced from one source.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *Name, *Definition; } MACRO;
typedef struct ID3DBlobVtbl {
    HRESULT (__stdcall *QueryInterface)(void *, const void *, void **);
    ULONG   (__stdcall *AddRef)(void *);
    ULONG   (__stdcall *Release)(void *);
    void   *(__stdcall *GetBufferPointer)(void *);
    SIZE_T  (__stdcall *GetBufferSize)(void *);
} ID3DBlobVtbl;
typedef struct { const ID3DBlobVtbl *lpVtbl; } ID3DBlob;
typedef HRESULT (WINAPI *compile_fn)(const void *, SIZE_T, const char *, const MACRO *, void *,
                                     const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

#define D3DCOMPILE_OPTIMIZATION_LEVEL3 (1 << 15)
#define D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR (1 << 4)

/* d3dx9_34 (April 2007 D3DX, the version the game imports) still carries
 * Microsoft's HLSL compiler inside the DLL: D3DXCompileShader. Set FXCC_DLL
 * to a native d3dx9_34.dll (the Proton prefix has one) to compile with it
 * instead of vkd3d. The game's D3DX accepts its own compiler's bytecode in
 * effects and rejects vkd3d's, so this is the path that ships. */
typedef HRESULT (WINAPI *d3dx_compile_fn)(const char *, UINT, const MACRO *, void *, const char *,
                                          const char *, DWORD, ID3DBlob **, ID3DBlob **, void **);

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: fxcc <src.hlsl> <entry> <profile> <out.bin> [NAME=VALUE...]\n"); return 2; }
    const char *dxdll = getenv("FXCC_DLL");
    HMODULE m = LoadLibraryA(dxdll ? dxdll : "d3dcompiler_47.dll");
    if (!m) { fprintf(stderr, "cannot load %s\n", dxdll ? dxdll : "d3dcompiler_47.dll"); return 1; }
    compile_fn compile = (compile_fn)GetProcAddress(m, "D3DCompile");
    d3dx_compile_fn dxcompile = (d3dx_compile_fn)GetProcAddress(m, "D3DXCompileShader");
    if (!compile && !dxcompile) { fprintf(stderr, "no D3DCompile / D3DXCompileShader\n"); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "%s: cannot open\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc(n + 1); fread(src, 1, n, f); fclose(f); src[n] = 0;

    MACRO *defs = calloc(argc, sizeof *defs);
    int nd = 0;
    for (int i = 5; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) { *eq = 0; defs[nd].Name = argv[i]; defs[nd].Definition = eq + 1; }
        else { defs[nd].Name = argv[i]; defs[nd].Definition = "1"; }
        nd++;
    }
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;
    const char *fl = getenv("FXCC_FLAGS");          /* hex D3DXSHADER_* flags, default O3 */
    UINT flags = fl ? (UINT)strtoul(fl, NULL, 16) : D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (dxcompile)
        hr = dxcompile(src, (UINT)n, defs, NULL, argv[2], argv[3], flags, &code, &err, NULL);
    else
        hr = compile(src, n, argv[1], defs, NULL, argv[2], argv[3], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) {
        fwrite(err->lpVtbl->GetBufferPointer(err), 1, err->lpVtbl->GetBufferSize(err), stderr);
        err->lpVtbl->Release(err);
    }
    if (FAILED(hr) || !code) { fprintf(stderr, "%s: compile failed hr=0x%08lx\n", argv[1], hr); return 1; }
    FILE *o = fopen(argv[4], "wb");
    if (!o) { fprintf(stderr, "%s: cannot write\n", argv[4]); return 1; }
    fwrite(code->lpVtbl->GetBufferPointer(code), 1, code->lpVtbl->GetBufferSize(code), o);
    fclose(o);
    printf("%s: %s %s -> %s (%lu bytes, %s)\n", argv[1], argv[3], argv[2], argv[4],
           (unsigned long)code->lpVtbl->GetBufferSize(code), dxcompile ? "D3DXCompileShader" : "D3DCompile");
    code->lpVtbl->Release(code);
    return 0;
}
