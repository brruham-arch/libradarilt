// libradarilt v1.8
// CWidgetRadar::Update confirmed = jalur render.
// bounds input = sentinel (1000000,-1000000,-1000000,1000000) = konvensi terbalik
// inner[0x88] output bounds juga sentinel.
//
// Dari disassembly CWidgetRadar::Update:
//   Input:  r4[32]=x1, r4[36]=y1, r4[40]=x2, r4[44]=y2
//   Output ditulis ke r0[12], r0[16], r0[20], r0[24]
//   r0 = r4[0x88]
//
// Konvensi: x1=1000000 x2=-1000000 → x1 > x2, y1 < y2 (flipped)
// Center: cx = (x1+x2)/2 = 0, cy = (y1+y2)/2 = 0 (world coords, bukan screen)
// halfW  = abs(x2-x1)/2 * scale_factor
//
// Output ke r0[12..24] = screen rect (x_left, x_right, y_top, y_bot) atau serupa
// Kita patch OUTPUT setelah orig dipanggil, bukan input.
//
// Dari log: inner output = (1000000,-1000000,-1000000,1000000) juga sentinel
// Berarti output belum diisi saat kita baca, atau offset salah.
//
// Re-read disassembly output stores:
//   vstr s0, [r0, #12]   → r0[12] = left_x  (s0 = cx - halfW)  
//   vstr s8, [r0, #16]   → r0[16] = right_x? (s8 = cx + halfW?)
//   wait - dari kode:
//   s6  = cx + halfW  → [r0+20]
//   s8  = cy + halfH? → [r0+16]  -- ini Y_BOT
//   s0  = cx - halfW  → [r0+12]  -- ini X_LEFT
//   s2  = cy - halfH  → [r0+24]  -- ini Y_TOP
//
// Jadi layout r0: [12]=x_left, [16]=y_bot, [20]=x_right, [24]=y_top
// Tilt = modif y_top dan y_bot relatif terhadap center Y

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <math.h>
#include <android/log.h>
#include "mod/amlmod.h"

#define LOG_TAG "libradarilt"
#define LOGFILE "/storage/emulated/0/radarilt_log.txt"

static void _log(const char* msg) {
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "%s", msg);
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fputs(msg, f); fputc('\n', f); fclose(f); }
}
static void _logf(const char* fmt, ...) {
    char buf[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    _log(buf);
}

#define OFF_WidgetUpdate  0x002BF798u

// Tilt: kompres tepi atas radar
// 0 = flat, 0.3 = 30% tinggi dikurangi dari atas
#define TILT_FACTOR  0.30f

static int g_logCount = 0;

typedef void (*tWidgetUpdate)(void*);
static tWidgetUpdate orig_WidgetUpdate = nullptr;

static void hk_WidgetUpdate(void* self) {
    // Panggil original dulu - biarkan dia hitung output
    orig_WidgetUpdate(self);

    // Baca pointer inner dari self[0x88]
    uintptr_t inner = *(uintptr_t*)((uintptr_t)self + 0x88);
    if (!inner) return;

    // Output layout di inner (dari disassembly vstr):
    //   [12] = x_left  (s0)
    //   [16] = y_bot   (s8) -- perhatikan urutan dari vstr
    //   [20] = x_right (s6)
    //   [24] = y_top   (s2)
    float* xl   = (float*)(inner + 12);
    float* ybot = (float*)(inner + 16);
    float* xr   = (float*)(inner + 20);
    float* ytop = (float*)(inner + 24);

    // Log beberapa frame pertama untuk verifikasi nilai output
    if (g_logCount < 5) {
        _logf("[radarilt] inner output: xl=%.1f ybot=%.1f xr=%.1f ytop=%.1f",
              *xl, *ybot, *xr, *ytop);
        g_logCount++;
    }

    // Sanity check: nilai screen yang masuk akal (0-4000 pixel)
    float h = *ybot - *ytop;  // tinggi radar di screen
    if (h <= 0.0f || h > 2000.0f) {
        // Coba swap: mungkin ytop > ybot
        h = *ytop - *ybot;
        if (h <= 0.0f || h > 2000.0f) return;

        // Layout terbalik: ytop > ybot
        // Tilt: kurangi ytop (perkecil dari atas)
        *ytop -= h * TILT_FACTOR;
        return;
    }

    // Layout normal: ybot > ytop
    // Tilt: naikkan ytop (perkecil dari atas)
    *ytop += h * TILT_FACTOR;
}

MYMOD(brruham.libradarilt, libradarilt, 1.8, brruham)

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    _log("[radarilt] === v1.8 ===");
}

ON_MOD_LOAD() {
    _log("[radarilt] OnModLoad");

    uintptr_t base = 0;
    {
        FILE* maps = fopen("/proc/self/maps", "r");
        if (!maps) { _log("ERROR: maps"); return; }
        char line[512];
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "libGTASA.so") && strstr(line, "r-xp")) {
                base = (uintptr_t)strtoul(line, nullptr, 16);
                break;
            }
        }
        fclose(maps);
    }
    if (!base) { _log("ERROR: base"); return; }
    _logf("[radarilt] base=0x%08X", (unsigned)base);

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { _log("ERROR: dobby"); return; }
    auto dHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dHook) { _log("ERROR: sym"); return; }

    void* t = (void*)(base + OFF_WidgetUpdate + 1);
    int r = dHook(t, (void*)hk_WidgetUpdate, (void**)&orig_WidgetUpdate);
    _logf("[radarilt] Hook WidgetUpdate ret=%d", r);

    _logf("[radarilt] TILT=%.2f", TILT_FACTOR);
    _log("[radarilt] SELESAI");
    if (aml) aml->ShowToast(false, "[RadarTilt] v1.8");
}
