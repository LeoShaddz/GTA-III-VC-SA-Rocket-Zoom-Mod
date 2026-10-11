// RocketZoom.asi v3.1 - GTA III 1.0 / GTA Vice City 1.0 / GTA San Andreas 1.0 US (a single .asi file)
//
// When aiming with the rocket launcher, you can zoom in / zoom out using the SAME values as each game's sniper rifle
// (FOV limits, zoom speed, and sensitivity reduction while zoomed in):
//
//   GTA III  : GTA III sniper rifle values (FOV 70 -> 15, speed (255*dt+10000)*0.0001, sens = FOV*0.0125)
//   Vice City: VC sniper rifle values (same formula, read from the VC executable itself)
//   GTA SA   : SA sniper rifle values (minimum 15, same as v2)
//
// GTA III has no camera. In Vice City, mode 46 (photo mission camera, minimum FOV 3)
// is NOT used: the minimum comes from the sniper rifle (mode 7).
//
// ================= HOW IT WORKS (III and VC) =================
//  - Camera mode 7 = sniper rifle, mode 8 = rocket launcher (same numbers in both games).
//  - The rocket launcher's camera routine (III 0x461AF0, VC 0x47A20E) does this every frame:
//        mov [cam+FOV], 70.0f      (III: FOV at +0xB0 | VC: FOV at +0xB4)
//    and then uses this FOV as a multiplier for aiming sensitivity (FOV * 0.0125 * input), just like
//    the sniper routine. The plugin replaces this 10-byte "mov" instruction with a "call" to a custom function that writes
//    the zoom FOV. Since the game already uses the camera's FOV field for sensitivity, aiming becomes slower when zoomed in,
//    JUST LIKE the sniper rifle, without changing anything else.
//  - Input: the plugin calls the SAME CPad functions used by the sniper rifle for zoom in / zoom out
//    (III: 0x493E00 in / 0x493E70 out | VC: 0x4A9E90 in / 0x4A9E20 out | GetPad: III 0x492F60, VC 0x4AB060),
//    so the buttons/wheel configured for sniper zoom also work with the rocket launcher.
//  - Mouse wheel (same as the sniper rifle): each wheel step moves the zoom target by +-10 degrees, and the FOV moves toward it using the
//    game's own smoothing function (III 0x456F40 / VC 0x4864E3, factor 0.5 / 0.25). This only works if sniper zoom
//    is bound to the wheel in the controls, just like in the original game (checked via GetControllerKeyAssociatedWithAction).
//  - Page Up / Page Down and buttons (held): continuous zoom, with FOV *= f or /= f each frame, f = ...
//  - Speed: each frame, FOV *= f (zoom out) or FOV /= f (zoom in), f = (255*timestep + 10000) * 0.0001
//    (constants read from each game's executable; timestep = CTimer::ms_fTimeStep).
//  - Limits: minimum and maximum read from the executable (15.0 and 70.0), unless the .ini specifies different values.
//  - Nothing is written if the game's bytes do not match the expected values (signature check).
//
// ================= GTA SA (same as v2) =================
//  Camera modes 8 (RPG) and 51 (Heat-Seeker); hook in CDraw::SetFOV (call at 0x52C976) multiplies the FOV by
//  zoom/70; sensitivity: both "fld [esi+0xB4]" instructions in CCam::Process_Rocket (0x511D3E / 0x511DEC) read a
//  plugin variable. Mouse wheel (CPad mouse Z 0xB73420) and RT/LT (CPad 0xB73458, +0x0E/+0x0A).
//
// NONE OF THIS HAS BEEN COMPILED OR TESTED IN-GAME.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

enum Game { G_NONE = 0, G_III, G_VC, G_SA };

static HMODULE hSelf = NULL;
static char    gIni[MAX_PATH];
static bool    gLog = false;
static Game    gGame = G_NONE;

// ---- .ini options ----
static float gMinFovCfg = 0.0f;    // 0 = use the game's sniper minimum
static float gMaxFovCfg = 0.0f;    // 0 = use the game's sniper maximum
static float gSpeed     = 1.0f;
static bool  gWheel     = true;    // SA only
static bool  gPadKeys   = true;    // SA only
static bool  gHeatSeek  = true;    // SA only
static bool  gScaleSens = true;

