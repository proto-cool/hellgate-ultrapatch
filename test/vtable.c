/*
 * Confirms the IDirect3DDevice9 vtable slot indices the overlay hooks.
 *
 * Counting entries in a header is exactly how overlays get this wrong, and a
 * wrong index means hooking an unrelated method and corrupting the device. So
 * this proves the indices by behaviour instead: call the slots blind through
 * the vtable and check they do what BeginScene/EndScene are supposed to do.
 *
 *   41 BeginScene  -> D3D_OK once, D3DERR_INVALIDCALL if called again
 *   42 EndScene    -> D3D_OK after a BeginScene, D3DERR_INVALIDCALL before
 *
 * That asymmetry is what identifies them: no other adjacent pair behaves this
 * way. Run it under the same wine prefix the game uses.
 */
#include <d3d9.h>
#include <windows.h>
#include <stdio.h>

#define VT_RESET     16
#define VT_BEGIN     41
#define VT_ENDSCENE  42

typedef HRESULT (WINAPI *nullary_fn)(IDirect3DDevice9 *);

static const char *hr(HRESULT h)
{
    if (h == D3D_OK) return "D3D_OK";
    if (h == D3DERR_INVALIDCALL) return "D3DERR_INVALIDCALL";
    return "other";
}

int main(void)
{
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    HWND wnd;
    void **vt;
    nullary_fn begin, end;
    HRESULT a, b, c, d;
    int ok;

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("FAIL Direct3DCreate9\n"); return 1; }

    wnd = CreateWindowExA(0, "STATIC", "vt", WS_OVERLAPPED, 0, 0, 8, 8,
                          NULL, NULL, GetModuleHandleA(NULL), NULL);
    memset(&pp, 0, sizeof pp);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = wnd;

    if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                       wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                                       D3DCREATE_NOWINDOWCHANGES, &pp, &dev))) {
        printf("FAIL CreateDevice (no usable D3D9 in this prefix)\n");
        return 1;
    }

    vt = *(void ***)dev;
    printf("device   %p\nvtable   %p\n", (void *)dev, (void *)vt);
    printf("slot %-2d  %p  (Reset)\n",     VT_RESET,    vt[VT_RESET]);
    printf("slot %-2d  %p  (BeginScene?)\n", VT_BEGIN,  vt[VT_BEGIN]);
    printf("slot %-2d  %p  (EndScene?)\n", VT_ENDSCENE, vt[VT_ENDSCENE]);

    /* Compare against the names the header resolves, as a cross-check. */
    printf("header   BeginScene=%p EndScene=%p Reset=%p\n",
           (void *)dev->lpVtbl->BeginScene,
           (void *)dev->lpVtbl->EndScene,
           (void *)dev->lpVtbl->Reset);

    begin = (nullary_fn)vt[VT_BEGIN];
    end   = (nullary_fn)vt[VT_ENDSCENE];

    a = end(dev);        /* EndScene before any BeginScene -> INVALIDCALL */
    b = begin(dev);      /* first BeginScene               -> OK          */
    c = begin(dev);      /* nested BeginScene              -> INVALIDCALL */
    d = end(dev);        /* matching EndScene              -> OK          */

    printf("\nend before begin : %s\n", hr(a));
    printf("begin            : %s\n", hr(b));
    printf("begin again      : %s\n", hr(c));
    printf("end              : %s\n", hr(d));

    ok = (b == D3D_OK) && (d == D3D_OK) &&
         (a == D3DERR_INVALIDCALL) && (c == D3DERR_INVALIDCALL) &&
         (vt[VT_BEGIN] == (void *)dev->lpVtbl->BeginScene) &&
         (vt[VT_ENDSCENE] == (void *)dev->lpVtbl->EndScene) &&
         (vt[VT_RESET] == (void *)dev->lpVtbl->Reset);

    printf("\n%s: slots 16/41/42 are Reset/BeginScene/EndScene\n",
           ok ? "PASS" : "FAIL");

    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(wnd);
    return ok ? 0 : 1;
}
