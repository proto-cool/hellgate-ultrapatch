#include <windows.h>
#include "proxy.h"

void hook_start(void);

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        if (!proxy_init())
            return FALSE;   /* without forwarding the host cannot run at all */
        /* Real work happens off the loader lock. */
        hook_start();
    }
    return TRUE;
}