static float gMinFov = 15.0f;
static float gMaxFov = 70.0f;
static float gTarget = 70.0f;
static float gCur    = 70.0f;
static int   gLogCount = 0;
static volatile float gAimFov = 70.0f;   // variable read by the patched sensitivity "fld" instructions

static void Log(const char* fmt, ...)
{
    if (!gLog) return;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(hSelf, path, MAX_PATH)) return;
    char* dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".log"); else strcat(path, ".log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

// ---------- Safe reading (a game's addresses do not exist in the other games' executables) ----------
static bool Readable(uintptr_t a, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t end = a + n;
    while (a < end)
    {
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}
static bool BytesAre(uintptr_t a, const uint8_t* b, size_t n)
{
    return Readable(a, n) && memcmp((const void*)a, b, n) == 0;
}
static float ReadF(uintptr_t a) { return *(volatile float*)a; }

static bool WriteBytes(uintptr_t at, const uint8_t* b, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)at, b, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
    return true;
}

static float ReadIniFloat(const char* key, float def)
{
    char buf[32], d[32];
    sprintf(d, "%g", def);
    GetPrivateProfileStringA("RocketZoom", key, d, buf, sizeof buf, gIni);
    return (float)atof(buf);
}

static void ClampLimits(float defMin, float defMax)
{
    gMinFov = gMinFovCfg > 0.0f ? gMinFovCfg : defMin;
    gMaxFov = gMaxFovCfg > 0.0f ? gMaxFovCfg : defMax;
    if (gMinFov < 1.0f) gMinFov = 1.0f;
    if (gMaxFov > 70.0f) gMaxFov = 70.0f;
    if (gMinFov > gMaxFov) gMinFov = gMaxFov;
    gTarget = gCur = gMaxFov;
}

// Patch "fld DWORD PTR [reg+FOV]" (6 bytes) -> "fld DWORD PTR ds:[&gAimFov]" (6 bytes)
static bool PatchSensToVar(uintptr_t at)
{
    uint8_t b[6];
    b[0] = 0xD9; b[1] = 0x05;
    *(uint32_t*)(b + 2) = (uint32_t)(uintptr_t)&gAimFov;
    return WriteBytes(at, b, 6);
}

// =====================================================================================================
//                                      GTA III / VICE CITY
// =====================================================================================================
struct OldGameInfo
{
    const char* name;
    uintptr_t siteForce70;            // mov [cam+FOV], 70.0f (10 bytes) in the rocket launcher's routine
    uint8_t   forceBytes[10];
    uintptr_t siteSens1, siteSens2;   // fld [cam+FOV] (6 bytes): rocket launcher aiming sensitivity
    uint8_t   sensBytes[6];
    uintptr_t fnGetPad;               // CPad* __cdecl GetPad(int)
    uintptr_t fnZoomIn, fnZoomOut;    // bool CPad::xxx() (thiscall) - the same functions used by the sniper rifle
    uint8_t   padSig[5];              // start of the zoom functions (signature)
    uintptr_t timeStep;               // CTimer::ms_fTimeStep
    uintptr_t c255, c10000, c1e4;     // constants in the sniper zoom speed formula
    uintptr_t minFov, maxFov;         // sniper FOV limits (15.0 and 70.0)
    uint32_t  fovOffset;              // FOV offset in CCam
    // --- Mouse wheel zoom step (same as the sniper rifle) ---
    uintptr_t ctrlMgr;                // ControlsManager (this pointer for GetControllerKeyAssociatedWithAction)
    uintptr_t fnKeyAssoc;             // int __thiscall GetControllerKeyAssociatedWithAction(int action) (returns 4)
    uintptr_t wheelFlagA, wheelFlagB; // CPad bytes: mouse wheel up / down this frame
    uintptr_t fnSmooth;               // void __cdecl Smooth(float target, float* cur, float* vel, float, float, int)
    uintptr_t cStep, cHalf, cQuarter; // 10.0 (step), 0.5 (threshold), 0.25 (smoothing factor)
};

static const OldGameInfo INFO_III = {
    "GTA III 1.0",
    0x461B25, { 0xC7, 0x85, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8C, 0x42 },
    0x461D11, 0x461D9E, { 0xD9, 0x85, 0xB0, 0x00, 0x00, 0x00 },
    0x492F60, 0x493E00, 0x493E70, { 0x89, 0xCA, 0x80, 0xBA, 0xDF },
    0x8E2CB4,
    0x5F0670, 0x5F0674, 0x5F0678,
    0x5F0480, 0x5F067C,
    0xB0,
    0x8F43A4, 0x58F740, 0x8809F3, 0x8809F4,
    0x456F40,
    0x5F0428, 0x5F0464, 0x5F049C
};
static const OldGameInfo INFO_VC = {
    "Vice City 1.0",
    0x47A24D, { 0xC7, 0x85, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8C, 0x42 },
    0x47A470, 0x47A4F7, { 0xD9, 0x85, 0xB4, 0x00, 0x00, 0x00 },
    0x4AB060, 0x4A9E90, 0x4A9E20, { 0x89, 0xCA, 0x66, 0x83, 0xBA },
    0x975424,
    0x68AE8C, 0x68ADC4, 0x68B014,
    0x68AE28, 0x68AEC0,
    0xB4,
    0x9B491C, 0x615F60, 0x94D78B, 0x94D78C,
    0x4864E3,
    0x68AF74, 0x68AE08, 0x68AF24
};
static const OldGameInfo* gOld = NULL;

typedef void* (__cdecl *GetPad_t)(int);
typedef bool  (__thiscall *PadFn_t)(void*);
typedef int   (__thiscall *KeyAssoc_t)(void*, int);
typedef void  (__cdecl *Smooth_t)(float, float*, float*, float, float, int);

static LARGE_INTEGER gQpcFreq;
static double        gLastMs = -1e9;
static float         gVel = 0.0f;       // FOV "velocity" used by the game's smoothing function (field +0xB4/+0xB8 in the sniper CCam)

static double NowMs()
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)gQpcFreq.QuadPart;
}

