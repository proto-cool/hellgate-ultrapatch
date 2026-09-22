/*
 * Pre-flight for rebuilt effects: load a .fxo with the game's own D3DX
 * (the Proton prefix's native d3dx9_34.dll) on a real D3D9 device under Wine
 * and validate every technique, exactly as the engine will.
 *
 *   fxload.exe <d3dx9_34.dll> <file.fxo> [<file.fxo> ...]
 *
 * Prints the technique count and any technique that fails validation. A
 * failure here would be a black or missing material in the game.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3d9.h>
#include <d3dx9effect.h>
#include <string.h>

typedef HRESULT (WINAPI *create_fx_fn)(IDirect3DDevice9 *, const void *, UINT, const D3DXMACRO *,
                                       ID3DXInclude *, DWORD, ID3DXEffectPool *, ID3DXEffect **,
                                       ID3DXBuffer **);

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: fxload <d3dx9_34.dll> <file.fxo>...\n"); return 2; }
    HMODULE dx = LoadLibraryA(argv[1]);
    if (!dx) { fprintf(stderr, "cannot load %s (%lu)\n", argv[1], GetLastError()); return 1; }
    create_fx_fn create = (create_fx_fn)GetProcAddress(dx, "D3DXCreateEffect");
    if (!create) { fprintf(stderr, "no D3DXCreateEffect\n"); return 1; }

    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { fprintf(stderr, "Direct3DCreate9 failed\n"); return 1; }
    HWND wnd = CreateWindowExA(0, "STATIC", "fxload", WS_OVERLAPPED, 0, 0, 16, 16, NULL, NULL,
                               GetModuleHandleA(NULL), NULL);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev) {
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    }
    if (FAILED(hr) || !dev) { fprintf(stderr, "CreateDevice failed hr=0x%08lx\n", hr); return 1; }
    D3DCAPS9 caps;
    IDirect3DDevice9_GetDeviceCaps(dev, &caps);
    printf("device: vs %lx ps %lx\n", caps.VertexShaderVersion, caps.PixelShaderVersion);

    int bad = 0;
    /* -shader mode: create each blob as a raw shader and read its constant
     * table with this D3DX, to tell shader trouble from effect trouble. */
    if (argc > 3 && !strcmp(argv[2], "-shader")) {
        typedef HRESULT (WINAPI *getver_fn)(const DWORD *);
        typedef HRESULT (WINAPI *getct_fn)(const DWORD *, void **);
        getver_fn getver = (getver_fn)GetProcAddress(dx, "D3DXGetShaderVersion");
        getct_fn getct = (getct_fn)GetProcAddress(dx, "D3DXGetShaderConstantTable");
        for (int i = 3; i < argc; i++) {
            FILE *f = fopen(argv[i], "rb");
            if (!f) { printf("%s: cannot open\n", argv[i]); bad++; continue; }
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            DWORD *blob = malloc(n + 4); fread(blob, 1, n, f); fclose(f);
            DWORD ver = getver ? (DWORD)getver(blob) : 0;
            void *ct = NULL;
            HRESULT hct = getct ? getct(blob, &ct) : E_NOTIMPL;
            HRESULT hcr;
            if ((blob[0] >> 16) == 0xFFFE) { IDirect3DVertexShader9 *vs = NULL; hcr = IDirect3DDevice9_CreateVertexShader(dev, blob, &vs); if (vs) IDirect3DVertexShader9_Release(vs); }
            else { IDirect3DPixelShader9 *ps = NULL; hcr = IDirect3DDevice9_CreatePixelShader(dev, blob, &ps); if (ps) IDirect3DPixelShader9_Release(ps); }
            printf("%s: version=0x%08lx constant-table hr=0x%08lx create hr=0x%08lx\n", argv[i], ver, hct, hcr);
            if (ct) ((IUnknown *)ct)->lpVtbl->Release((IUnknown *)ct);
            if (FAILED(hct) || FAILED(hcr)) bad++;
            free(blob);
        }
        return bad ? 1 : 0;
    }
    /* -bind mode: does a technique's pass receive the effect's constants?
     *   fxload <dll> -bind <file.fxo> <technique> <pass>
     * Sets WorldViewProjection to a marker matrix, begins the pass, and looks
     * for the marker among the vertex shader constants. Techniques that share
     * shader objects may silently not get their constants uploaded. */
    if (argc >= 6 && !strcmp(argv[2], "-bind")) {
        FILE *f = fopen(argv[3], "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[3]); return 1; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        void *buf = malloc(n); fread(buf, 1, n, f); fclose(f);
        ID3DXEffect *fx = NULL;
        hr = create(dev, buf, (UINT)n, NULL, NULL, 0, NULL, &fx, NULL);
        if (FAILED(hr) || !fx) { printf("create failed\n"); return 1; }
        D3DXMATRIX m; memset(&m, 0, sizeof m);
        m._11 = 11.5f; m._22 = 22.5f; m._33 = 33.5f; m._44 = 44.5f; m._14 = 14.5f;
        fx->lpVtbl->SetMatrix(fx, "WorldViewProjection", &m);
        D3DXVECTOR4 eye = { 1.25f, 2.25f, 3.25f, 4.25f };
        fx->lpVtbl->SetVector(fx, "EyeInObject", &eye);
        D3DXHANDLE t = fx->lpVtbl->GetTechniqueByName(fx, argv[4]);
        if (!t) { printf("no technique %s\n", argv[4]); return 1; }
        UINT passes = 0;
        fx->lpVtbl->SetTechnique(fx, t);
        fx->lpVtbl->Begin(fx, &passes, 0);
        UINT p = (UINT)atoi(argv[5]);
        hr = fx->lpVtbl->BeginPass(fx, p);
        float c[256 * 4];
        IDirect3DDevice9_GetVertexShaderConstantF(dev, 0, c, 256);
        int wvp = -1, eyer = -1;
        for (int r = 0; r < 253; r++) {
            if (c[r * 4] == 11.5f && c[(r + 1) * 4 + 1] == 22.5f) wvp = r;
            if (c[r * 4] == 1.25f && c[r * 4 + 1] == 2.25f) eyer = r;
        }
        printf("%s pass %u of %u (BeginPass hr=0x%08lx): WorldViewProjection at c%d, EyeInObject at c%d\n",
               argv[4], p, passes, hr, wvp, eyer);
        fx->lpVtbl->EndPass(fx); fx->lpVtbl->End(fx);
        fx->lpVtbl->Release(fx);
        return 0;
    }
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "%s: cannot open\n", argv[i]); bad++; continue; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        void *buf = malloc(n); fread(buf, 1, n, f); fclose(f);
        ID3DXEffect *fx = NULL; ID3DXBuffer *err = NULL;
        DWORD t0 = GetTickCount();
        hr = create(dev, buf, (UINT)n, NULL, NULL, 0, NULL, &fx, &err);
        DWORD tcreate = GetTickCount() - t0;
        if (err) { fwrite(err->lpVtbl->GetBufferPointer(err), 1, err->lpVtbl->GetBufferSize(err), stderr); err->lpVtbl->Release(err); }
        if (FAILED(hr) || !fx) { printf("%s: D3DXCreateEffect FAILED hr=0x%08lx\n", argv[i], hr); bad++; free(buf); continue; }
        printf("  D3DXCreateEffect took %lu ms for %ld bytes\n", tcreate, n);
        D3DXEFFECT_DESC ed;
        fx->lpVtbl->GetDesc(fx, &ed);
        int failed = 0, passes = 0;
        for (UINT t = 0; t < ed.Techniques; t++) {
            D3DXHANDLE h = fx->lpVtbl->GetTechnique(fx, t);
            D3DXTECHNIQUE_DESC td;
            fx->lpVtbl->GetTechniqueDesc(fx, h, &td);
            passes += td.Passes;
            HRESULT v = fx->lpVtbl->ValidateTechnique(fx, h);
            if (FAILED(v)) {
                if (failed < 12) printf("  INVALID %s (%u passes) hr=0x%08lx\n", td.Name, td.Passes, v);
                failed++;
            }
        }
        printf("%s: %u params, %u techniques, %d passes, %d invalid\n", argv[i], ed.Parameters, ed.Techniques, passes, failed);
        if (failed) bad++;
        fx->lpVtbl->Release(fx);
        free(buf);
    }
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return bad ? 1 : 0;
}
