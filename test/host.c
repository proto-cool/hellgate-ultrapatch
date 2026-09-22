/* Trivial 32-bit host: proves the proxy forwards and the hash guard fires. */
#include <windows.h>
#include <stdio.h>
int main(void)
{
    WCHAR self[MAX_PATH];
    DWORD handle = 0, size;
    void *buf;
    UINT len = 0;
    VS_FIXEDFILEINFO *ffi = NULL;

    GetModuleFileNameW(NULL, self, MAX_PATH);
    size = GetFileVersionInfoSizeW(self, &handle);
    printf("GetFileVersionInfoSizeW -> %lu\n", size);
    if (!size) { printf("no version resource; forwarding still exercised\n"); }
    else {
        buf = malloc(size);
        if (GetFileVersionInfoW(self, 0, size, buf)) {
            printf("GetFileVersionInfoW ok\n");
            if (VerQueryValueW(buf, L"\\", (void **)&ffi, &len))
                printf("VerQueryValueW ok, len=%u sig=%08lx\n", len, ffi->dwSignature);
        }
    }
    printf("VerLanguageNameA test: ");
    { char nm[128]; VerLanguageNameA(0x0409, nm, sizeof nm); printf("%s\n", nm); }
    /*
     * The panel runs on a thread this process owns, so a 500ms host cannot
     * be used to test it — the socket dies with main(). Hold open when the
     * panel selftest is asked for so the endpoints can actually be probed.
     */
    if (GetEnvironmentVariableA("HG_PANEL_SELFTEST", NULL, 0) > 0) {
        printf("holding open for panel selftest; Ctrl-C or kill to stop\n");
        fflush(stdout);
        for (;;) Sleep(1000);
    }
    Sleep(500);
    return 0;
}