// Reproduces the zoom block from the sniper routine (III 0x462420 / VC 0x479410):
//  1) Mouse wheel (if zoom is bound to the wheel and it is rotated this frame): step of +-10 degrees
//     (target = FOV -/+ 10), and the FOV moves toward the target using the game's own smoothing function;
//  2) PageUp/PageDown / buttons (held): FOV *= f or /= f each frame; the target becomes the current FOV.
static void OldZoomStep(const OldGameInfo* g)
{
    void* pad = ((GetPad_t)g->fnGetPad)(0);
    bool step = false;

    int k7 = ((KeyAssoc_t)g->fnKeyAssoc)((void*)g->ctrlMgr, 7);
    int k8 = ((KeyAssoc_t)g->fnKeyAssoc)((void*)g->ctrlMgr, 8);
    bool wheelBound = (k7 != 0) && ((k7 == 4 || k7 == 5) || k8 == 4 || k8 == 5);
    if (wheelBound && (*(volatile uint8_t*)g->wheelFlagA || *(volatile uint8_t*)g->wheelFlagB))
    {
        if (((PadFn_t)g->fnZoomIn)(pad))       { gTarget = gCur - ReadF(g->cStep); step = true; }
        else if (((PadFn_t)g->fnZoomOut)(pad)) { gTarget = gCur + ReadF(g->cStep); step = true; }
    }

    bool zout = ((PadFn_t)g->fnZoomOut)(pad);
    bool zin  = zout ? false : ((PadFn_t)g->fnZoomIn)(pad);
    if ((zout || zin) && !step)
    {
        const float ts = ReadF(g->timeStep);
        float f = (ReadF(g->c255) * ts + ReadF(g->c10000)) * ReadF(g->c1e4);   // sniper zoom formula
        f = 1.0f + (f - 1.0f) * gSpeed;
        if (zout) gCur *= f; else gCur /= f;
        gVel = 0.0f;
        gTarget = gCur;
    }
    else
    {
        float d = gTarget - gCur;
        if (d < 0.0f) d = -d;
        if (d > ReadF(g->cHalf))
            ((Smooth_t)g->fnSmooth)(gTarget, &gCur, &gVel, ReadF(g->cHalf), ReadF(g->cQuarter), 0);
        else
            gVel = 0.0f;
    }
    if (gCur > gMaxFov) gCur = gMaxFov;
    if (gCur < gMinFov) gCur = gMinFov;
}

