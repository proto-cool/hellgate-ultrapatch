/*
 * Probe for the inventory sort (docs/spikes/inventory-sort.md, plan step 1).
 * Logs only; changes nothing. Removed once the sort is built.
 *
 * - FUN_0055e258, the client's message send (__thiscall: the message id in
 *   ECX, the message on the stack): each id the first time it is seen, and
 *   every message with an inventory-range id (0x28-0x36), with its first
 *   16 bytes. Settles whether a move goes out as 0x2d or 0x30.
 * - FUN_00473c45, UIInitLoad (__thiscall: the file name in ECX): the name.
 * - FUN_0047381d, its load-complete callback (cdecl, the request record):
 *   the record's first 16 dwords, and every dword in it or in the context
 *   at +0x28 that points at text, so the XML buffer and its size can be
 *   found for the in-memory override.
 */
#include <windows.h>
#include "panel.h"
#include "../ref/minhook/include/MinHook.h"

#define VA_SEND     0x0055e258u
#define VA_UILOAD   0x00473c45u
#define VA_UIDONE   0x0047381du

typedef int  (__fastcall *send_fn)(int id, int edx, const unsigned char *msg);
typedef void (__fastcall *uiload_fn)(const char *name, int edx, int a, int b);
typedef void (__cdecl *uidone_fn)(unsigned char *req);

static send_fn   o_send;
static uiload_fn o_uiload;
static uidone_fn o_uidone;
static unsigned char g_seen[0x200];
static volatile LONG g_inv_lines;

static void hex16(char *out, const unsigned char *p)
{
    static const char h[] = "0123456789abcdef";
    int i;
    if (!p || IsBadReadPtr(p, 16)) { lstrcpyA(out, "(unreadable)"); return; }
    for (i = 0; i < 16; i++) {
        out[i * 3] = h[p[i] >> 4];
        out[i * 3 + 1] = h[p[i] & 15];
        out[i * 3 + 2] = i == 15 ? 0 : ' ';
    }
}

static int __fastcall d_send(int id, int edx, const unsigned char *msg)
{
    int inv = id >= 0x28 && id <= 0x36;
    int first = id >= 0 && id < (int)sizeof g_seen && !g_seen[id];
    if (first) g_seen[id] = 1;
    if (first || (inv && InterlockedIncrement(&g_inv_lines) <= 400)) {
        char hx[48];
        hex16(hx, msg);
        hg_log("invprobe: send id 0x%x%s tid %lu | %s", id, first ? " (first)" : "",
               GetCurrentThreadId(), hx);
    }
    return o_send(id, edx, msg);
}

static void __fastcall d_uiload(const char *name, int edx, int a, int b)
{
    hg_log("invprobe: UIInitLoad \"%s\" (%d, %d) tid %lu",
           name && !IsBadStringPtrA(name, 260) ? name : "?", a, b, GetCurrentThreadId());
    o_uiload(name, edx, a, b);
}

/* dwords in [base, base+n) that point at text: offset, value, the first 40 chars */
static void text_ptrs(const char *what, const unsigned char *base, int n)
{
    int off;
    if (!base || IsBadReadPtr(base, n)) return;
    for (off = 0; off < n; off += 4) {
        const char *p = *(const char *const *)(base + off);
        char t[41];
        int k;
        if ((UINT_PTR)p < 0x10000 || IsBadReadPtr(p, 40)) continue;
        for (k = 0; k < 40 && p[k] >= 9 && (unsigned char)p[k] < 127; k++) t[k] = p[k] == '\n' || p[k] == '\r' ? ' ' : p[k];
        if (k < 8) continue;
        t[k] = 0;
        hg_log("invprobe:   %s+0x%x -> %p \"%s\"", what, off, p, t);
    }
}

static void __cdecl d_uidone(unsigned char *req)
{
    hg_log("invprobe: UI load done, req %p tid %lu", req, GetCurrentThreadId());
    if (req && !IsBadReadPtr(req, 0x40)) {
        const DWORD *d = (const DWORD *)req;
        hg_log("invprobe:   req %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
               d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        hg_log("invprobe:   req %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
               d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
        text_ptrs("req", req, 0x60);
        text_ptrs("ctx", *(const unsigned char *const *)(req + 0x28), 0x180);
    }
    o_uidone(req);
}

static void hook(unsigned int va, void *detour, void **orig, const char *name)
{
    const unsigned char *p = (const unsigned char *)(UINT_PTR)va;
    if (MH_CreateHook((void *)(UINT_PTR)va, detour, orig) == MH_OK && MH_EnableHook((void *)(UINT_PTR)va) == MH_OK)
        hg_log("invprobe: hooked %s at %08x (%02x %02x %02x %02x %02x)", name, va, p[0], p[1], p[2], p[3], p[4]);
    else
        hg_log("invprobe: FAILED to hook %s at %08x", name, va);
}

void invprobe_install(void)
{
    hook(VA_SEND, (void *)d_send, (void **)&o_send, "message send");
    hook(VA_UILOAD, (void *)d_uiload, (void **)&o_uiload, "UIInitLoad");
    hook(VA_UIDONE, (void *)d_uidone, (void **)&o_uidone, "UI load callback");
}
