// libradarilt v1.9
// inner output confirmed: xl=74.8 ybot=1004.4 xr=299.8 ytop=779.5
// tinggi = 224.9px, center_y = (1004.4+779.5)/2 = 891.95
//
// Masalah v1.8: modif SETELAH orig → nilai sudah dipakai untuk render frame ini.
// Fix: modif SEBELUM orig dipanggil menggunakan cached value dari frame sebelumnya.
// Frame pertama: tidak ada efek (belum ada cache).
// Frame kedua+: patch dengan cache, lalu orig jalan dengan nilai yang sudah dimodif.
// Setelah orig selesai, restore ke nilai asli untuk konsistensi state.

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

// Tilt: naikkan ytop sebesar TILT_FACTOR * height
// ytop=779.5, ybot=1004.4, h=224.9
// TILT=0.3 → ytop += 67.5 → ytop=847 → radar terlihat lebih pendek di atas
#define TILT_FACTOR  0.30f

// inner layout (confirmed dari log):
// [12]=xl, [16]=ybot, [20]=xr, [24]=ytop

typedef void (*tWidgetUpdate)(void*);
static tWidgetUpdate orig_WidgetUpdate = nullptr;

// Cache nilai output dari frame sebelumnya
static float g_xl   = 0, g_ybot = 0, g_xr = 0, g_ytop = 0;
static bool  g_hasCache = false;
static int   g_logN = 0;

static void hk_WidgetUpdate(void* self) {
    uintptr_t inner = *(uintptr_t*)((uintptr_t)self + 0x88);

    if (inner && g_hasCache) {
        float* xl   = (float*)(inner + 12);
        float* ybot = (float*)(inner + 16);
        float* xr   = (float*)(inner + 20);
        float* ytop = (float*)(inner + 24);

        float h = g_ybot - g_ytop;  // tinggi dari cache
        if (h > 0.0f && h < 2000.0f) {
            // Tulis cache ke inner SEBELUM orig dipanggil
            *xl   = g_xl;
            *ybot = g_ybot;
            *xr   = g_xr;
            *ytop = g_ytop + h * TILT_FACTOR;  // tilt: naikkan ytop

            if (g_logN < 3) {
                _logf("[radarilt] PATCH: ytop %.1f→%.1f (h=%.1f)",
                      g_ytop, *ytop, h);
                g_logN++;
            }
        }
    }

    orig_WidgetUpdate(self);

    // Baca output terbaru untuk cache frame berikutnya
    if (inner) {
        float* xl   = (float*)(inner + 12);
        float* ybot = (float*)(inner + 16);
        float* xr   = (float*)(inner + 20);
        float* ytop = (float*)(inner + 24);

        // Sanity check sebelum cache
        float h = *ybot - *ytop;
        if (h > 0.0f && h < 2000.0f) {
            g_xl      = *xl;
            g_ybot    = *ybot;
            g_xr      = *xr;
            g_ytop    = *ytop;
            g_hasCache = true;
        }
    }
}

MYMOD(brruham.libradarilt, libradarilt, 1.9, brruham)

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    _log("[radarilt] === v1.9 ===");
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
    if (aml) aml->ShowToast(false, "[RadarTilt] v1.9");
}