// Called by the game instead of "mov [cam+FOV], 70.0f" in the rocket launcher's routine (only runs while aiming with the RPG)
static void __cdecl OldRocketHook(void* cam)
{
    const OldGameInfo* g = gOld;
    double now = NowMs();
    double dt  = now - gLastMs;
    if (dt > 250.0)                     // stopped aiming for a while (or game paused): every new aim starts without zoom
    { gTarget = gCur = gMaxFov; gVel = 0.0f; }
    if (dt > 2.0 || dt < 0.0)           // update only once per frame
    {
        gLastMs = now;
        OldZoomStep(g);
        if (gLogCount < 6 && fabsf(gCur - gMaxFov) > 0.5f)
        { ++gLogCount; Log("Rocket launcher zoom: FOV %.2f target %.2f (min %.1f max %.1f)", gCur, gTarget, gMinFov, gMaxFov); }
    }
    *(float*)((uint8_t*)cam + g->fovOffset) = gCur;
}

__declspec(naked) static void OldRocketStub()
{
    __asm {
        pushfd
        push eax
        push ecx
        push edx
        push ebp            // ebp = CCam* (the game routine uses ebp as "this")
        call OldRocketHook
        add  esp, 4
        pop  edx
        pop  ecx
        pop  eax
        popfd
        ret
    }
}

static bool MatchOld(const OldGameInfo& g)
{
    if (!BytesAre(g.siteForce70, g.forceBytes, sizeof g.forceBytes)) return false;
    if (!BytesAre(g.siteSens1, g.sensBytes, sizeof g.sensBytes)) return false;
    if (!BytesAre(g.siteSens2, g.sensBytes, sizeof g.sensBytes)) return false;
    if (!BytesAre(g.fnZoomIn,  g.padSig, sizeof g.padSig)) return false;
    if (!BytesAre(g.fnZoomOut, g.padSig, sizeof g.padSig)) return false;
    if (!Readable(g.fnGetPad, 1) || !Readable(g.timeStep, 4) || !Readable(g.c255, 4) || !Readable(g.c10000, 4)
        || !Readable(g.c1e4, 4) || !Readable(g.minFov, 4) || !Readable(g.maxFov, 4)) return false;
    if (fabsf(ReadF(g.c255) - 255.0f) > 1e-3f) return false;
    if (fabsf(ReadF(g.c10000) - 10000.0f) > 1e-1f) return false;
    if (fabsf(ReadF(g.c1e4) - 0.0001f) > 1e-7f) return false;
    if (fabsf(ReadF(g.maxFov) - 70.0f) > 1e-3f) return false;
    if (fabsf(ReadF(g.minFov) - 15.0f) > 1e-3f) return false;
    static const uint8_t keySig[7]    = { 0x8B, 0x44, 0x24, 0x04, 0xC1, 0xE0, 0x05 };
    static const uint8_t smoothSig[7] = { 0x83, 0xEC, 0x08, 0x8B, 0x54, 0x24, 0x10 };
    if (!BytesAre(g.fnKeyAssoc, keySig, sizeof keySig)) return false;
    if (!BytesAre(g.fnSmooth, smoothSig, sizeof smoothSig)) return false;
    if (!Readable(g.wheelFlagA, 1) || !Readable(g.wheelFlagB, 1) || !Readable(g.ctrlMgr, 4)) return false;
    if (!Readable(g.cStep, 4) || !Readable(g.cHalf, 4) || !Readable(g.cQuarter, 4)) return false;
    if (fabsf(ReadF(g.cStep) - 10.0f) > 1e-3f) return false;
    if (fabsf(ReadF(g.cHalf) - 0.5f) > 1e-3f) return false;
    if (fabsf(ReadF(g.cQuarter) - 0.25f) > 1e-3f) return false;
    return true;
}

static bool InstallOld(const OldGameInfo& g)
{
    gOld = &g;
    QueryPerformanceFrequency(&gQpcFreq);
    ClampLimits(ReadF(g.minFov), ReadF(g.maxFov));      // game's sniper minimum/maximum

    uint8_t patch[10];
    patch[0] = 0xE8;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)&OldRocketStub - (g.siteForce70 + 5));
    memset(patch + 5, 0x90, 5);
    bool ok = WriteBytes(g.siteForce70, patch, sizeof patch);

    // ScaleSensitivity = 0: aiming does not become slower when zoomed in (reads a fixed FOV of 70 for the sensitivity calculation)
    if (ok && !gScaleSens)
    {
        gAimFov = 70.0f;
        bool a = PatchSensToVar(g.siteSens1), b = PatchSensToVar(g.siteSens2);
        if (!a || !b) Log("Fixed sensitivity: failed to write (%d,%d)", (int)a, (int)b);
    }
    return ok;
}

