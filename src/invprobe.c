/*
 * Probe for the inventory sort (docs/spikes/inventory-sort.md, plan step 1).
 * Logs only; changes nothing. Removed once the sort is built.
 *
 * - FUN_0055e258, the client's message send (__thiscall: the message id in
 *   ECX, the message on the stack): each id the first time it is seen, and
 *   every message with an inventory-range id (0x28-0x36), with its first
 *   16 bytes. Settles whether a move goes out as 0x2d or 0x30.
 * - FUN_00473c45, UIInitLoad (__thiscall: the file name in ECX): the name.
 * (The load-complete callback's record was mapped here too; src/uiext.c
 * owns that hook now.)
 */
#include <windows.h>
#include "panel.h"
#include "../ref/minhook/include/MinHook.h"

#define VA_SEND     0x0055e258u
#define VA_UILOAD   0x00473c45u

typedef int  (__fastcall *send_fn)(int id, int edx, const unsigned char *msg);
typedef void (__fastcall *uiload_fn)(const char *name, int edx, int a, int b);

static send_fn   o_send;
static uiload_fn o_uiload;
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
}
