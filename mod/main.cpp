// libradarilt v1.5
// TransformRPtoSS confirmed dipanggil. Fix: bounds dari in.y == ±1 exact.
// in.y range persis [-1, +1] untuk tile radar, kita pakai itu langsung.

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

#define OFF_TransformRPtoSS  0x0043F5DCu
#define OFF_DrawBlips        0x0043E99Cu

struct CVector2D { float x, y; };

// ─── Tilt ─────────────────────────────────────────────────────────────────────
// TILT_TOP: skala Y sisi atas radar (in.y = -1). < 1.0 = dikompres ke center.
// TILT_BOT: skala Y sisi bawah radar (in.y = +1). 1.0 = normal.
// Efek: atas "mundur", bawah "maju" → kesan miring 3D.
#define TILT_TOP  0.10f
#define TILT_BOT  1.00f

// Kita derive screen center Y dari pasangan:
//   in.y = -1.0 → out.y = top_screen
//   in.y = +1.0 → out.y = bot_screen
// Catat keduanya, lalu lock.

static float g_topY    = 0.0f;
static float g_botY    = 0.0f;
static bool  g_hasTop  = false;
static bool  g_hasBot  = false;
static bool  g_ready   = false;
static float g_centerY = 0.0f;

// Toleransi: in.y dianggap -1 atau +1 jika dalam range ini
#define EDGE_TOL 0.02f

// ─── Hook TransformRPtoSS ─────────────────────────────────────────────────────
typedef void (*tTransform)(CVector2D&, const CVector2D&);
static tTransform orig_Transform = nullptr;

static void hk_Transform(CVector2D& out, const CVector2D& in) {
    orig_Transform(out, in);

    // Fase 1: calibrasi - catat posisi tepi atas dan bawah radar di screen
    if (!g_ready) {
        if (!g_hasTop && in.y <= -1.0f + EDGE_TOL) {
            g_topY   = out.y;
            g_hasTop = true;
            _logf("[radarilt] Calibrate TOP: in.y=%.3f out.y=%.1f", in.y, out.y);
        }
        if (!g_hasBot && in.y >= 1.0f - EDGE_TOL) {
            g_botY   = out.y;
            g_hasBot = true;
            _logf("[radarilt] Calibrate BOT: in.y=%.3f out.y=%.1f", in.y, out.y);
        }
        if (g_hasTop && g_hasBot && g_botY > g_topY) {
            g_centerY = (g_topY + g_botY) * 0.5f;
            g_ready   = true;
            _logf("[radarilt] READY: topY=%.1f botY=%.1f centerY=%.1f",
                  g_topY, g_botY, g_centerY);
        }
        return;
    }

    // Fase 2: terapkan tilt
    // t: 0 = atas (in.y=-1), 1 = bawah (in.y=+1)
    float t     = (in.y + 1.0f) * 0.5f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float scale  = TILT_TOP + (TILT_BOT - TILT_TOP) * t;
    float dy     = out.y - g_centerY;
    float origY  = out.y;
    out.y        = g_centerY + dy * scale;

    // Log 5 sample pertama setelah READY untuk verifikasi
    static int dbgN = 0;
    if (dbgN < 5) {
        _logf("[radarilt] TILT applied: in.y=%.3f origY=%.1f newY=%.1f scale=%.3f",
              in.y, origY, out.y, scale);
        dbgN++;
    }
}

// ─── Hook DrawBlips: reset kalibrasi tiap ~10 detik ──────────────────────────
typedef void (*tVoidF)(float);
static tVoidF orig_DrawBlips = nullptr;
static int    g_blipFrame    = 0;

static void hk_DrawBlips(float f) {
    g_blipFrame++;
    if (g_blipFrame % 600 == 0) {
        g_hasTop = g_hasBot = g_ready = false;
        _log("[radarilt] Recalibrate triggered");
    }
    if (orig_DrawBlips) orig_DrawBlips(f);
}

// ─── AML ──────────────────────────────────────────────────────────────────────
MYMOD(brruham.libradarilt, libradarilt, 1.5, brruham)

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    _log("[radarilt] === v1.5 ===");
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

    void* tT = (void*)(base + OFF_TransformRPtoSS + 1);
    int rT = dHook(tT, (void*)hk_Transform, (void**)&orig_Transform);
    _logf("[radarilt] Hook Transform ret=%d", rT);

    void* tB = (void*)(base + OFF_DrawBlips + 1);
    int rB = dHook(tB, (void*)hk_DrawBlips, (void**)&orig_DrawBlips);
    _logf("[radarilt] Hook DrawBlips ret=%d", rB);

    _logf("[radarilt] TILT top=%.2f bot=%.2f", TILT_TOP, TILT_BOT);
    _log("[radarilt] SELESAI");
    if (aml) aml->ShowToast(false, "[RadarTilt] v1.5");
}