// =====================================================================================================
//                                              GTA SA 1.0 US (same as v2)
// =====================================================================================================
static const uintptr_t SA_VERSION_ADDR = 0x82457C;
static const uint32_t  SA_VERSION_OK   = 0x94BF;
static const uintptr_t SA_SITE_SETFOV  = 0x52C976;
static const uintptr_t SA_FN_SETFOV    = 0x6FF410;
static const uintptr_t SA_THE_CAMERA   = 0xB6F028;
static const uintptr_t SA_PAD0         = 0xB73458;
static const uintptr_t SA_MOUSE_Z      = 0xB73420;
static const uintptr_t SA_TIMESTEP     = 0xB7CB5C;
static const uintptr_t SA_FRAME_COUNT  = 0xB7CB4C;
static const uintptr_t SA_SITE_SENS_PAD   = 0x511D3E;
static const uintptr_t SA_SITE_SENS_MOUSE = 0x511DEC;
static const uint8_t   SA_SENS_ORIG[6]    = { 0xD9, 0x86, 0xB4, 0x00, 0x00, 0x00 };
static const int SA_MODE_ROCKET    = 8;
static const int SA_MODE_ROCKET_HS = 51;

typedef void (__cdecl *SetFOV_t)(float fov);
static unsigned gLastFrame = 0xFFFFFFFFu;

static int SA_ActiveCamMode()
{
    uint8_t active = *(volatile uint8_t*)(SA_THE_CAMERA + 0x59);
    if (active > 2) return -1;
    return *(volatile uint16_t*)(SA_THE_CAMERA + 0x180 + (uintptr_t)active * 0x238);
}
static bool SA_AimingRocket(int mode)
{
    return mode == SA_MODE_ROCKET || (gHeatSeek && mode == SA_MODE_ROCKET_HS);
}
static inline float ClampFov(float v) { return v < gMinFov ? gMinFov : (v > gMaxFov ? gMaxFov : v); }

static void SA_UpdateZoom()
{
    const float ts = *(volatile float*)SA_TIMESTEP;
    float tgt = gTarget;
    if (gWheel)
    {
        float z = *(volatile float*)SA_MOUSE_Z;
        if (z != 0.0f)
        {
            float f = (fabsf(z) * 7.0f + 10000.0f) * 0.0001f;
            f = 1.0f + (f - 1.0f) * gSpeed;
            tgt = (z > 0.0f) ? tgt / f : tgt * f;
        }
    }
    if (gPadKeys)
    {
        volatile short* pad = (volatile short*)SA_PAD0;
        bool zin  = pad[0x0E / 2] > 30;
        bool zout = pad[0x0A / 2] > 30;
        float f = (ts * 255.0f + 10000.0f) * 0.0001f;
        f = 1.0f + (f - 1.0f) * gSpeed;
        if (zin && !zout)      tgt /= f;
        else if (zout && !zin) tgt *= f;
    }
    gTarget = ClampFov(tgt);
    gCur += (gTarget - gCur) * 0.35f;
    if (fabsf(gTarget - gCur) < 0.01f) gCur = gTarget;
}

static void __cdecl SA_HookSetFOV(float fov)
{
    int mode = SA_ActiveCamMode();
    if (SA_AimingRocket(mode))
    {
        unsigned frame = *(volatile unsigned*)SA_FRAME_COUNT;
        if (frame != gLastFrame) { gLastFrame = frame; SA_UpdateZoom(); }
        float out = fov * (gCur / 70.0f);
        if (gLogCount < 6 && fabsf(gCur - 70.0f) > 0.5f)
        { ++gLogCount; Log("Rocket launcher zoom: mode=%d, FOV %.1f -> %.1f", mode, fov, out); }
        fov = out;
        gAimFov = gScaleSens ? gCur : 70.0f;
    }
    else
    {
        gTarget = gCur = gMaxFov;
        gLastFrame = 0xFFFFFFFFu;
        gAimFov = 70.0f;
    }
    ((SetFOV_t)SA_FN_SETFOV)(fov);
}

