/*
 * /fart
 *
 * The retail build has no console to add a command to -- consolecmd.cpp is
 * down to a single surviving assert -- and chat submission is not cheaply
 * hookable, so there is no slash-command surface to extend. Rather than
 * spend a session recovering one for this, the fart is synthesised here and
 * played through winmm, which needs nothing from the game at all.
 *
 * That also makes it the one feature in this DLL that cannot fail for the
 * usual reason: it depends on no recovered address, so it either makes a
 * noise or winmm is missing.
 *
 * The waveform is a clipped sawtooth whose pitch falls over the duration,
 * wobbled by a low-frequency oscillator and roughened with a little noise.
 * Pitch, length, wobble rate and sputter are all randomised per call, so it
 * is a different fart every time rather than the same sample on repeat.
 */
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include "panel.h"
#endif
#include <math.h>
#include <string.h>

/*
 * The synthesis below is deliberately free of Windows, so test/ui.c can
 * build it natively and `uitest --fart out.wav` can render one to a file.
 * Tuning a sound by launching the game each time is not tuning.
 */

#define FART_SR      22050u              /* sample rate, Hz              */
#define FART_MAXMS   1100u               /* longest fart we will produce */
#define FART_MAXSMP  (FART_SR * FART_MAXMS / 1000u)

static unsigned char g_wav[44 + FART_MAXSMP * 2];
static unsigned int  g_seed = 0x5eedface;

