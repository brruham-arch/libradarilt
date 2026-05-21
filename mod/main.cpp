// libradarilt v1.4 - DIAGNOSTIC ONLY
// Hook semua fungsi radar, log mana yang terpanggil saat ingame.
// Tidak ada modifikasi visual — murni untuk identifikasi call chain.

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

// ─── Offsets ──────────────────────────────────────────────────────────────────
#define OFF_CHud_DrawRadar            0x00437ABCu  // CHud::DrawRadar
#define OFF_DrawRadarMap              0x0043E5A0u  // CRadar::DrawRadarMap
#define OFF_DrawBlips                 0x0043E99Cu  // CRadar::DrawBlips(float)
#define OFF_DrawRadarMask             0x00444150u  // CRadar::DrawRadarMask
#define OFF_DrawRadarSection          0x004437B0u  // CRadar::DrawRadarSection(int,int,int)
#define OFF_DrawRadarSectionMap       0x00443B40u  // CRadar::DrawRadarSectionMap
#define OFF_TransformRPtoSS           0x0043F5DCu  // TransformRadarPointToScreenSpace
#define OFF_TransformRWtoRS           0x0043F694u  // TransformRealWorldPointToRadarSpace
#define OFF_DrawCoordBlip             0x0043FA18u  // DrawCoordBlip
#define OFF_DrawEntityBlip            0x0043FED4u  // DrawEntityBlip
#define OFF_DrawRadarSprite           0x0043F764u  // DrawRadarSprite
#define OFF_DrawRotatingRadarSprite   0x00441088u  // DrawRotatingRadarSprite

struct CVector2D { float x, y; };

typedef void (*tVoid)(void);
typedef void (*tVoidF)(float);
typedef void (*tVoidIII)(int,int,int);
typedef void (*tTransform)(CVector2D&, const CVector2D&);

// ─── Per-fungsi: flag sudah-dipanggil + orig pointer ─────────────────────────
#define MAKE_HOOK(NAME) \
    static bool       called_##NAME = false; \
    static tVoid      orig_##NAME   = nullptr; \
    static void hk_##NAME() { \
        if (!called_##NAME) { _log("[radarilt] CALLED: " #NAME); called_##NAME = true; } \
        if (orig_##NAME) orig_##NAME(); \
    }

MAKE_HOOK(DrawRadar)
MAKE_HOOK(DrawRadarMap)
MAKE_HOOK(DrawRadarMask)

static bool   called_DrawBlips = false;
static tVoidF orig_DrawBlips   = nullptr;
static void hk_DrawBlips(float f) {
    if (!called_DrawBlips) { _logf("[radarilt] CALLED: DrawBlips(%.3f)", f); called_DrawBlips = true; }
    if (orig_DrawBlips) orig_DrawBlips(f);
}

static bool     called_DrawRadarSection = false;
static tVoidIII orig_DrawRadarSection   = nullptr;
static void hk_DrawRadarSection(int a, int b, int c) {
    if (!called_DrawRadarSection) {
        _logf("[radarilt] CALLED: DrawRadarSection(%d,%d,%d)", a, b, c);
        called_DrawRadarSection = true;
    }
    if (orig_DrawRadarSection) orig_DrawRadarSection(a, b, c);
}

static bool      called_Transform = false;
static tTransform orig_Transform  = nullptr;
static void hk_Transform(CVector2D& out, const CVector2D& in) {
    if (!called_Transform) {
        orig_Transform(out, in);
        _logf("[radarilt] CALLED: TransformRPtoSS in=(%.3f,%.3f) out=(%.3f,%.3f)",
              in.x, in.y, out.x, out.y);
        called_Transform = true;
        return;
    }
    orig_Transform(out, in);
}

static bool      called_TransformRW = false;
static tTransform orig_TransformRW  = nullptr;
static void hk_TransformRW(CVector2D& out, const CVector2D& in) {
    if (!called_TransformRW) {
        orig_TransformRW(out, in);
        _logf("[radarilt] CALLED: TransformRWtoRS in=(%.3f,%.3f) out=(%.3f,%.3f)",
              in.x, in.y, out.x, out.y);
        called_TransformRW = true;
        return;
    }
    orig_TransformRW(out, in);
}

// ─── AML ──────────────────────────────────────────────────────────────────────
MYMOD(brruham.libradarilt, libradarilt, 1.4, brruham)

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    _log("[radarilt] === v1.4 DIAGNOSTIC ===");
}

ON_MOD_LOAD() {
    _log("[radarilt] === OnModLoad ===");

    uintptr_t base = 0;
    {
        FILE* maps = fopen("/proc/self/maps", "r");
        if (!maps) { _log("[radarilt] ERROR: maps"); return; }
        char line[512];
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "libGTASA.so") && strstr(line, "r-xp")) {
                base = (uintptr_t)strtoul(line, nullptr, 16);
                break;
            }
        }
        fclose(maps);
    }
    if (!base) { _log("[radarilt] ERROR: base"); return; }
    _logf("[radarilt] base = 0x%08X", (unsigned)base);

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { _log("[radarilt] ERROR: dobby"); return; }
    auto dobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { _log("[radarilt] ERROR: sym"); return; }

#define HOOK(NAME, OFF, hk, orig) { \
    void* t = (void*)(base + (OFF) + 1); \
    int r = dobbyHook(t, (void*)(hk), (void**)(orig)); \
    _logf("[radarilt] hook %-30s ret=%d", #NAME, r); \
}

    HOOK(CHud_DrawRadar,          OFF_CHud_DrawRadar,          hk_DrawRadar,          &orig_DrawRadar)
    HOOK(DrawRadarMap,            OFF_DrawRadarMap,             hk_DrawRadarMap,       &orig_DrawRadarMap)
    HOOK(DrawRadarMask,           OFF_DrawRadarMask,            hk_DrawRadarMask,      &orig_DrawRadarMask)
    HOOK(DrawBlips,               OFF_DrawBlips,                hk_DrawBlips,          &orig_DrawBlips)
    HOOK(DrawRadarSection,        OFF_DrawRadarSection,         hk_DrawRadarSection,   &orig_DrawRadarSection)
    HOOK(TransformRPtoSS,         OFF_TransformRPtoSS,          hk_Transform,          &orig_Transform)
    HOOK(TransformRWtoRS,         OFF_TransformRWtoRS,          hk_TransformRW,        &orig_TransformRW)

    _log("[radarilt] Semua hook terpasang. Masuk game, lihat mana yang CALLED.");
    _log("[radarilt] === OnModLoad SELESAI ===");
    if (aml) aml->ShowToast(false, "[RadarTilt] v1.4 Diagnostic");
}