static uintptr_t CallTarget(uintptr_t site)
{
    uint8_t* c = (uint8_t*)site;
    if (c[0] != 0xE8) return 0;
    return site + 5 + (uintptr_t)(intptr_t)(*(int32_t*)(c + 1));
}
static bool WriteCall(uintptr_t at, void* target)
{
    uint8_t b[5];
    b[0] = 0xE8;
    *(int32_t*)(b + 1) = (int32_t)((uintptr_t)target - (at + 5));
    return WriteBytes(at, b, 5);
}
static bool SA_PatchSens(uintptr_t at)
{
    if (memcmp((void*)at, SA_SENS_ORIG, sizeof SA_SENS_ORIG) != 0) return false;
    return PatchSensToVar(at);
}

static bool InstallSA()
{
    ClampLimits(15.0f, 70.0f);
    if (CallTarget(SA_SITE_SETFOV) != SA_FN_SETFOV)
    {
        Log("SA: call at 0x%08X does not point to CDraw::SetFOV (target=0x%08X). Mod disabled.",
            (unsigned)SA_SITE_SETFOV, (unsigned)CallTarget(SA_SITE_SETFOV));
        return false;
    }
    bool ok = WriteCall(SA_SITE_SETFOV, (void*)&SA_HookSetFOV);
    bool s1 = false, s2 = false;
    if (gScaleSens)
    {
        s1 = SA_PatchSens(SA_SITE_SENS_PAD);
        s2 = SA_PatchSens(SA_SITE_SENS_MOUSE);
        if (!s1 || !s2) Log("SA sensitivity: unexpected bytes (analog=%d, mouse=%d).", (int)s1, (int)s2);
    }
    Log("SA: wheel=%d buttons=%d heatseeker=%d. Patch: %s. Sensitivity: %s",
        (int)gWheel, (int)gPadKeys, (int)gHeatSeek, ok ? "OK" : "FAILED",
        gScaleSens ? ((s1 && s2) ? "OK" : "PARTIAL/FAILED") : "disabled");
    return ok;
}

// =====================================================================================================
BOOL APIENTRY DllMain(HMODULE hm, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hm);
    hSelf = hm;

    GetModuleFileNameA(hm, gIni, MAX_PATH);
    char* dot = strrchr(gIni, '.');
    if (dot) strcpy(dot, ".ini"); else strcat(gIni, ".ini");

    gLog = GetPrivateProfileIntA("RocketZoom", "Log", 0, gIni) != 0;
    if (!GetPrivateProfileIntA("RocketZoom", "Enable", 1, gIni)) { Log("Disabled in .ini"); return TRUE; }

    gMinFovCfg = ReadIniFloat("MinFov", 0.0f);
    gMaxFovCfg = ReadIniFloat("MaxFov", 0.0f);
    gSpeed     = ReadIniFloat("Speed", 1.0f);
    if (gSpeed < 0.1f) gSpeed = 0.1f;
    if (gSpeed > 10.0f) gSpeed = 10.0f;
    gWheel     = GetPrivateProfileIntA("RocketZoom", "MouseWheel", 1, gIni) != 0;
    gPadKeys   = GetPrivateProfileIntA("RocketZoom", "PadButtons", 1, gIni) != 0;
    gHeatSeek  = GetPrivateProfileIntA("RocketZoom", "HeatSeeker", 1, gIni) != 0;
    gScaleSens = GetPrivateProfileIntA("RocketZoom", "ScaleSensitivity", 1, gIni) != 0;

    // Identify the game using byte signatures (safe reading: a game's addresses do not exist in the other games' executables)
    if (MatchOld(INFO_III))      gGame = G_III;
    else if (MatchOld(INFO_VC))  gGame = G_VC;
    else if (Readable(SA_VERSION_ADDR, 4) && *(uint32_t*)SA_VERSION_ADDR == SA_VERSION_OK) gGame = G_SA;

    if (gGame == G_NONE)
    {
        Log("Game/version not recognized (supported: GTA III 1.0, Vice City 1.0, SA 1.0 US). Mod disabled.");
        return TRUE;
    }

    bool ok;
    if (gGame == G_III)      ok = InstallOld(INFO_III);
    else if (gGame == G_VC)  ok = InstallOld(INFO_VC);
    else                     ok = InstallSA();

    Log("RocketZoom v3.1 - %s: patch %s. FOV %.2f..%.2f, speed %.2f, zoom sensitivity %s",
        gGame == G_SA ? "GTA SA 1.0 US" : gOld->name, ok ? "OK" : "FAILED", gMinFov, gMaxFov, gSpeed,
        gScaleSens ? "enabled" : "disabled");
    return TRUE;
}