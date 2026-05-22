// libradarilt v1.7 - diagnostic pointer chain + fallback CWidgetRadar

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

#define OFF_DrawRadarMap   0x0043E5A0u
#define OFF_DrawBlips      0x0043E99Cu
// CWidgetRadar::Update @ 0x002BF798 - ini update posisi widget radar
// r4 = CWidgetRadar*, r4[0x88] = pointer ke sesuatu (lihat disassembly)
// r4[32..44] = x1,y1,x2,y2 dari widget (screen coords, ditulis ke r0[12..24])
// r0 = r4[0x88]
#define OFF_WidgetUpdate   0x002BF798u

#define TILT_FACTOR 0.30f

static uintptr_t g_base    = 0;
static bool      g_logged  = false;

// ─── Pointer chain dari DrawRadar disassembly ─────────────────────────────────
// 437b32: ldr r0,[pc,#0x4ac] → PC=0x437b36 → literal at 0x437FE2
// 0x437FE2 berisi relative offset (Thumb PC-rel: (PC & ~3) + offset)
// PC = 0x437b36, align4 = 0x437b34, + 0x4ac = 0x437FE0 ... hmm
// Mari hitung ulang dengan benar:
// ldr.w r0,[pc,#0x4ac] di addr 0x437b32 (4-byte Thumb2 instruction)
// PC = 0x437b32 + 4 = 0x437b36
// target = (0x437b36 & ~3) + 0x4ac = 0x437b34 + 0x4ac = 0x437FE0
// Jadi literal pool di 0x437FE0, bukan 0x437FE2
#define OFF_LITERAL_DRAWRADAR  0x00437FE0u  // literal pool untuk global ptr

// ─── Approach baru: scan memori untuk nilai rect yang masuk akal ─────────────
// Dari v1.6 log: PTR_TABLE → global=0xDEC9D4F0
// Kita log isi pointer chain step by step

static uintptr_t resolveViaDrawRadar() {
    // Dari 437b32: ldr.w r0,[pc,#0x4ac]
    // PC=0x437b36, align4(PC)=0x437b34, +0x4ac = 0x437FE0
    uint32_t rel       = *(uint32_t*)(g_base + OFF_LITERAL_DRAWRADAR);
    // rel adalah offset dari PC 0x437b36 ke global var address
    // add r0,pc → r0 = 0x437b36 + rel (tapi ini adalah address of pointer, bukan value)
    uintptr_t ptr_addr = (g_base + 0x437b36) + rel;
    uintptr_t ptr_val  = *(uintptr_t*)ptr_addr;

    _logf("[radarilt] DrawRadar literal=0x%08X rel=0x%08X ptr_addr=0x%08X ptr_val=0x%08X",
          (unsigned)(g_base+OFF_LITERAL_DRAWRADAR), rel,
          (unsigned)ptr_addr, (unsigned)ptr_val);

    if (!ptr_val) return 0;
    uintptr_t radar_struct = *(uintptr_t*)(ptr_val + 0x284);
    _logf("[radarilt] ptr_val[0x284]=0x%08X (radar_struct)", (unsigned)radar_struct);

    return radar_struct;
}

// ─── Approach CWidgetRadar ────────────────────────────────────────────────────
// Dari disassembly CWidgetRadar::Update:
//   r4 = this (CWidgetRadar*)
//   r4[0x88] = inner pointer (r0)
//   r4[32]=x1, r4[36]=y1, r4[40]=x2, r4[44]=y2  (input widget bounds)
//   Output ditulis ke r0[12], r0[16], r0[20], r0[24]
// Kita hook Update, modif r4[36] dan r4[44] SEBELUM kalkulasi

typedef void (*tWidgetUpdate)(void*);
static tWidgetUpdate orig_WidgetUpdate = nullptr;
static bool g_widgetLogged = false;

static void hk_WidgetUpdate(void* self) {
    float* x1 = (float*)((uintptr_t)self + 32);
    float* y1 = (float*)((uintptr_t)self + 36);
    float* x2 = (float*)((uintptr_t)self + 40);
    float* y2 = (float*)((uintptr_t)self + 44);

    if (!g_widgetLogged) {
        _logf("[radarilt] WidgetUpdate self=0x%08X bounds=(%.1f,%.1f,%.1f,%.1f)",
              (unsigned)(uintptr_t)self, *x1, *y1, *x2, *y2);
        // Log inner pointer
        void** inner = (void**)((uintptr_t)self + 0x88);
        _logf("[radarilt] inner[0x88]=0x%08X", (unsigned)(uintptr_t)*inner);
        if (*inner) {
            float* ox1 = (float*)((uintptr_t)*inner + 12);
            float* oy1 = (float*)((uintptr_t)*inner + 16);
            float* ox2 = (float*)((uintptr_t)*inner + 20);
            float* oy2 = (float*)((uintptr_t)*inner + 24);
            _logf("[radarilt] inner output bounds=(%.1f,%.1f,%.1f,%.1f)",
                  *ox1, *oy1, *ox2, *oy2);
        }
        g_widgetLogged = true;
    }

    float h = *y2 - *y1;
    float savedY1 = *y1;
    if (h > 0.0f && h < 2000.0f) {
        *y1 += h * TILT_FACTOR;
    }

    orig_WidgetUpdate(self);

    *y1 = savedY1;
}

// ─── DrawRadarMap hook: diagnostic pointer chain ──────────────────────────────
typedef void (*tVoid)(void);
static tVoid orig_DrawRadarMap = nullptr;
static bool  g_drLogged = false;

static void hk_DrawRadarMap() {
    if (!g_drLogged) {
        uintptr_t rs = resolveViaDrawRadar();
        if (rs) {
            float* x1 = (float*)(rs + 32);
            float* y1 = (float*)(rs + 36);
            float* x2 = (float*)(rs + 40);
            float* y2 = (float*)(rs + 44);
            _logf("[radarilt] radarStruct rect=(%.1f,%.1f,%.1f,%.1f)",
                  *x1, *y1, *x2, *y2);
        }
        g_drLogged = true;
    }
    orig_DrawRadarMap();
}

MYMOD(brruham.libradarilt, libradarilt, 1.7, brruham)

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    _log("[radarilt] === v1.7 ===");
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
    g_base = base;
    _logf("[radarilt] base=0x%08X", (unsigned)base);

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { _log("ERROR: dobby"); return; }
    auto dHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dHook) { _log("ERROR: sym"); return; }

    // Hook DrawRadarMap untuk diagnostic pointer chain
    {
        void* t = (void*)(base + OFF_DrawRadarMap + 1);
        int r = dHook(t, (void*)hk_DrawRadarMap, (void**)&orig_DrawRadarMap);
        _logf("[radarilt] Hook DrawRadarMap ret=%d", r);
    }

    // Hook CWidgetRadar::Update - pendekatan alternatif
    {
        void* t = (void*)(base + OFF_WidgetUpdate + 1);
        int r = dHook(t, (void*)hk_WidgetUpdate, (void**)&orig_WidgetUpdate);
        _logf("[radarilt] Hook WidgetUpdate ret=%d", r);
    }

    _log("[radarilt] SELESAI");
    if (aml) aml->ShowToast(false, "[RadarTilt] v1.7");
}
