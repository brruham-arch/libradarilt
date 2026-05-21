// libradarilt v1.3
// Derive radar bounds dari titik-titik yang lewat TransformRadarPointToScreenSpace
// lalu terapkan perspective tilt langsung tanpa butuh m_radarRect.

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
#define OFF_TransformRPtoSS  0x0043F5DCu
#define OFF_DrawRadarMap     0x0043E5A0u

struct CVector2D { float x, y; };

// ─── Tilt param ───────────────────────────────────────────────────────────────
// Perspektif: in.y = -1 (atas radar) sampai +1 (bawah radar)
// Kita map ke scale Y: atas dikompres, bawah normal.
// TILT = seberapa besar efek. 0 = flat, 0.5 = cukup kentara.
#define TILT  0.45f

// ─── Radar bounds autodiscovery ───────────────────────────────────────────────
// Kita amati in.y == -1.0 (tepi atas) dan in.y == +1.0 (tepi bawah)
// untuk tahu out.y min/max → dapat center dan half-height layar.
// Ini akurat karena game selalu menggambar tile dari tepi ke tepi.

static float g_screenCY  = 0.0f;
static float g_screenHH  = 0.0f;
static bool  g_boundsOK  = false;

// Accumulator: kumpulkan min/max out.y selama ~60 call, lalu lock
static float g_minY      =  1e9f;
static float g_maxY      = -1e9f;
static int   g_sampleCnt = 0;
#define SAMPLE_LOCK 80

// ─── Hook TransformRadarPointToScreenSpace ────────────────────────────────────
typedef void (*tTransform)(CVector2D& out, const CVector2D& in);
static tTransform orig_Transform = nullptr;

static void hk_Transform(CVector2D& out, const CVector2D& in) {
    orig_Transform(out, in);

    // Phase 1: sampling bounds
    if (!g_boundsOK) {
        if (out.y < g_minY) g_minY = out.y;
        if (out.y > g_maxY) g_maxY = out.y;
        g_sampleCnt++;
        if (g_sampleCnt == SAMPLE_LOCK && g_maxY > g_minY) {
            g_screenCY = (g_minY + g_maxY) * 0.5f;
            g_screenHH = (g_maxY - g_minY) * 0.5f;
            g_boundsOK = true;
            _logf("[radarilt] Bounds locked: minY=%.1f maxY=%.1f cy=%.1f hh=%.1f",
                  g_minY, g_maxY, g_screenCY, g_screenHH);
        }
        return; // jangan modif selama sampling
    }

    // Phase 2: terapkan tilt
    // in.y range [-1, +1]: -1=atas, +1=bawah
    // t: 0=atas, 1=bawah
    float t = (in.y + 1.0f) * 0.5f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    // scale: atas = (1-TILT), bawah = 1.0
    float scale = (1.0f - TILT) + TILT * t;

    float dy = out.y - g_screenCY;
    out.y    = g_screenCY + dy * scale;
}

// ─── Hook DrawRadarMap: reset sampling setiap kali map digambar ───────────────
// Ini opsional tapi memastikan bounds selalu fresh jika ukuran radar berubah.
typedef void (*tVoid)(void);
static tVoid orig_DrawRadarMap = nullptr;
static int   g_frameCount = 0;

static void hk_DrawRadarMap() {
    g_frameCount++;
    // Re-sample tiap 300 frame (~5 detik) untuk adaptasi ukuran radar
    if (g_frameCount % 300 == 0) {
        g_boundsOK  = false;
        g_sampleCnt = 0;
        g_minY      =  1e9f;
        g_maxY      = -1e9f;
    }
    orig_DrawRadarMap();
}

// ─── AML ──────────────────────────────────────────────────────────────────────
MYMOD(brruham.libradarilt, libradarilt, 1.3, brruham)

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    _log("[radarilt] === OnModPreLoad v1.3 ===");
}

ON_MOD_LOAD() {
    _log("[radarilt] === OnModLoad mulai ===");

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
    if (!dobbyHook) { _log("[radarilt] ERROR: DobbyHook sym"); return; }

    {
        void* t = (void*)(base + OFF_TransformRPtoSS + 1);
        int r = dobbyHook(t, (void*)hk_Transform, (void**)&orig_Transform);
        _logf("[radarilt] Hook Transform: ret=%d", r);
    }
    {
        void* t = (void*)(base + OFF_DrawRadarMap + 1);
        int r = dobbyHook(t, (void*)hk_DrawRadarMap, (void**)&orig_DrawRadarMap);
        _logf("[radarilt] Hook DrawRadarMap: ret=%d", r);
    }

    _logf("[radarilt] TILT=%.2f, sampling %d points before lock", TILT, SAMPLE_LOCK);
    _log("[radarilt] === OnModLoad SELESAI ===");
    if (aml) aml->ShowToast(false, "[RadarTilt] v1.3 Aktif");
}
