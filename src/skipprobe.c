/*
 * Why a mesh gets depth but no colour (the invisible barbed wire on the
 * character select, 2026-09-24): the engine's silent skips, logged.
 *
 * Umbra is ruled out (all visible changed nothing). The engine skips a
 * colour draw without a word in two places:
 *   dxC_ModelDrawAddMesh (0x7ccea4, cdecl, 6 args: mesh list, context,
 *     model draw, mesh index, material override, pass flags): a dozen
 *     `return 1` exits (technique lookup, EFFECTS row, ...);
 *   FUN_007c7d29 (0x7c7d29, cdecl, 3 args, dxC_render.cpp ~1156): returns 1
 *     when a vertex stream the colour technique declares has no buffer
 *     (the depth pass needs only position and UV).
 * Both are hooked and counted; the first skips are logged with their
 * arguments, and every 10 s a summary by pass flags.
 */
#include <windows.h>
#include "panel.h"
#include "../ref/minhook/include/MinHook.h"

#define RVA_ADD_MESH   0x003CCEA4u
#define RVA_STREAMS    0x003C7D29u
#define RVA_MESH_CHECK 0x0038FD95u   /* FUN_0078fd95: mesh in ESI, model draw state in EDI, returns draw or not */

typedef int (__cdecl *addmesh_fn)(void *list, int ctx, void *md, int mesh, int ovr, unsigned char flags);
typedef int (__cdecl *streams_fn)(void *model, void *mesh, int *out);
static addmesh_fn o_addmesh;
static streams_fn o_streams;

static volatile LONG g_calls[256][3];   /* per pass flags: ok, skipped (1), error (< 0) */
static volatile LONG g_st_ok, g_st_skip, g_st_err, g_logged;

static void summary(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    int f;
    if (now - last < 10000) return;
    last = now;
    for (f = 0; f < 256; f++)
        if (g_calls[f][1] || g_calls[f][2])
            hg_log("skipprobe: add mesh, pass flags %02x: %ld added, %ld skipped, %ld errors", f,
                   g_calls[f][0], g_calls[f][1], g_calls[f][2]);
    hg_log("skipprobe: stream check: %ld ok, %ld skipped (a stream without a buffer), %ld errors",
           g_st_ok, g_st_skip, g_st_err);
}

static int __cdecl d_addmesh(void *list, int ctx, void *md, int mesh, int ovr, unsigned char flags)
{
    int r = o_addmesh(list, ctx, md, mesh, ovr, flags);
    InterlockedIncrement(&g_calls[flags][r == 0 ? 0 : r == 1 ? 1 : 2]);
    if (r != 0 && InterlockedIncrement(&g_logged) <= 60)
        hg_log("skipprobe: add mesh SKIPPED (%d): model draw %p, mesh %d, override %d, pass flags %02x",
               r, md, mesh, ovr, flags);
    summary();
    return r;
}

static int __cdecl d_streams(void *model, void *mesh, int *out)
{
    int r = o_streams(model, mesh, out);
    if (r == 0) InterlockedIncrement(&g_st_ok);
    else if (r == 1) {
        InterlockedIncrement(&g_st_skip);
        if (InterlockedIncrement(&g_logged) <= 60)
            hg_log("skipprobe: draw SKIPPED, a vertex stream has no buffer: model %p, mesh %p (vb index %d)",
                   model, mesh, mesh && !IsBadReadPtr((char *)mesh + 0x38, 4) ? *(int *)((char *)mesh + 0x38) : -1);
    } else InterlockedIncrement(&g_st_err);
    return r;
}

/*
 * FUN_0078fd95, the mesh-flags check dxC_ModelDrawAddMesh runs before the
 * pass split: the mesh in ESI (flags word first; +0x10 compared with the
 * model's override; +0x44 must be set), EDI the draw state. A stub calls it
 * as it was called and hands the result to fd95_log with the registers.
 */
void *o_fd95 __attribute__((used));
void __cdecl fd95_log(unsigned int *mesh, unsigned int *st, int drawn) __attribute__((used));
void __cdecl fd95_log(unsigned int *mesh, unsigned int *st, int drawn)
{
    static unsigned int *seen[48];
    static LONG nseen;
    LONG i, n;
    if ((drawn & 0xff) || !mesh || IsBadReadPtr(mesh, 0x48)) return;
    n = nseen;
    for (i = 0; i < n && i < 48; i++) if (seen[i] == mesh) return;
    if (n >= 48) return;
    seen[n] = mesh; nseen = n + 1;
    hg_log("skipprobe: mesh %p NOT drawn by the flags check: flags %08x, +10 %08x, +44 %08x; state flags %08x",
           (void *)mesh, mesh[0], mesh[4], mesh[0x11], st && !IsBadReadPtr(st, 8) ? st[1] : 0xffffffffu);
}
void d_fd95(void);
__asm__(
    ".text\n\t"
    ".globl _d_fd95\n"
    "_d_fd95:\n\t"
    "call *_o_fd95\n\t"              /* as the caller called it: ESI, EDI untouched */
    "pushal\n\t"
    "pushl %eax\n\t"
    "pushl %edi\n\t"
    "pushl %esi\n\t"
    "call _fd95_log\n\t"
    "addl $12, %esp\n\t"
    "popal\n\t"
    "ret\n"
);

static int hook(unsigned int image, unsigned int rva, void *detour, void **orig, const char *name)
{
    void *t = (void *)(image + rva);
    int ok = MH_CreateHook(t, detour, orig) == MH_OK && MH_EnableHook(t) == MH_OK;
    if (!ok) hg_log("skipprobe: could not hook %s", name);
    return ok;
}

void skipprobe_install(unsigned int image)
{
    int n = hook(image, RVA_ADD_MESH, (void *)d_addmesh, (void **)&o_addmesh, "dxC_ModelDrawAddMesh") +
            hook(image, RVA_STREAMS, (void *)d_streams, (void **)&o_streams, "the stream check") +
            hook(image, RVA_MESH_CHECK, (void *)d_fd95, (void **)&o_fd95, "the mesh flags check");
    hg_log("skipprobe: %d of 3 hooks in (silent draw skips logged)", n);
}