static unsigned int rnd(void)
{
    /* xorshift32: no allocation, no CRT state, deterministic to debug. */
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static double rnd01(void) { return (double)(rnd() >> 8) / 16777216.0; }

static void put32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void put16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

/* A minimal 16-bit mono PCM WAV header over `n` samples. */
static void wav_header(unsigned int n)
{
    unsigned int data = n * 2;
    memcpy(g_wav, "RIFF", 4);
    put32(g_wav + 4, 36 + data);
    memcpy(g_wav + 8, "WAVEfmt ", 8);
    put32(g_wav + 16, 16);              /* fmt chunk size   */
    put16(g_wav + 20, 1);               /* PCM              */
    put16(g_wav + 22, 1);               /* mono             */
    put32(g_wav + 24, FART_SR);
    put32(g_wav + 28, FART_SR * 2);     /* byte rate        */
    put16(g_wav + 32, 2);               /* block align      */
    put16(g_wav + 34, 16);              /* bits per sample  */
    memcpy(g_wav + 36, "data", 4);
    put32(g_wav + 40, data);
}

static unsigned int synth(void)
{
    /* Ranges chosen by ear. A fart is mostly a falling buzz with a wobble
     * on it; the sputter is what stops it sounding like a bass note. */
    double ms      = 260.0 + rnd01() * 620.0;
    double f_start = 95.0  + rnd01() * 75.0;
    double f_end   = 42.0  + rnd01() * 28.0;
    double lfo_hz  = 11.0  + rnd01() * 22.0;
    double lfo_amt = 0.16  + rnd01() * 0.34;
    double sputter = 0.10  + rnd01() * 0.35;
    double drive   = 2.4   + rnd01() * 3.2;

    unsigned int n = (unsigned int)(ms * FART_SR / 1000.0);
    double phase = 0.0, lfo = 0.0, wobble = 0.0;
    unsigned int i;

    if (n > FART_MAXSMP) n = FART_MAXSMP;
    wav_header(n);

    for (i = 0; i < n; i++) {
        double t   = (double)i / (double)n;
        double f   = f_start + (f_end - f_start) * t;
        double env, saw, s;

        /* Pitch wobble, plus a slow random walk so it never sits still. */
        lfo    += 2.0 * 3.14159265358979 * lfo_hz / (double)FART_SR;
        wobble += (rnd01() - 0.5) * sputter * 0.5;
        if (wobble >  1.0) wobble =  1.0;
        if (wobble < -1.0) wobble = -1.0;
        f *= 1.0 + lfo_amt * sin(lfo) + 0.12 * wobble;

        phase += 2.0 * 3.14159265358979 * f / (double)FART_SR;
        if (phase > 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;

        /* Sawtooth, waveshaped to a buzz, with noise for the wetness. */
        saw = phase / 3.14159265358979 - 1.0;
        s   = tanh(saw * drive);
        s  += (rnd01() - 0.5) * 0.22 * sputter;

        /* Fast attack, long-ish decay, with a tail that trails off. */
        env = (t < 0.04) ? (t / 0.04) : pow(1.0 - (t - 0.04) / 0.96, 1.7);
        s  *= env * 0.72;

        if (s >  1.0) s =  1.0;
        if (s < -1.0) s = -1.0;
        put16(g_wav + 44 + i * 2, (unsigned int)(short)(s * 32000.0));
    }
    return n;
}

/* Renders one fart. Returns its size in bytes; *out points at the WAV. */
unsigned int hg_fart_render(const unsigned char **out, unsigned int seed)
{
    unsigned int n;
    if (seed) g_seed ^= seed * 2654435761u;
    n = synth();
    if (out) *out = g_wav;
    return 44u + n * 2u;
}

#ifdef _WIN32
static volatile LONG g_farting;          /* one in flight at a time */

/*
 * Real farts.
 *
 * Synthesis gets you a buzz, not a fart -- the real thing is wet and
 * chaotic in ways a sawtooth is not, and no amount of tuning fixes that.
 * So: drop .wav files in a `farts` folder next to version.dll and one is
 * chosen at random per press. Nothing is shipped with the DLL, which keeps
 * the repo free of audio nobody has the rights to redistribute.
 *
 * The synthesised fart stays as the fallback, so the feature still makes a
 * noise on a fresh install.
 */
#define FART_MAXFILES 64

static WCHAR g_files[FART_MAXFILES][MAX_PATH];
static int   g_nfiles = -1;              /* -1 = not scanned yet */

static void scan_farts(void)
{
    WCHAR dir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    g_nfiles = 0;
    hg_dll_dir(dir, MAX_PATH);
    if (!dir[0]) return;

    lstrcpynW(pat, dir, MAX_PATH);
    lstrcatW(pat, L"\\farts\\*.wav");

    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        hg_log("fart: no samples — put .wav files in <game>/bin/farts/ for "
               "real ones; using the synthesised fallback");
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_nfiles >= FART_MAXFILES) break;
        lstrcpynW(g_files[g_nfiles], dir, MAX_PATH);
        lstrcatW(g_files[g_nfiles], L"\\farts\\");
        lstrcatW(g_files[g_nfiles], fd.cFileName);
        g_nfiles++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    hg_log("fart: %d sample%s loaded from <game>/bin/farts/", g_nfiles,
           g_nfiles == 1 ? "" : "s");
}

int hg_fart_samples(void)
{
    return g_nfiles < 0 ? 0 : g_nfiles;
}

/*
 * Safe from any thread. PlaySound with SND_ASYNC returns immediately, so
 * this never stalls the render thread, and the guard means a second press
 * while one is rendering is dropped rather than interleaving buffers.
 */
void hg_fart(void)
{
    unsigned int bytes;

    if (InterlockedExchange(&g_farting, 1)) return;

    if (g_nfiles < 0) scan_farts();
    if (g_nfiles > 0) {
        /* A real one. SND_FILENAME reads it straight off disk; these are a
         * few tens of KB and the OS caches them after the first play. */
        unsigned int pick;
        g_seed ^= GetTickCount() * 2654435761u;
        pick = rnd() % (unsigned int)g_nfiles;
        if (PlaySoundW(g_files[pick], NULL,
                       SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
            InterlockedExchange(&g_farting, 0);
            return;
        }
        hg_log("fart: sample %u would not play; falling back to synthesis",
               pick);
    }

    bytes = hg_fart_render(NULL, GetTickCount());
    if (!PlaySoundA((const char *)g_wav, NULL,
                    SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
        hg_log("fart: PlaySound refused it (no winmm audio device?)");
    else
        hg_log("fart: %u ms", (bytes - 44u) / 2u * 1000u / FART_SR);
    InterlockedExchange(&g_farting, 0);
}
#endif
