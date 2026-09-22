/*
 * Compile an .fx source into an fx_2_0 binary with Microsoft's effect
 * compiler from a native d3dx9_34.dll (the version the game uses), under Wine.
 *
 *   fxcomp.exe <d3dx9_34.dll> <source.fx> <out.fxo> [NAME=VALUE ...]
 *   fxcomp.exe <d3dx9_34.dll> <source.fx> -batch <list>
 *
 * Batch mode compiles one variant per line of <list> ("<out.fxo> NAME=VALUE
 * ...") in a single process; a Wine start per variant is most of the cost
 * when a material family has hundreds of them.
 *
 * Why not D3DXCompileShader: shaders compiled standalone lack the constant
 * default blocks the effect compiler writes, and the game's D3DX rejects an
 * effect whose injected shader spans many registers (Bones[180]) without
 * them. Blobs lifted from an effect compiled here are exactly what the
 * loader expects. tools/fx/mkfx.py extracts them with hgfx.py.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d9.h>
#include <d3dx9effect.h>

typedef HRESULT (WINAPI *create_compiler_fn)(LPCSTR, UINT, const D3DXMACRO *, ID3DXInclude *, DWORD,
                                             ID3DXEffectCompiler **, ID3DXBuffer **);

static create_compiler_fn create;
static char *src;
static long srclen;

/* Compile src with the NAME=VALUE macros in defv[0..defc) into out. The
 * strings in defv are modified (split at '='). */
static int compile(const char *srcname, const char *out, int defc, char **defv)
{
    D3DXMACRO *defs = calloc(defc + 1, sizeof *defs);
    int nd = 0;
    for (int i = 0; i < defc; i++) {
        char *eq = strchr(defv[i], '=');
        if (eq) { *eq = 0; defs[nd].Name = defv[i]; defs[nd].Definition = eq + 1; }
        else { defs[nd].Name = defv[i]; defs[nd].Definition = "1"; }
        nd++;
    }
    ID3DXEffectCompiler *comp = NULL; ID3DXBuffer *err = NULL, *blob = NULL;
    HRESULT hr = create(src, (UINT)srclen, defs, NULL, 0, &comp, &err);
    free(defs);
    if (err) { fwrite(err->lpVtbl->GetBufferPointer(err), 1, err->lpVtbl->GetBufferSize(err), stderr); err->lpVtbl->Release(err); err = NULL; }
    if (FAILED(hr) || !comp) { fprintf(stderr, "%s: D3DXCreateEffectCompiler failed hr=0x%08lx (%s)\n", srcname, hr, out); return 1; }
    hr = comp->lpVtbl->CompileEffect(comp, 0, &blob, &err);
    if (err) { fwrite(err->lpVtbl->GetBufferPointer(err), 1, err->lpVtbl->GetBufferSize(err), stderr); err->lpVtbl->Release(err); }
    if (FAILED(hr) || !blob) { fprintf(stderr, "%s: CompileEffect failed hr=0x%08lx (%s)\n", srcname, hr, out); comp->lpVtbl->Release(comp); return 1; }
    FILE *o = fopen(out, "wb");
    if (!o) { fprintf(stderr, "%s: cannot write\n", out); return 1; }
    fwrite(blob->lpVtbl->GetBufferPointer(blob), 1, blob->lpVtbl->GetBufferSize(blob), o);
    fclose(o);
    printf("%s -> %s (%lu bytes)\n", srcname, out, (unsigned long)blob->lpVtbl->GetBufferSize(blob));
    blob->lpVtbl->Release(blob);
    comp->lpVtbl->Release(comp);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: fxcomp <d3dx9_34.dll> <src.fx> <out.fxo> [NAME=VALUE...] | -batch <list>\n"); return 2; }
    HMODULE dx = LoadLibraryA(argv[1]);
    if (!dx) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    create = (create_compiler_fn)GetProcAddress(dx, "D3DXCreateEffectCompiler");
    if (!create) { fprintf(stderr, "no D3DXCreateEffectCompiler\n"); return 1; }

    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "%s: cannot open\n", argv[2]); return 1; }
    fseek(f, 0, SEEK_END); srclen = ftell(f); fseek(f, 0, SEEK_SET);
    src = malloc(srclen + 1); fread(src, 1, srclen, f); fclose(f); src[srclen] = 0;

    if (strcmp(argv[3], "-batch"))
        return compile(argv[2], argv[3], argc - 4, argv + 4);
    if (argc < 5) { fprintf(stderr, "-batch needs a list file\n"); return 2; }
    FILE *l = fopen(argv[4], "r");
    if (!l) { fprintf(stderr, "%s: cannot open\n", argv[4]); return 1; }
    char line[4096];
    int bad = 0;
    while (fgets(line, sizeof line, l)) {
        char *tok[64]; int nt = 0;
        for (char *t = strtok(line, " \t\r\n"); t && nt < 64; t = strtok(NULL, " \t\r\n")) tok[nt++] = t;
        if (nt == 0 || tok[0][0] == '#') continue;
        bad += compile(argv[2], tok[0], nt - 1, tok + 1);
    }
    fclose(l);
    return bad ? 1 : 0;
}
