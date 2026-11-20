// PN2 Coop proxy DLL (dinput8.dll replacement next to PokerNight2.exe).
//
// Phase 1a: load real dinput8, forward DirectInput8Create. Verified.
// Phase 1b: 5-byte JMP trampoline on lua_ChorePlay (VA 0x702170). Verified.
// Phase 1c: lua_tolstring read from inside our hook. Verified ("ui_pressStartIntro.chore").
// Phase 2:  additional 5-byte JMP trampoline on GameLogicSetAgent (VA 0x6CCA70).
//           Post-call, use the Lua 5.1 C API primitives Jackbox identified
//           (pushstring/pushcclosure/rawset) to overwrite PN2's global Print
//           with our own C function. Every subsequent Lua Print(...) call
//           lands in coop_log.txt â€” direct evidence the install seam works
//           and we can register coop C bindings from here on.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../build/coop_exchange_lua.h"

#pragma comment(lib, "ws2_32.lib")

#define PN2_STRINGIFY_VALUE(value) #value
#define PN2_STRINGIFY(value) PN2_STRINGIFY_VALUE(value)
#ifdef PN2_SPECTATING_ONLY
#define PN2_LOBBY_PROTOCOL 14
#else
#define PN2_LOBBY_PROTOCOL 17
#endif
#define PN2_LOBBY_PROTOCOL_TEXT PN2_STRINGIFY(PN2_LOBBY_PROTOCOL)

// ---- Diagnostic logging ---------------------------------------------------

static CRITICAL_SECTION g_log_lock;
static BOOL g_log_ready = FALSE;
static wchar_t g_log_path[MAX_PATH];
static wchar_t g_config_path[MAX_PATH];

static BOOL coop_init_paths(HMODULE proxy) {
    wchar_t directory[MAX_PATH];
    DWORD length = GetModuleFileNameW(proxy, directory, ARRAYSIZE(directory));
    if (!length || length >= ARRAYSIZE(directory)) return FALSE;
    wchar_t *filename = wcsrchr(directory, L'\\');
    if (!filename) return FALSE;
    filename[1] = L'\0';
    return _snwprintf_s(g_log_path, ARRAYSIZE(g_log_path), _TRUNCATE, L"%lscoop_log.txt", directory) > 0 &&
           _snwprintf_s(g_config_path, ARRAYSIZE(g_config_path), _TRUNCATE, L"%lscoop_config.txt", directory) > 0;
}

static void coop_log(const char *fmt, ...) {
    if (!g_log_ready) return;
    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "%02u:%02u:%02u.%03u ",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    n += _vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;
    EnterCriticalSection(&g_log_lock);
    HANDLE h = CreateFileW(g_log_path,
                           FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)n, &w, NULL);
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_log_lock);
}

// ---- dinput8 forwarder ----------------------------------------------------

typedef HRESULT(WINAPI *PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
static HMODULE g_real_dinput8 = NULL;
static PFN_DirectInput8Create g_real_DirectInput8Create = NULL;
static volatile LONG g_retry_coop;
static BOOL try_start_coop(BOOL retry);

static void load_real_dinput8(void) {
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 20) return;
    strcat_s(path, MAX_PATH, "\\dinput8.dll");
    g_real_dinput8 = LoadLibraryA(path);
    if (g_real_dinput8) {
        g_real_DirectInput8Create =
            (PFN_DirectInput8Create)GetProcAddress(g_real_dinput8, "DirectInput8Create");
    }
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst,
                                                       DWORD dwVersion,
                                                       REFIID riidltf,
                                                       LPVOID *ppvOut,
                                                       void *punkOuter) {
    if (!g_real_DirectInput8Create) return E_FAIL;
    if (InterlockedExchange(&g_retry_coop, 0)) try_start_coop(TRUE);
    return g_real_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

// ---- PN2 preferred-image facts + Lua 5.1 C API map ------------------------

#define PN2_PREFERRED_BASE      0x00400000u

// Telltale Lua C functions (hook targets).
#define VA_GAME_SET_AGENT       0x006CCA70u  // Jackbox-identified
#define VA_LUA_CHOREPLAY        0x00702170u  // Jackbox-identified, verified
#define VA_SAVE_PREFERENCES     0x00407F80u  // Shared native and Lua serializer.
#define VA_BACKGROUND_RUN       0x004557D0u

// Lua 5.1 C API (nine Jackbox-identified at LuaBridge.cpp:427-428, plus two
// we decompiled in session 2026-11-13: getfield at 0x7F4E70 (3-arg, builds
// Lua string, calls luaV_gettable, pushes +8), setfield at 0x7F5090 (3-arg,
// builds Lua string, pops top)).
#define VA_LUA_GETTOP           0x007F4550u
#define VA_LUA_SETTOP           0x007F4570u
#define VA_LUA_CHECKSTACK       0x007F4410u
#define VA_LUA_PUSHINTEGER      0x007F4BF0u
#define VA_LUA_PUSHSTRING       0x007F4C60u
#define VA_LUA_PUSHCCLOSURE     0x007F4D30u
#define VA_LUA_RAWSET           0x007F50F0u
#define VA_LUA_PCALL            0x007F5370u
#define VA_LUA_TOLSTRING        0x007F49E0u
#define VA_LUA_GETFIELD         0x007F4E70u
#define VA_LUA_SETFIELD         0x007F5090u

// Lua 5.1 pseudo-indices (compile-time constants, same across all 5.1 binaries).
#define LUA_REGISTRYINDEX       (-10000)
#define LUA_GLOBALSINDEX        (-10002)

typedef int   (__cdecl *PFN_lua_CFunction)(void *L);
typedef int   (__cdecl *PFN_lua_gettop)   (void *L);
typedef void  (__cdecl *PFN_lua_settop)   (void *L, int idx);
typedef int   (__cdecl *PFN_lua_checkstack)(void *L, int sz);
typedef void  (__cdecl *PFN_lua_pushinteger)(void *L, long i);
typedef void  (__cdecl *PFN_lua_pushstring)(void *L, const char *s);
typedef void  (__cdecl *PFN_lua_pushcclosure)(void *L, PFN_lua_CFunction fn, int n);
typedef void  (__cdecl *PFN_lua_rawset)    (void *L, int idx);
typedef int   (__cdecl *PFN_lua_pcall)     (void *L, int nargs, int nresults, int errfunc);
typedef const char *(__cdecl *PFN_lua_tolstring)(void *L, int idx, size_t *len);
typedef void  (__cdecl *PFN_lua_getfield) (void *L, int idx, const char *k);
typedef void  (__cdecl *PFN_lua_setfield) (void *L, int idx, const char *k);

static uintptr_t           g_base_delta = 0;
static PFN_lua_gettop      p_lua_gettop      = NULL;
static PFN_lua_settop      p_lua_settop      = NULL;
static PFN_lua_checkstack  p_lua_checkstack  = NULL;
static PFN_lua_pushinteger p_lua_pushinteger = NULL;
static PFN_lua_pushstring  p_lua_pushstring  = NULL;
static PFN_lua_pushcclosure p_lua_pushcclosure = NULL;
static PFN_lua_rawset      p_lua_rawset      = NULL;
static PFN_lua_pcall       p_lua_pcall       = NULL;
static PFN_lua_tolstring   p_lua_tolstring   = NULL;
static PFN_lua_getfield    p_lua_getfield    = NULL;
static PFN_lua_setfield    p_lua_setfield    = NULL;

static void *resolve_va(uintptr_t va) {
    return (void *)(va + g_base_delta);
}

static const uint8_t g_save_prologue[] = {0x55, 0x8b, 0xec, 0x64, 0xa1, 0, 0, 0, 0};
static const uint8_t g_background_prologue[] = {0x56, 0x8b, 0xf1, 0xff, 0x15};

static BOOL matches_game_code(HMODULE exe, uintptr_t va, const void *bytes, size_t length) {
    const uint8_t *address = resolve_va(va);
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery(address, &region, sizeof(region)) ||
        region.AllocationBase != exe || region.Type != MEM_IMAGE || region.State != MEM_COMMIT ||
        (region.Protect & PAGE_GUARD)) return FALSE;
    DWORD protection = region.Protect & 0xff;
    if (protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY) return FALSE;
    size_t offset = (uintptr_t)address - (uintptr_t)region.BaseAddress;
    return offset <= region.RegionSize && length <= region.RegionSize - offset &&
           memcmp(address, bytes, length) == 0;
}

static BOOL validate_game_code(HMODULE exe, BOOL lobby_mode) {
    // Both supplied installations expose these entry points once the game code
    // is ready. Use instruction bytes that contain no image relocations.
    static const struct {
        uintptr_t va;
        uint8_t bytes[8];
    } required[] = {
        {VA_LUA_CHECKSTACK,   {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x0c, 0x53, 0x56}},
        {VA_LUA_GETTOP,       {0x55, 0x8b, 0xec, 0x8b, 0x4d, 0x08, 0x8b, 0x41}},
        {VA_LUA_SETTOP,       {0x55, 0x8b, 0xec, 0x8b, 0x4d, 0x0c, 0x8b, 0x45}},
        {VA_LUA_TOLSTRING,    {0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x08, 0x57}},
        {VA_LUA_PUSHINTEGER,  {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0xdb, 0x45}},
        {VA_LUA_PUSHSTRING,   {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x0c, 0x85, 0xc0}},
        {VA_LUA_PUSHCCLOSURE, {0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x08, 0x8b}},
        {VA_LUA_GETFIELD,     {0x55, 0x8b, 0xec, 0x8b, 0x4d, 0x0c, 0x83, 0xec}},
        {VA_LUA_SETFIELD,     {0x55, 0x8b, 0xec, 0x8b, 0x4d, 0x0c, 0x83, 0xec}},
        {VA_LUA_RAWSET,       {0x55, 0x8b, 0xec, 0x8b, 0x4d, 0x0c, 0x53, 0x56}},
        {VA_LUA_PCALL,        {0x55, 0x8b, 0xec, 0x8b, 0x4d, 0x14, 0x83, 0xec}},
    };
    for (size_t i = 0; i < ARRAYSIZE(required); ++i) {
        if (!matches_game_code(exe, required[i].va, required[i].bytes, sizeof(required[i].bytes))) {
            coop_log("install: native code unavailable at VA %08X", (unsigned)required[i].va);
            return FALSE;
        }
    }
    static const uint8_t hook_prologue[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
    if (!matches_game_code(exe, VA_GAME_SET_AGENT, hook_prologue, sizeof(hook_prologue)) ||
        !matches_game_code(exe, VA_LUA_CHOREPLAY, hook_prologue, sizeof(hook_prologue))) {
        coop_log("install: SetAgent or ChorePlay hook signature unavailable");
        return FALSE;
    }
    if (lobby_mode && !matches_game_code(exe, VA_SAVE_PREFERENCES,
                                         g_save_prologue, sizeof(g_save_prologue))) {
        coop_log("install: native profile write protection unavailable");
        return FALSE;
    }
    return TRUE;
}

// ---- 5-byte JMP trampoline infrastructure --------------------------------

// Install a 5-byte JMP rel32 at `target` â†’ `hook`, copying the original 5
// bytes into `trampoline` followed by a JMP back to target+5. Original is
// callable by invoking the trampoline. Returns 1 on success.
static int install_trampoline_span(void *target, void *hook, void *trampoline,
                                   size_t span) {
    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    uint8_t *src = (uint8_t *)target;
    uint8_t *tr  = (uint8_t *)trampoline;
    memcpy(tr, src, span);
    tr[span] = 0xE9;
    int32_t rel_back = (int32_t)((intptr_t)target + span - ((intptr_t)tr + span + 5));
    memcpy(tr + span + 1, &rel_back, 4);
    src[0] = 0xE9;
    int32_t rel_hook = (int32_t)((intptr_t)hook - ((intptr_t)target + 5));
    memcpy(src + 1, &rel_hook, 4);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    return 1;
}

static int install_trampoline(void *target, void *hook, void *trampoline) {
    return install_trampoline_span(target, hook, trampoline, 5);
}

typedef unsigned char (__cdecl *PFN_save_preferences)(int);
static PFN_save_preferences g_orig_save_preferences;
static volatile LONG g_session_preferences_dirty;

static unsigned char __cdecl hook_save_preferences(int mode) {
    if (InterlockedCompareExchange(&g_session_preferences_dirty, 0, 0)) {
        coop_log("COOP_SESSION native_preference_write_blocked");
        return 0;
    }
    return g_orig_save_preferences(mode);
}

static int __cdecl coop_protect_preferences(void *L) {
    const char *mode = p_lua_tolstring(L, 1, NULL);
    if (mode && (strcmp(mode, "1") == 0 || strcmp(mode, "0") == 0)) {
        InterlockedExchange(&g_session_preferences_dirty, mode[0] == '1');
    }
    return 0;
}

// ---- Phase 1 hook: lua_ChorePlay ------------------------------------------

static void *g_choreplay_addr        = NULL;
static void *g_choreplay_trampoline  = NULL;
static PFN_lua_CFunction g_orig_choreplay = NULL;
static volatile LONG g_choreplay_logged = 0;

// Forward-declared helper so hook_lua_ChorePlay can broadcast without
// touching the net-code globals (which are defined further down). The real
// implementation lives next to COOP_Broadcast below.
static void coop_broadcast_from_c(const char *msg);
// Gameplay gate â€” defined with the broadcast helper further down.
static volatile LONG g_gameplay_active;
// Experimental exchange replay is explicitly opt-in in each process.
static int g_test_mode = 0;

static int __cdecl hook_lua_ChorePlay(void *L) {
    const char *name = p_lua_tolstring ? p_lua_tolstring(L, 1, NULL) : NULL;
    if (name && name[0]) {
        coop_log("hook: ChorePlay L=%p chore=\"%s\"", L, name);
        // This binding does not intercept native dialog playback. In the
        // exchange experiment, local action chores stay local to avoid duplicates.
        if (!g_test_mode && g_gameplay_active && strncmp(name, "ui_", 3) != 0) {
            char buf[256];
            int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "CHORE name=%s", name);
            if (n > 0) coop_broadcast_from_c(buf);
        }
    }
    if (InterlockedExchange(&g_choreplay_logged, 1) == 0 && name) {
        coop_log("hook: ChorePlay FIRST call, chore=\"%s\"", name);
    }
    return g_orig_choreplay(L);
}

// ---- Phase 2 hook: GameLogicSetAgent --------------------------------------
// On every SetAgent call, overwrite _G.Print with coop_print_hook. rawset is
// idempotent; repeated installs are harmless. We use the state's own API
// pointers so this works regardless of ASLR or which lua_State is active.

static void *g_setagent_addr       = NULL;
static void *g_setagent_trampoline = NULL;
static PFN_lua_CFunction g_orig_setagent = NULL;
static volatile LONG g_setagent_install_count = 0;

// Our Print replacement. Signature is a Lua C function; reads any number of
// string args and logs them. Returns 0 â€” no Lua return values.
static int __cdecl coop_print_hook(void *L) {
    int top = p_lua_gettop ? p_lua_gettop(L) : 0;
    char acc[1024];
    int acc_n = 0;
    for (int i = 1; i <= top; ++i) {
        const char *s = p_lua_tolstring ? p_lua_tolstring(L, i, NULL) : NULL;
        int w = _snprintf_s(acc + acc_n, sizeof(acc) - acc_n, _TRUNCATE,
                            "%s%s", (i > 1 ? "\t" : ""), s ? s : "<non-string>");
        if (w < 0) break;
        acc_n += w;
        if (acc_n >= (int)sizeof(acc) - 4) break;
    }
    coop_log("lua Print(%d args): %s", top, acc_n ? acc : "<empty>");
    return 0;
}

// Phase 4b â€” chain-to-original via a Lua wrapper.
//
// The C-function hook hit a Lua 5.1 wall: `AIPlayer:EvaluateAndAct` calls
// Yield() internally to wait for the AI-thinking chore, and 5.1 can't yield
// across a C-call boundary. Fix: install the wrapper as a LUA function (no
// C boundary), compiled at runtime from the string below via Lua's own
// loadstring. That means Lua-to-Lua calls both ways, Yield works freely,
// and SetDialogLogic / banter are fully restored.
//
// The wrapper also builds in the Phase 4c hook point: on each AI turn it
// asks COOP_SeatClaimed(self) whether a human has taken this seat; if yes,
// it returns COOP_NetworkAction(self, state) instead of the original. Both
// COOP_* functions are C globals we register here; for Phase 4b, SeatClaimed
// is a no-op returning nil (falsy) so every seat falls through to the AI.

// Phase 4c/d â€” the Lua wrapper does all the waiting *and* translates our
// (action, amount) pair into a "result" number the original bet function
// would have returned. Then it calls the original EvaluateAndAct with a
// hijacked self.ai.BetPreflop / BetPostflop so every downstream side
// effect (Logic.iLastBetAmount, RemapChipsBasedOnBetAmount, SetDialogLogic,
// all banter triggers) fires with the right value. HandleBetResult picks
// the final action string from the amount â€” this is why "CALL" needs the
// true callValue, not just a sentinel.
//
// Phase 4d additions:
//   - Wrap GameTable:InitRound to broadcast HAND_START (seed, dealer idx)
//   - Broadcast ACTION_RESULT after every EvaluateAndAct (both paths)
static const char COOP_WRAPPER_SRC[] =
"local exchangeTest = COOP_TestMode and COOP_TestMode()\n"
"local function emit(msg)\n"
"  if COOP_Broadcast then COOP_Broadcast(msg) end\n"
"end\n"
"local function actionWireLabel(action, label)\n"
"  -- allIn describes the presentation, not whether betting CALLed or RAISEd.\n"
"  -- Keep the existing wire vocabulary while preserving the engine branch.\n"
"  if exchangeTest and action == 1 and label == kAction_AllIn then return kAction_Call end\n"
"  return label\n"
"end\n"
"local function waitAction(name)\n"
"  local waited, expectedEpoch = 0, nil\n"
"  local function checkSession()\n"
"    if not exchangeTest then return end\n"
"    local ready, epoch\n"
"    if COOP_ExchangeSync then ready, epoch = COOP_ExchangeSync() end\n"
"    if not ready or type(epoch) ~= 'number' or not (Logic and Logic.bInGame == true)\n"
"       or (expectedEpoch and epoch ~= expectedEpoch) then\n"
"      if COOP_ExchangeAbort then COOP_ExchangeAbort() end\n"
"      error('Co-op action wait cancelled: session changed')\n"
"    end\n"
"    expectedEpoch = epoch\n"
"  end\n"
"  checkSession()\n"
"  while not COOP_HasPendingAction(name) do\n"
"    Yield(); waited = waited + 1\n"
"    checkSession()\n"
"    if waited > 3000 and not exchangeTest then break end\n"
"  end\n"
"  return COOP_TakePendingAction(name)\n"
"end\n"
"if AIPlayer and AIPlayer.EvaluateAndAct and not AIPlayer.COOP_Wrapped then\n"
"  local orig = AIPlayer.EvaluateAndAct\n"
"  AIPlayer.COOP_Wrapped = true\n"
    // Effect-capture style: host runs AI naturally and broadcasts its
    // decision; guest waits and forces the same decision through the engine's
    // own debug_force_action / BetPreflop hijack, after neutralizing the
    // random branches in PostAIBetEvaluation and the min-raise clamp so the
    // engine won't override the amount with a different random choice.
"  AIPlayer.EvaluateAndAct = function(self, state)\n"
"    local name = self.GetName and self:GetName() or \"<unknown>\"\n"
"    if not (COOP_IsGuest and COOP_IsGuest()) then\n"
"      local chipsBefore = exchangeTest and self:GetChips()\n"
"      local a, s = orig(self, state)\n"
"      local amt = Logic and Logic.iLastBetAmount or 0\n"
"      -- HandleBetResult may promote a proposed re-raise to the whole stack.\n"
"      -- iLastBetAmount still holds that proposal; capture the applied effect.\n"
"      if chipsBefore and (a == 1 or a == 2) then amt = chipsBefore - self:GetChips() end\n"
"      Print('COOP_TRACE ai_host seat=' .. tostring(name) ..\n"
"            ' action=' .. tostring(a) .. ' str=' .. tostring(s) ..\n"
"            ' amount=' .. tostring(amt) ..\n"
"            ' chips_after=' .. tostring(self:GetChips()) ..\n"
"            ' chipsInPot=' .. tostring(self:GetChipsInPot()))\n"
"      emit(string.format('ACTION_RESULT seat=%s action=%s amount=%d',\n"
"                          tostring(name):gsub(' ', '_'), tostring(actionWireLabel(a,s)), tonumber(amt) or 0))\n"
"      return a, s\n"
"    end\n"
"    local action, actionStr, amount = waitAction(name)\n"
"    local gameTable = self.parentTable\n"
"    local playerIdx = gameTable:GetPlayerIndex(name)\n"
"    -- Save state we're about to override so we can restore after orig.\n"
"    local saveForceAction = Logic.debug_force_action\n"
"    local saveForceTarget = Logic.debug_target\n"
"    local savePre = self.ai.BetPreflop\n"
"    local savePost = self.ai.BetPostflop\n"
"    local saveOnCheckRaise = self.onCheckRaise\n"
"    local saveOnBluff = self.onBluff\n"
"    local saveDifficulty = gameTable.difficulty\n"
"    local saveLastRaiseAmount = Logic.iLastRaiseAmount\n"
"    -- Neutralize PostAIBetEvaluation's random overrides on guest: Medium\n"
"    -- difficulty gates EASY MODE FOLD/BLUFF; onCheckRaise and onBluff gate\n"
"    -- the checkraise and bluff-response branches.\n"
"    self.onCheckRaise = false\n"
"    self.onBluff = false\n"
"    if gameTable.difficulty == kDifficulty_Medium then\n"
"      gameTable.difficulty = kDifficulty_Hard\n"
"    end\n"
"    -- Zero iLastRaiseAmount so the (result < minRaise) clamp becomes a no-op\n"
"    -- for our forced raise target.\n"
"    Logic.iLastRaiseAmount = 0\n"
"    -- Raise path: PostAIBetEvaluation at aiplayer.src.lua:202 applies a\n"
"    -- RandomInt jitter to any raise amount before the near-all-in clamp\n"
"    -- fires. We can't patch that local function, so bypass orig entirely\n"
"    -- for raises and replicate the essential side effects by hand.\n"
"    if action == 2 then\n"
"      Player.EvaluateAndAct(self)\n"
"      local callValue = gameTable:GetCallValue() - self:GetChipsInPot()\n"
"      local raisePortion = (amount or 0) - callValue\n"
"      if raisePortion > 0 then self:Raise(raisePortion) end\n"
"      Logic.iLastBetAmount = amount\n"
"      self.raisedThisRound = true\n"
"      if state.name == 'ModePreFlop' then self.raisedPreFlop = true end\n"
"      local astr = kAction_Raise\n"
"      if self:GetChips() == 0 then astr = kAction_AllIn\n"
"      elseif callValue == 0 and state.name ~= 'ModePreFlop' then astr = kAction_Bet\n"
"      end\n"
"      Logic['LastAction_Player' .. self.index] = astr\n"
"      self.playerAction = 2\n"
"      Logic.debug_force_action = saveForceAction\n"
"      Logic.debug_target = saveForceTarget\n"
"      self.ai.BetPreflop = savePre\n"
"      self.ai.BetPostflop = savePost\n"
"      self.onCheckRaise = saveOnCheckRaise\n"
"      self.onBluff = saveOnBluff\n"
"      gameTable.difficulty = saveDifficulty\n"
"      Logic.iLastRaiseAmount = saveLastRaiseAmount\n"
"      Print('COOP_TRACE ai_guest seat=' .. tostring(name) ..\n"
"            ' host_action=2 host_amount=' .. tostring(amount) ..\n"
"            ' bypassed_orig result_str=' .. tostring(astr) ..\n"
"            ' chips_after=' .. tostring(self:GetChips()) ..\n"
"            ' chipsInPot=' .. tostring(self:GetChipsInPot()))\n"
"      return 2, astr\n"
"    end\n"
"    if action == 3 then\n"
"      Logic.debug_force_action = 3\n"
"      Logic.debug_target = playerIdx\n"
"    elseif action == 1 or action == 4 then\n"
"      Logic.debug_force_action = 1\n"
"      Logic.debug_target = playerIdx\n"
"    end\n"
"    local okA, okS = orig(self, state)\n"
"    -- Guest AI diagnostic: dump host broadcast vs what engine actually did.\n"
"    Print('COOP_TRACE ai_guest seat=' .. tostring(name) ..\n"
"          ' host_action=' .. tostring(action) ..\n"
"          ' host_amount=' .. tostring(amount) ..\n"
"          ' result_action=' .. tostring(okA) ..\n"
"          ' result_str=' .. tostring(okS) ..\n"
"          ' iLastBetAmount=' .. tostring(Logic.iLastBetAmount) ..\n"
"          ' chips_after=' .. tostring(self:GetChips()) ..\n"
"          ' chipsInPot=' .. tostring(self:GetChipsInPot()))\n"
"    Logic.debug_force_action = saveForceAction\n"
"    Logic.debug_target = saveForceTarget\n"
"    self.ai.BetPreflop = savePre\n"
"    self.ai.BetPostflop = savePost\n"
"    self.onCheckRaise = saveOnCheckRaise\n"
"    self.onBluff = saveOnBluff\n"
"    gameTable.difficulty = saveDifficulty\n"
"    Logic.iLastRaiseAmount = saveLastRaiseAmount\n"
"    return okA, okS\n"
"  end\n"
"end\n"
"-- InitRound wrapper installed below (merged host+guest) at end of chunk.\n"
"local rankMap = {A=14, K=13, Q=12, J=11}\n"
"for i=2,10 do rankMap[tostring(i)] = i end\n"
"local suitMap = {S=1, H=2, D=3, C=4}\n"
"local function parseCard(s)\n"
"  local rs, cs\n"
"  if #s == 3 then rs, cs = s:sub(1,2), s:sub(3,3)\n"
"  else rs, cs = s:sub(1,1), s:sub(2,2) end\n"
"  return rankMap[rs], suitMap[cs]\n"
"end\n"
"if Deck and Deck.Shuffle and not Deck.COOP_Wrapped then\n"
"  local origShuffle = Deck.Shuffle\n"
"  Deck.COOP_Wrapped = true\n"
"  Deck.Shuffle = function(self)\n"
"    local isGuest = COOP_IsGuest and COOP_IsGuest()\n"
"    if exchangeTest and ((isGuest and not COOP_WaitForHandDeck) or\n"
"        (not isGuest and not COOP_PublishHandDeck)) then\n"
"      -- Scene/table construction is local setup, never a network hand.\n"
"      return origShuffle(self)\n"
"    end\n"
"    if isGuest then\n"
"      local deck = COOP_PollDeck and COOP_PollDeck() or nil\n"
"      if exchangeTest then\n"
"        local wasReady = false\n"
"        while not deck do\n"
"          local active = COOP_ExchangeSync and COOP_ExchangeSync()\n"
"          if not (Logic and Logic.bInGame == true) or (wasReady and not active) then\n"
"            COOP_ExchangeAbort(); error('Co-op hand deck cancelled')\n"
"          end\n"
"          wasReady = active or wasReady\n"
"          Yield()\n"
"          deck = COOP_PollDeck and COOP_PollDeck() or nil\n"
"        end\n"
"        -- Validate before mutating the local deck. No independent fallback.\n"
"        if not (Logic and Logic.bInGame == true) then\n"
"          COOP_ExchangeAbort(); error('Co-op hand deck cancelled')\n"
"        end\n"
"        local count, seen = 0, {}\n"
"        for card in deck:gmatch('[^,]+') do\n"
"          local r, u = parseCard(card)\n"
"          local valid = card:match('^[2-9AJQK][SHDC]$') or card:match('^10[SHDC]$')\n"
"          if not valid or not r or not u or seen[card] then\n"
"            COOP_ExchangeAbort(); error('Invalid host hand deck')\n"
"          end\n"
"          seen[card] = true; count = count + 1\n"
"        end\n"
"        if count ~= 52 then COOP_ExchangeAbort(); error('Incomplete host hand deck') end\n"
"        COOP_WaitForHandDeck = false\n"
"      end\n"
"      if deck then\n"
"        self.burnCards = {}\n"
"        self:Clear()\n"
"        for s in deck:gmatch('[^,]+') do\n"
"          local r, u = parseCard(s)\n"
"          if r and u then self:Add(Card(r, u)) end\n"
"        end\n"
"        if exchangeTest then Print('COOP_TRACE hand_deck_guest deck=' .. deck) end\n"
"        return\n"
"      end\n"
"    end\n"
"    local ret = origShuffle(self)\n"
"    local parts = {}\n"
"    for i, c in self:Cards() do parts[i] = c:ToString() end\n"
"    if exchangeTest and Logic and Logic.bInGame then\n"
"      Print('COOP_TRACE hand_deck_host deck=' .. table.concat(parts, ','))\n"
"    end\n"
"    emit('SHUFFLE deck=' .. table.concat(parts, ','))\n"
"    if exchangeTest then COOP_PublishHandDeck = false end\n"
"    return ret\n"
"  end\n"
"end\n"
"if HumanPlayer and HumanPlayer.EvaluateAndAct and not HumanPlayer.COOP_Wrapped then\n"
"  local origHE = HumanPlayer.EvaluateAndAct\n"
"  HumanPlayer.COOP_Wrapped = true\n"
"  HumanPlayer.EvaluateAndAct = function(self, state)\n"
"    local name = self.GetName and self:GetName() or 'The Player'\n"
"    if COOP_IsGuest and COOP_IsGuest() then\n"
"      local action, actionStr, amount = waitAction(name)\n"
"      Player.EvaluateAndAct(self)\n"
"      self.playerAction = action\n"
"      local astr = kAction_Fold\n"
"      if action == 1 or action == 4 then\n"
"        -- call or check: both go through Player:Call. Call() returns 0 when\n"
"        -- there's nothing to match (= check). Only fold (action==3) is handled\n"
"        -- in the else branch below; action==4 was previously falling through\n"
"        -- and being treated as fold, which removed Player from active list\n"
"        -- and triggered AI fold-taunt lines.\n"
"        local c = self:Call()\n"
"        Logic.iLastBetAmount = c\n"
"        if c == 0 then astr = kAction_Check\n"
"        elseif self:GetChips() == 0 then astr = kAction_AllIn\n"
"        else astr = kAction_Call end\n"
"      elseif action == 2 then\n"
"        self:Raise(amount)\n"
"        Logic.iLastBetAmount = amount\n"
"        astr = (self:GetChips() == 0 and kAction_AllIn) or kAction_Raise\n"
"      else\n"
"        astr = kAction_Fold\n"
"        local cur = GameObject:GetTable():GetActivePlayers()\n"
"        for i, p in ipairs(cur) do if p == self then table.remove(cur, i); break end end\n"
"      end\n"
"      return action, astr\n"
"    end\n"
"    local chipsBefore = exchangeTest and self:GetChips()\n"
"    local a, s = origHE(self, state)\n"
"    local amt = Logic and Logic.iLastBetAmount or 0\n"
"    if chipsBefore and (a == 1 or a == 2) then amt = chipsBefore - self:GetChips() end\n"
"    Print('COOP_TRACE human_host seat=' .. tostring(name) ..\n"
"          ' action=' .. tostring(a) .. ' str=' .. tostring(s) ..\n"
"          ' amount=' .. tostring(amt) ..\n"
"          ' chips_after=' .. tostring(self:GetChips()) ..\n"
"          ' chipsInPot=' .. tostring(self:GetChipsInPot()))\n"
"    emit(string.format('ACTION_RESULT seat=%s action=%s amount=%d',\n"
"                       tostring(name):gsub(' ', '_'), tostring(actionWireLabel(a,s)), tonumber(amt) or 0))\n"
"    return a, s\n"
"  end\n"
"end\n"
"-- Diagnostic + banter-root broadcast. OnNodeBegin fires for every dialog\n"
"-- node begin. On host, when a 'bBanterSequence' root fires, broadcast its\n"
"-- stringified nodeID so guest can replay the exact same sequence instead of\n"
"-- letting its own engine picker choose a different one.\n"
"if not exchangeTest and Dialogue and Dialogue.OnNodeBegin and not Dialogue.COOP_OnNodeBeginWrapped then\n"
"  local orig = Dialogue.OnNodeBegin\n"
"  Dialogue.COOP_OnNodeBeginWrapped = true\n"
"  Dialogue.OnNodeBegin = function(self, dlgName, nodeID, instanceID)\n"
"    Print('COOP_TRACE node_begin dlg=' .. tostring(dlgName) ..\n"
"          ' node=' .. tostring(nodeID))\n"
"    if not (COOP_IsGuest and COOP_IsGuest()) and DlgGetObjUserPropsValue\n"
"       and DlgGetObjUserPropsValue(dlgName, nodeID, 'bBanterSequence') then\n"
"      -- Extract the numeric portion of 'dlg_id: NNN' for the wire.\n"
"      local idStr = tostring(nodeID)\n"
"      local n = idStr:match('(%d+)')\n"
"      if n then\n"
"        emit('BANTER_ROOT dlg=' .. tostring(dlgName) .. ' node=' .. n)\n"
"      end\n"
"    end\n"
"    return orig(self, dlgName, nodeID, instanceID)\n"
"  end\n"
"end\n"
"-- Guest: wrap StartBanter. Instead of calling DlgRun(dialogFile, 'banter')\n"
"-- which lets the engine pick, consume host's broadcast and build banterNodes\n"
"-- from the specific nodeID. If the engine's APIs accept the stringified ID,\n"
"-- banter sequences match host exactly. If not, this call silently fails and\n"
"-- guest's banter stays silent for that round.\n"
"if not exchangeTest and Dialogue and Dialogue.StartBanter and not Dialogue.COOP_StartBanterWrapped then\n"
"  local orig = Dialogue.StartBanter\n"
"  Dialogue.COOP_StartBanterWrapped = true\n"
"  Dialogue.StartBanter = function(self, player)\n"
"    if not (COOP_IsGuest and COOP_IsGuest()) then\n"
"      return orig(self, player)\n"
"    end\n"
"    if Logic.bModeInstant or Logic.bPauseBanter or Logic.bBanterPlaying then return end\n"
"    -- Short wait for host's banter-root broadcast.\n"
"    local waited = 0\n"
"    local dlg, node\n"
"    while waited < 90 do\n"
"      if COOP_TakeBanterRoot then dlg, node = COOP_TakeBanterRoot() end\n"
"      if dlg and node then break end\n"
"      Yield(); waited = waited + 1\n"
"    end\n"
"    if not (dlg and node) then\n"
"      Print('COOP_TRACE banter_guest timeout_no_root')\n"
"      return\n"
"    end\n"
"    Print('COOP_TRACE banter_guest apply dlg=' .. tostring(dlg) .. ' node=' .. tostring(node))\n"
"    Logic.bBanterPlaying = true\n"
"    self.banterSequenceRootDlg = dlg\n"
"    self.banterSequenceRootID = node\n"
"    self.currentBanterIndex = 0\n"
"    self.banterSpeakers = {}\n"
"    -- Try the engine's expected tostring format: 'dlg_id: NNN'. If that\n"
"    -- fails, try the raw numeric string as fallback.\n"
"    local fullStr = 'dlg_id: ' .. node\n"
"    local ok, nodes = pcall(DlgCollectNodesOfType, dlg, fullStr, 'all', false)\n"
"    if not ok or not nodes then\n"
"      Print('COOP_TRACE banter_guest fullStr failed, trying raw')\n"
"      ok, nodes = pcall(DlgCollectNodesOfType, dlg, node, 'all', false)\n"
"    end\n"
"    if ok and nodes then\n"
"      self.banterNodes = nodes\n"
"      if DlgGetExchangeNodeSpeakerNames then\n"
"        for _, n in ipairs(nodes) do\n"
"          local sp = DlgGetExchangeNodeSpeakerNames(n.Dlog, n['Node ID'])\n"
"          if sp then\n"
"            for _, s in ipairs(sp) do\n"
"              if not table.find(self.banterSpeakers, s) then\n"
"                table.insert(self.banterSpeakers, s)\n"
"              end\n"
"            end\n"
"          end\n"
"        end\n"
"      end\n"
"      if Dialogue_KillLookatIdles then Dialogue_KillLookatIdles() end\n"
"      if Dialogue_BanterThread and ThreadStart then\n"
"        self.banterThread = ThreadStart(Dialogue_BanterThread)\n"
"      end\n"
"    else\n"
"      Print('COOP_TRACE banter_guest DlgCollectNodesOfType failed')\n"
"      Logic.bBanterPlaying = false\n"
"    end\n"
"  end\n"
"end\n"
"-- Diagnostic + guest-side force-fire: if host already broadcast a banter\n"
"-- root but guest's actionToBanterCountdown hasn't hit 0 yet, fire\n"
"-- StartBanter directly so the broadcast isn't ignored.\n"
"-- Silence per-action poker lines on guest. Host plays them audibly; guest\n"
"-- stays quiet on per-action responses so engine's variant divergence doesn't\n"
"-- cause the audio drift we were seeing. We still decrement\n"
"-- actionToBanterCountdown so StartBanter triggers on the same action-count\n"
"-- timeline as host. Positive per-line broadcast is the longer-term fix.\n"
"-- Phase 4f.5 step 2: chore replay. Host-side broadcast of ChorePlay lives\n"
"-- in the C trampoline (hook_lua_ChorePlay) so engine-internal C++ callers\n"
"-- are captured too. No Lua wrap of ChorePlayAndWait â€” earlier version of\n"
"-- that wrap was somehow preventing cs_intro_vacuumShoot.chore from firing\n"
"-- on guest, leaving the main menu scene visible under the poker overlay.\n"
"-- Guest side spawns a replay thread that drains the C queue.\n"
"if not exchangeTest and not _G.COOP_ChoreWrapped then\n"
"  _G.COOP_ChoreWrapped = true\n"
"  local origChorePlay = _G.ChorePlay\n"
"  -- Guest replay thread. Uses origChorePlay (the wrapped C trampoline) so\n"
"  -- calls do get logged, but there's no risk of recursive broadcast because\n"
"  -- the C trampoline only sends when !g_role_is_guest.\n"
"  -- Host side: keep the C broadcast gate in sync with Logic.bInGame.\n"
"  -- Prevents title-screen / menu-return chores from rippling to guest.\n"
"  if not (COOP_IsGuest and COOP_IsGuest()) and type(ThreadStart) == 'function' then\n"
"    ThreadStart(function()\n"
"      while true do\n"
"        if COOP_SetGameplayActive then\n"
"          COOP_SetGameplayActive(Logic.bInGame == true and '1' or '0')\n"
"        end\n"
"        Yield()\n"
"      end\n"
"    end)\n"
"  end\n"
"  if COOP_IsGuest and COOP_IsGuest() and type(ThreadStart) == 'function' then\n"
"    ThreadStart(function()\n"
"      while true do\n"
"        if Logic.bInGame == true and COOP_TakeChore then\n"
"          local chore = COOP_TakeChore()\n"
"          while chore do\n"
"            pcall(origChorePlay, chore)\n"
"            chore = COOP_TakeChore()\n"
"          end\n"
"        else\n"
"          -- Not in gameplay yet: drain the queue so it doesn't fill with\n"
"          -- stale chores from host's ongoing hand, but don't replay them.\n"
"          if COOP_TakeChore then\n"
"            while COOP_TakeChore() do end\n"
"          end\n"
"        end\n"
"        Yield()\n"
"      end\n"
"    end)\n"
"  end\n"
"end\n"
"-- Silence the engine's dialog interpreter entry points on guest during\n"
"-- gameplay. Only string-selector calls (e.g. DlgRun('banter.dlog','banter'),\n"
"-- DlgRunAndWait('brock.dlog','brock_fold'), DlgRunAndWait('dealer.dlog',\n"
"-- 'dealer_blinds')) are silenced â€” those are where the engine picks a\n"
"-- divergent variant. Specific-node calls (second arg is a userdata nodeID\n"
"-- from banterNodes) are left through so our banter sync keeps playing.\n"
"-- Gated by Logic.bInGame so menus are unaffected.\n"
"-- This REPLACES the per-method silencing of PokerAction/PlayPokerResponse\n"
"-- because everything those methods do eventually calls DlgRun or DlgRunAndWait.\n"
"-- PokerAction's countdown decrement still needs to happen; handled via the\n"
"-- UpdateBanter wrapper below.\n"
"-- Audited responses.dlog entries are Start -> Chore -> idle-property Logic\n"
"-- only, with no exchanges/branches/scripts. Keep original NewRound's\n"
"-- nonblocking restore thread, guards and 0.4s style/lookat timing intact.\n"
"local foldRestores = { ['Brock Fold To Idle']=true, ['Claptrap Fold To Idle']=true,\n"
"  ['Ash Fold To Idle']=true, ['Sam Fold To Idle']=true }\n"
"if not _G.COOP_DlgSilenced and (not exchangeTest or\n"
"    (type(DlgRun) == 'function' and type(DlgRunAndWait) == 'function')) then\n"
"  _G.COOP_DlgSilenced = true\n"
"  local names = { 'DlgRun', 'DlgRunAndWait' }\n"
"  for _, nm in ipairs(names) do\n"
"    local orig = _G[nm]\n"
"    if type(orig) == 'function' then\n"
"      _G[nm] = function(dlg, nodeArg, ...)\n"
"        if COOP_IsGuest and COOP_IsGuest()\n"
"           and Logic.bInGame == true\n"
"           and (exchangeTest or type(nodeArg) == 'string') then\n"
"          if exchangeTest and nm == 'DlgRun' and dlg == 'responses.dlog'\n"
"             and type(nodeArg) == 'string' and foldRestores[nodeArg] then\n"
"            Print('COOP_TRACE fold_restore selector=' .. nodeArg)\n"
"            return orig(dlg, nodeArg, ...)\n"
"          end\n"
"          -- selector: let host author; guest stays silent for this call.\n"
"          return nil\n"
"        end\n"
"        return orig(dlg, nodeArg, ...)\n"
"      end\n"
"    end\n"
"  end\n"
"end\n"
"-- PokerAction wrapper: on guest, we no longer need to silence the dialog\n"
"-- (DlgRun* above handles that) but we DO still need to decrement the banter\n"
"-- countdown so UpdateBanter triggers on the same action-count timeline as\n"
"-- host. Without this, actionToBanterCountdown never ticks on guest and the\n"
"-- UpdateBanter force-fire path (below) never gets the chance to run.\n"
"if Dialogue and Dialogue.PokerAction and not Dialogue.COOP_PokerActionWrapped then\n"
"  local orig = Dialogue.PokerAction\n"
"  Dialogue.COOP_PokerActionWrapped = true\n"
"  Dialogue.PokerAction = function(self, initiator, actionString)\n"
"    if COOP_IsGuest and COOP_IsGuest() then\n"
"      if not self.banterSequenceRootID and self.actionToBanterCountdown then\n"
"        self.actionToBanterCountdown = self.actionToBanterCountdown - 1\n"
"      end\n"
"      return\n"
"    end\n"
"    return orig(self, initiator, actionString)\n"
"  end\n"
"end\n"
"if not exchangeTest and Dialogue and Dialogue.UpdateBanter and not Dialogue.COOP_UpdateBanterWrapped then\n"
"  local orig = Dialogue.UpdateBanter\n"
"  Dialogue.COOP_UpdateBanterWrapped = true\n"
"  Dialogue.UpdateBanter = function(self, player)\n"
"    local pname = (player and player.GetName and player:GetName()) or 'nil'\n"
"    Print('COOP_TRACE update_banter player=' .. tostring(pname) ..\n"
"          ' countdown=' .. tostring(self.actionToBanterCountdown) ..\n"
"          ' rootID=' .. tostring(self.banterSequenceRootID))\n"
"    if COOP_IsGuest and COOP_IsGuest()\n"
"       and COOP_HasBanterRoot and COOP_HasBanterRoot() ~= 0\n"
"       and not self.banterSequenceRootID\n"
"       and not Logic.bBanterPlaying\n"
"       and not Logic.bPauseBanter\n"
"       and player and player.GetType and player:GetType() == 'AI' then\n"
"      Print('COOP_TRACE update_banter guest_force_fire')\n"
"      return self:StartBanter(player)\n"
"    end\n"
"    return orig(self, player)\n"
"  end\n"
"end\n"
"if GameTable and GameTable.InitRound and not GameTable.COOP_InitWrapped then\n"
"  local origInit = GameTable.InitRound\n"
"  GameTable.COOP_InitWrapped = true\n"
"  GameTable.InitRound = function(self, seedOverride, dealerOverride)\n"
"    if COOP_IsGuest and COOP_IsGuest() then\n"
"      local waited = 0\n"
"      local handPeerReady = exchangeTest and COOP_ExchangeSync and COOP_ExchangeSync()\n"
"      local initialHand = self.handNumber == 0\n"
"      local connectionGeneration\n"
"      local lastStartupReady\n"
"      local function checkHandStart()\n"
"        if not exchangeTest then return end\n"
"        local generation, connected, fault\n"
"        if COOP_ExchangeConnection then generation, connected, fault = COOP_ExchangeConnection() end\n"
"        local active = COOP_ExchangeSync and COOP_ExchangeSync()\n"
"        if not (Logic and Logic.bInGame == true) or type(generation) ~= 'number' or fault ~= 0\n"
"           or (connectionGeneration and (connected ~= 1 or generation ~= connectionGeneration))\n"
"           or (not initialHand and handPeerReady and not active) then\n"
"          COOP_ExchangeAbort(); error('Co-op hand start cancelled')\n"
"        end\n"
"        if connected == 1 then connectionGeneration = generation end\n"
"        if initialHand and active ~= lastStartupReady then\n"
"          Print('COOP_TRACE startup_wait connection=' .. tostring(generation) .. ' ready=' .. tostring(active))\n"
"          lastStartupReady = active\n"
"        end\n"
"        handPeerReady = active or handPeerReady\n"
"      end\n"
"      checkHandStart()\n"
"      while not (COOP_ConsumeHandStart and COOP_ConsumeHandStart()) do\n"
"        Yield(); waited = waited + 1\n"
"        if waited > 3000 and not exchangeTest then break end\n"
"        checkHandStart()\n"
"      end\n"
"      checkHandStart()\n"
"      local s = COOP_HandStartSeed and COOP_HandStartSeed() or 0\n"
"      local d = COOP_HandStartDealer and COOP_HandStartDealer() or 0\n"
"      if s and s > 0 then math.randomseed(s) end\n"
"      if d and d > 0 then dealerOverride = d end\n"
"      -- Force chip counts to host's snapshot. Self-healing: any chip drift\n"
"      -- from the previous hand resets to zero here.\n"
"      if COOP_HandStartChips then\n"
"        local c1, c2, c3, c4, c5 = COOP_HandStartChips()\n"
"        local guestChips = { c1, c2, c3, c4, c5 }\n"
"        local players = self:GetPlayers()\n"
"        for i = 1, 5 do\n"
"          if players[i] and players[i].SetChips and guestChips[i] and guestChips[i] > 0 then\n"
"            players[i]:SetChips(guestChips[i])\n"
"          end\n"
"        end\n"
"      end\n"
"      -- Guest hand-start trace: dump per-seat chip state right before orig.\n"
"      local players = self:GetPlayers()\n"
"      local parts = {}\n"
"      for i = 1, 5 do\n"
"        local p = players[i]\n"
"        parts[i] = tostring((p and p.GetName and p:GetName()) or '?') .. '=' ..\n"
"                   tostring((p and p.GetChips and p:GetChips()) or 0)\n"
"      end\n"
"      Print('COOP_TRACE hand_start_guest seed=' .. tostring(s) ..\n"
"            ' dealer=' .. tostring(d) ..\n"
"            ' chips=' .. table.concat(parts, ','))\n"
"      if exchangeTest then COOP_WaitForHandDeck = true end\n"
"      return origInit(self, seedOverride, dealerOverride)\n"
"    end\n"
"    -- Host: compute the next dealer manually (same logic AdvanceDealer\n"
"    -- uses with no override: increment to next non-busted player). This\n"
"    -- lets us broadcast dealer BEFORE origInit, so guest can force it as\n"
"    -- dealerOverride, bypassing the engine's own RandomInt-seeded choice.\n"
"    local players = self:GetPlayers()\n"
"    local nd = self.dealerIndex or 1\n"
"    local safety = 0\n"
"    repeat\n"
"      nd = nd + 1\n"
"      if nd > #players then nd = 1 end\n"
"      safety = safety + 1\n"
"    until (not players[nd]:IsBusted()) or safety > #players\n"
"    local s = math.random(1, 2000000000)\n"
"    math.randomseed(s)\n"
"    -- Snapshot current chip counts for all 5 seats; guest force-applies\n"
"    -- these at HAND_START so between-hand drift can't accumulate.\n"
"    local chipParts = {}\n"
"    for i = 1, 5 do\n"
"      local p = players[i]\n"
"      local c = (p and p.GetChips and p:GetChips()) or 0\n"
"      chipParts[i] = tostring(tonumber(c) or 0)\n"
"    end\n"
"    Print('COOP_TRACE hand_start_host seed=' .. tostring(s) ..\n"
"          ' dealer=' .. tostring(nd) ..\n"
"          ' chips=' .. table.concat(chipParts, ','))\n"
"    emit('HAND_START seed=' .. tostring(s)\n"
"              .. ' dealer=' .. tostring(nd)\n"
"              .. ' chips=' .. table.concat(chipParts, ','))\n"
"    if exchangeTest and COOP_ExchangeNewHand then COOP_ExchangeNewHand() end\n"
"    if exchangeTest then COOP_PublishHandDeck = true end\n"
"    return origInit(self, seedOverride, nd)\n"
"  end\n"
"end\n";

// Forward declaration â€” g_role_is_guest is defined with the config section
// in the net code below but coop_seat_claimed (right after this) reads it.
extern int g_role_is_guest_fwd;  // placeholder; actual symbol below has file scope
static int g_role_is_guest;      // file-scope; initializer at definition below is removed
// Phase 4c seat-ownership table. Hardcoded single entry for the first test:
// Brock is coop-claimed; every other seat plays normally. Once the TCP stub
// lands, this list becomes dynamic.
// shortcut: empty claim list keeps every AI seat host-driven, so hand #1
// mirrors cleanly on guest. Upgrade when seat-intent routing lands (guest
// click -> host runs that seat's turn on the clicker's behalf).
static const char *const COOP_CLAIMED_SEATS[] = {
    NULL,
};
static const char *coop_default_seat(void) {
    return COOP_CLAIMED_SEATS[0] ? COOP_CLAIMED_SEATS[0] : "Brock";
}

// COOP_SeatClaimed(name) -> truthy if `name` is coop-controlled.
// On guest, EVERY seat is claimed so host events drive the whole table.
static int __cdecl coop_seat_claimed(void *L) {
    if (g_role_is_guest) { p_lua_pushinteger(L, 1); return 1; }
    size_t len = 0;
    const char *name = p_lua_tolstring ? p_lua_tolstring(L, 1, &len) : NULL;
    if (!name) return 0;
    for (int i = 0; COOP_CLAIMED_SEATS[i]; ++i) {
        if (strcmp(name, COOP_CLAIMED_SEATS[i]) == 0) {
            p_lua_pushinteger(L, 1);
            return 1;
        }
    }
    return 0;
}

// ---- Network transport (Phase 4c) ----------------------------------------
// TCP listener on port 47103 (per docs/protocol.md). A client connects and
// sends newline-terminated commands:
//   FOLD
//   CHECK
//   CALL
//   RAISE <amount>
// The server keeps one "pending action" slot; the Lua wrapper polls it via
// COOP_HasPendingAction and consumes via COOP_TakePendingAction.

#define COOP_LISTEN_PORT 47103

// Action queue: FIFO ring. Host single-process keeps 1 item; guest may have
// up to 5 pending from a round of broadcasts.
#define Q_CAP 16
typedef struct { int action; int amount; char seat[32]; } ActionItem;
static CRITICAL_SECTION g_net_lock;
static ActionItem g_queue[Q_CAP];
static int g_q_head = 0, g_q_tail = 0;
static char g_latest_deck[1024] = {0};     // last received SHUFFLE from host
static volatile LONG g_hand_start_pending = 0;  // set on guest when host says HAND_START
static volatile LONG g_hand_start_seed    = 0;  // host's math.random seed for this hand
static volatile LONG g_hand_start_dealer  = 0;  // host's dealer seat index for this hand
static volatile LONG g_hand_start_chips[5];     // host's chip counts per seat (1..5) for this hand
// Latest banter-root broadcast from host. Guarded by g_net_lock.
static char g_banter_dlg[64]  = {0};
static char g_banter_node[64] = {0};  // stringified nodeID (numeric part only)
static volatile LONG g_banter_pending = 0;
// Chore replay queue: ring buffer of chore names host broadcast; guest's replay
// thread drains it via COOP_TakeChore. Guarded by g_net_lock.
#define CHORE_Q_CAP 64
#define CHORE_NAME_MAX 96
static char g_chore_queue[CHORE_Q_CAP][CHORE_NAME_MAX];
static int g_chore_q_head = 0, g_chore_q_tail = 0;
static volatile LONG g_net_started = 0;
static SOCKET g_client_sock = INVALID_SOCKET;
static PVOID volatile g_game_hwnd = NULL;
#include "display_mode.h"

// Optional role configuration lives next to the mod DLL.
// (g_role_is_guest declared earlier so coop_seat_claimed can read it.)
static char g_host_ip[64] = "127.0.0.1";
static int  g_host_port   = 47103;

#include "multi_guest_state.h"
#include "exchange_transport.h"
#include "seat_reservation.h"
#include "seat_control.h"
#include "multi_guest.h"
#include "presentation_sync.h"
#include "voice_sync.h"
#include "lobby.h"
#include "lobby_discovery.h"

static BOOL coop_read_config(void) {
    HANDLE h = CreateFileW(g_config_path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char buf[512] = {0};
    if (h == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            coop_log("config: could not open coop_config.txt, err=%lu", error);
            return FALSE;
        }
        // A fresh install can choose either role from the Multiplayer menu.
        g_lobby_mode = g_test_mode = g_seat_probe = g_seat_control = 1;
        g_two_guests = g_presentation_sync = g_voice_sync = 1;
        coop_log("config: coop_config.txt not found, using multiplayer lobby defaults");
    } else {
        DWORD n;
        BOOL read = ReadFile(h, buf, sizeof(buf) - 1, &n, NULL);
        CloseHandle(h);
        if (!read) {
            coop_log("config: could not read coop_config.txt");
            return FALSE;
        }
    }
    // simple key=value, one per line
    char *p = buf;
    while (*p) {
        char *end = strpbrk(p, "\r\n");
        if (end) *end = 0;
        if (!strncmp(p, "role=guest", 10))       g_role_is_guest = 1;
        else if (!strncmp(p, "role=host", 9))    g_role_is_guest = 0;
        else if (!strncmp(p, "host_ip=", 8))
            strncpy(g_host_ip, p + 8, sizeof(g_host_ip) - 1);
        else if (!strncmp(p, "host_port=", 10))  g_host_port = atoi(p + 10);
        else if (!strcmp(p, "test_mode=1"))      g_test_mode = 1;
        else if (!strcmp(p, "seat_probe=1"))     g_seat_probe = 1;
        else if (!strcmp(p, "seat_control=1"))   g_seat_control = 1;
        else if (!strcmp(p, "seat_picker=1"))    g_seat_picker = 1;
        else if (!strcmp(p, "two_guests=1"))     g_two_guests = 1;
        else if (!strcmp(p, "presentation_sync=1")) g_presentation_sync = 1;
        else if (!strcmp(p, "voice_sync=1"))     g_voice_sync = 1;
        else if (!strcmp(p, "lobby_mode=1"))     g_lobby_mode = 1;
        else if (!strcmp(p, "guest_instance=2")) g_guest_instance = 2;
        else if (!strncmp(p, "guest_seat=", 11)) {
            // Strict single-digit seat ID, never atoi's partial/negative parsing.
            g_requested_seat = (p[11] >= '1' && p[11] <= '4' && !p[12]) ? (unsigned)(p[11]-'0') : 0;
        }
        p = end ? end + 1 : p + strlen(p);
        while (*p == '\r' || *p == '\n') ++p;
    }
    coop_log("config: role=%s host=%s:%d",
             g_role_is_guest ? "guest" : "host", g_host_ip, g_host_port);
    coop_log("exchange: test_mode=%d protocol=%d", g_test_mode,g_lobby_mode?PN2_LOBBY_PROTOCOL:g_test_mode?(g_two_guests?(g_presentation_sync?(g_voice_sync?9:8):7):6):1);
    if (g_seat_probe) coop_log("seat: probe=%d requested=%u mode=reservation_only full_deck=1",g_test_mode,g_requested_seat);
    return TRUE;
}

static void seat_normalize(char *s) {
    for (; *s; ++s) if (*s == ' ') *s = '_';
}

static void queue_push(const char *seat, int action, int amount) {
    EnterCriticalSection(&g_net_lock);
    int nxt = (g_q_tail + 1) % Q_CAP;
    if (nxt == g_q_head) {  // full, drop oldest
        g_q_head = (g_q_head + 1) % Q_CAP;
        coop_log("queue: full, dropped oldest");
    }
    g_queue[g_q_tail].action = action;
    g_queue[g_q_tail].amount = amount;
    strncpy(g_queue[g_q_tail].seat,
            seat ? seat : "", sizeof(g_queue[0].seat) - 1);
    g_queue[g_q_tail].seat[sizeof(g_queue[0].seat) - 1] = 0;
    seat_normalize(g_queue[g_q_tail].seat);
    g_q_tail = nxt;
    LeaveCriticalSection(&g_net_lock);
    coop_log("queue: pushed seat=%s action=%d amount=%d", seat ? seat : "", action, amount);
}

// For the Brock/Node-client path: no seat in the message, use the single
// hardcoded claimed seat (the first entry of COOP_CLAIMED_SEATS defined above).
static void net_queue_simple(int action, int amount) {
    queue_push(coop_default_seat(), action, amount);
}

static void net_handle_line(const char *line) {
    if (g_lobby_mode && lb_handle_line(line)) return;
    if (g_lobby_mode && lb_phase == 5 && strcmp(line, "XFAULT")) return;
    if (voice_handle_line(line)) return;
    if (presentation_handle_line(line)) return;
    if (g_two_guests && mp_handle_line(line)) return;
    if (seat_control_handle_line(line)) return;
    if (seat_reservation_handle_line(line)) return;
    if (exchange_handle_line(line)) return;
    if (g_test_mode && g_seat_control && !g_role_is_guest) {
        coop_log("control: rejected legacy guest gameplay command");
        return;
    }
    // Simple one-word commands from the Node client (host single-process test).
    if      (!strcmp(line, "FOLD"))  { net_queue_simple(3, 0); return; }
    if      (!strcmp(line, "CHECK")) { net_queue_simple(4, 0); return; }
    if      (!strcmp(line, "CALL"))  { net_queue_simple(1, 0); return; }
    if      (!strncmp(line, "RAISE ", 6)) {
        net_queue_simple(2, atoi(line + 6));
        return;
    }

    // Broadcasts (guest side receives these from host).
    if (!strncmp(line, "HAND_START", 10)) {
        // Parse "HAND_START seed=N dealer=M chips=C1,C2,C3,C4,C5". Order-independent.
        int seed = 0, dealer = 0;
        int chips[5] = {0, 0, 0, 0, 0};
        const char *s = strstr(line, "seed=");
        if (s) seed = atoi(s + 5);
        const char *d = strstr(line, "dealer=");
        if (d) dealer = atoi(d + 7);
        const char *c = strstr(line, "chips=");
        if (c) {
            const char *p = c + 6;
            for (int i = 0; i < 5 && *p; ++i) {
                chips[i] = atoi(p);
                while (*p && *p != ',' && *p != ' ') ++p;
                if (*p == ',') ++p;
            }
        }
        // TCP orders the next hand's SHUFFLE after HAND_START. In test mode,
        // invalidate setup/previous-hand decks BEFORE publishing this hand.
        if (g_test_mode) {
            EnterCriticalSection(&g_net_lock);
            g_latest_deck[0] = 0;
            coop_log("net: hand_deck invalidated at HAND_START");
        }
        InterlockedExchange(&g_hand_start_seed, seed);
        InterlockedExchange(&g_hand_start_dealer, dealer);
        for (int i = 0; i < 5; ++i) InterlockedExchange(&g_hand_start_chips[i], chips[i]);
        InterlockedExchange(&g_hand_start_pending, 1);
        if (g_test_mode) LeaveCriticalSection(&g_net_lock);
        coop_log("net: HAND_START received seed=%d dealer=%d chips=%d,%d,%d,%d,%d",
                 seed, dealer, chips[0], chips[1], chips[2], chips[3], chips[4]);
        return;
    }
    if (!strncmp(line, "SHUFFLE deck=", 13)) {
        if (g_test_mode && !g_role_is_guest) {
            coop_log("net: ignored guest-authored SHUFFLE");
            return;
        }
        EnterCriticalSection(&g_net_lock);
        strncpy(g_latest_deck, line + 13, sizeof(g_latest_deck) - 1);
        g_latest_deck[sizeof(g_latest_deck) - 1] = 0;
        LeaveCriticalSection(&g_net_lock);
        coop_log("net: SHUFFLE deck stored (%zu chars)", strlen(g_latest_deck));
        return;
    }
    if (!strncmp(line, "BANTER_ROOT ", 12)) {
        // Parse "BANTER_ROOT dlg=NAME node=NNN" and stash the strings for guest's
        // Dialogue:StartBanter wrapper to consume.
        char dlg[64] = {0};
        char node[64] = {0};
        const char *d = strstr(line, "dlg=");
        if (d) {
            d += 4; int i = 0;
            while (*d && *d != ' ' && i < (int)sizeof(dlg) - 1) dlg[i++] = *d++;
            dlg[i] = 0;
        }
        const char *n = strstr(line, "node=");
        if (n) {
            n += 5; int i = 0;
            while (*n && *n != ' ' && *n != '\r' && *n != '\n' && i < (int)sizeof(node) - 1) node[i++] = *n++;
            node[i] = 0;
        }
        EnterCriticalSection(&g_net_lock);
        strncpy(g_banter_dlg,  dlg,  sizeof(g_banter_dlg)  - 1);
        strncpy(g_banter_node, node, sizeof(g_banter_node) - 1);
        g_banter_dlg[sizeof(g_banter_dlg) - 1] = 0;
        g_banter_node[sizeof(g_banter_node) - 1] = 0;
        LeaveCriticalSection(&g_net_lock);
        InterlockedExchange(&g_banter_pending, 1);
        coop_log("net: BANTER_ROOT received dlg=%s node=%s", dlg, node);
        return;
    }
    if (!strncmp(line, "CHORE ", 6)) {
        // Parse "CHORE name=X" and push to replay queue for guest's thread.
        const char *n = strstr(line, "name=");
        if (!n) return;
        n += 5;
        EnterCriticalSection(&g_net_lock);
        int next = (g_chore_q_tail + 1) % CHORE_Q_CAP;
        if (next == g_chore_q_head) {
            g_chore_q_head = (g_chore_q_head + 1) % CHORE_Q_CAP;
        }
        int i = 0;
        while (*n && *n != '\r' && *n != '\n' && i < CHORE_NAME_MAX - 1) {
            g_chore_queue[g_chore_q_tail][i++] = *n++;
        }
        g_chore_queue[g_chore_q_tail][i] = 0;
        char copy[CHORE_NAME_MAX];
        strncpy(copy, g_chore_queue[g_chore_q_tail], sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = 0;
        g_chore_q_tail = next;
        LeaveCriticalSection(&g_net_lock);
        static volatile LONG g_chore_rx_logged = 0;
        if (InterlockedIncrement(&g_chore_rx_logged) <= 5) {
            coop_log("net: CHORE received name=\"%s\"", copy);
        }
        return;
    }
    if (!strncmp(line, "ACTION_RESULT ", 14)) {
        // seat=NAME action=STR amount=N  â€” fields space-separated, seat already space->_
        char seat[32] = {0};
        char actstr[16] = {0};
        int amount = 0;
        // manual parse: tokenize by spaces
        const char *p = line + 14;
        while (*p) {
            if (!strncmp(p, "seat=", 5)) {
                p += 5; int i = 0;
                while (*p && *p != ' ' && i < (int)sizeof(seat) - 1) seat[i++] = *p++;
                seat[i] = 0;
            } else if (!strncmp(p, "action=", 7)) {
                p += 7; int i = 0;
                while (*p && *p != ' ' && i < (int)sizeof(actstr) - 1) actstr[i++] = *p++;
                actstr[i] = 0;
            } else if (!strncmp(p, "amount=", 7)) {
                amount = atoi(p + 7);
                while (*p && *p != ' ') p++;
            } else {
                p++;
            }
            while (*p == ' ') p++;
        }
        int code = 3;
        if (!strcmp(actstr, "call") || !strcmp(actstr, "Call")) code = 1;
        else if (!strcmp(actstr, "raise") || !strcmp(actstr, "Raise")
              || !strcmp(actstr, "bet")   || !strcmp(actstr, "Bet")
              || !strcmp(actstr, "allIn") || !strcmp(actstr, "AllIn")
              || !strcmp(actstr, "allin")) code = 2;
        else if (!strcmp(actstr, "check") || !strcmp(actstr, "Check")) code = 4;
        else if (!strcmp(actstr, "fold") || !strcmp(actstr, "Fold")) code = 3;
        queue_push(seat, code, amount);
        return;
    }

    coop_log("net: ignoring unknown line \"%s\"", line);
}

// Drain lines from a connected socket. Shared by host/guest paths.
static void net_pump_socket(SOCKET c) {
    // Expose for broadcast. For guest, broadcasts aren't sent anywhere
    // meaningful, but setting it costs nothing.
    EnterCriticalSection(&g_net_lock);
    g_client_sock = c;
    exchange_connected_locked();
    LeaveCriticalSection(&g_net_lock);
    // shortcut: 2048-byte buffer handles our longest lines (SHUFFLE deck ~160 chars).
    char rx[2048]; int rxn = 0;
    for (;;) {
        int n = recv(c, rx + rxn, (int)sizeof(rx) - 1 - rxn, 0);
        if (n <= 0) break;
        rxn += n; rx[rxn] = 0;
        int start = 0;
        for (int i = 0; i < rxn; ++i) {
            if (rx[i] == '\n' || rx[i] == '\r') {
                rx[i] = 0;
                if (i > start) net_handle_line(rx + start);
                start = i + 1;
            }
        }
        if (start < rxn) { memmove(rx, rx + start, rxn - start); rxn -= start; }
        else             { rxn = 0; }
    }
    EnterCriticalSection(&g_net_lock);
    g_client_sock = INVALID_SOCKET;
    exchange_disconnected_locked();
    LeaveCriticalSection(&g_net_lock);
}

static DWORD WINAPI net_server(LPVOID unused) {
    (void)unused;
    coop_log("net: thread started (tid=%u) role=%s",
             GetCurrentThreadId(), g_role_is_guest ? "guest" : "host");
    WSADATA wd;
    int wsa = WSAStartup(MAKEWORD(2, 2), &wd);
    coop_log("net: WSAStartup returned %d", wsa);
    if (wsa != 0) return 1;
    if (g_voice_sync && !(g_presentation_sync && g_two_guests)) {
        coop_log("voice: requires presentation_sync and two_guests; networking stopped");
        WSACleanup();return 1;
    }
    if (g_presentation_sync && !g_two_guests) {
        coop_log("presentation: requires two_guests=1; networking stopped");
        WSACleanup();return 1;
    }
    if (g_two_guests && !(g_test_mode && g_seat_probe && g_seat_control)) {
        coop_log("multi: requires test_mode/seat_probe/seat_control; networking stopped");
        WSACleanup();return 1;
    }
    if (g_role_is_guest) {
        coop_log("net: guest mode, dialing host %s:%d", g_host_ip, g_host_port);
        int attempt = 0;
        for (;;) {
            SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (c == INVALID_SOCKET) {
                coop_log("net: guest socket() failed, err=%d", WSAGetLastError());
                Sleep(500); continue;
            }
            struct sockaddr_in a = { 0 };
            a.sin_family = AF_INET;
            a.sin_port = htons((u_short)g_host_port);
            a.sin_addr.s_addr = inet_addr(g_host_ip);
            if (connect(c, (struct sockaddr *)&a, sizeof(a)) == 0) {
                coop_log("net: guest connected to host %s:%d (attempt %d)",
                         g_host_ip, g_host_port, attempt);
                attempt = 0;
                net_pump_socket(c);
                closesocket(c);
                coop_log("net: guest disconnected from host â€” reconnecting");
            } else {
                int err = WSAGetLastError();
                closesocket(c);
                if ((attempt++ % 10) == 0) {
                    coop_log("net: guest connect attempt %d failed, err=%d",
                             attempt, err);
                }
            }
            Sleep(1000);
        }
    } else {
        SOCKET sv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sv == INVALID_SOCKET) { coop_log("net: socket failed"); return 1; }
        BOOL opt = TRUE;
        setsockopt(sv, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((u_short)g_host_port);
        if (bind(sv, (struct sockaddr *)&a, sizeof(a)) != 0) {
            coop_log("net: bind failed, err=%d", WSAGetLastError());
            closesocket(sv); return 1;
        }
        if (listen(sv, g_two_guests ? MP_COUNT : 1) != 0) {
            coop_log("net: listen failed, err=%d", WSAGetLastError());
            closesocket(sv); return 1;
        }
        coop_log("net: listening on TCP %d", g_host_port);
        if (g_two_guests) {
            mp_server_loop(sv);closesocket(sv);WSACleanup();return 0;
        }
        for (;;) {
            struct sockaddr_in ca = { 0 };
            int cal = sizeof(ca);
            SOCKET c = accept(sv, (struct sockaddr *)&ca, &cal);
            if (c == INVALID_SOCKET) break;
            coop_log("net: client connected from %u.%u.%u.%u:%u",
                     ca.sin_addr.S_un.S_un_b.s_b1, ca.sin_addr.S_un.S_un_b.s_b2,
                     ca.sin_addr.S_un.S_un_b.s_b3, ca.sin_addr.S_un.S_un_b.s_b4,
                     ntohs(ca.sin_port));
            net_pump_socket(c);
            closesocket(c);
            coop_log("net: client disconnected");
        }
        closesocket(sv);
    }
    WSACleanup();
    return 0;
}

// Forward-declared from the ChorePlay hook block above. Host-only broadcast
// helper that skips sending if the role is guest, so a single call site works
// from inside hooks that run on both sides.
static volatile LONG g_broadcast_log_once = 0;
// Host-side gameplay-active flag synced from Lua (Logic.bInGame). Declared
// up top as forward; defined here. The C ChorePlay broadcast skips when
// this is 0 so title-screen / menu-return chores don't ripple to guest and
// drag its scene back to the menu.
static void coop_broadcast_from_c(const char *msg) {
    if (!msg) return;
    if (InterlockedExchange(&g_broadcast_log_once, 1) == 0) {
        coop_log("broadcast: first call role_guest=%d g_client_sock_valid=%d msg=\"%s\"",
                 g_role_is_guest, (g_client_sock != INVALID_SOCKET), msg);
    }
    if (g_role_is_guest) return;
    char buf[1024];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s\n", msg);
    if (n <= 0) return;
    EnterCriticalSection(&g_net_lock);
    SOCKET s = g_client_sock;
    int rc = -1;
    if (g_two_guests) rc = exchange_send_locked(buf) ? n : -1;
    else if (s != INVALID_SOCKET) rc = send(s, buf, n, 0);
    LeaveCriticalSection(&g_net_lock);
    static volatile LONG g_broadcast_logged = 0;
    if (InterlockedIncrement(&g_broadcast_logged) <= 3) {
        coop_log("broadcast: sent n=%d rc=%d msg=\"%s\"", n, rc, msg);
    }
}

// COOP_Broadcast(str) â€” send one newline-terminated line to the connected
// client. Silently drops when no client is connected. The actual send happens
// under the net lock because the accept thread might reset the socket.
static int __cdecl coop_broadcast(void *L) {
    size_t len = 0;
    const char *msg = p_lua_tolstring ? p_lua_tolstring(L, 1, &len) : NULL;
    if (!msg || len == 0) return 0;
    char buf[1024];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s\n", msg);
    if (n < 0) n = (int)sizeof(buf) - 1;
    EnterCriticalSection(&g_net_lock);
    SOCKET s = g_client_sock;
    if (g_two_guests) exchange_send_locked(buf);
    else if (s != INVALID_SOCKET) {
        send(s, buf, n, 0);   // best-effort; ignore errors (client may vanish)
    }
    LeaveCriticalSection(&g_net_lock);
    return 0;
}

static void start_net_once(void) {
    if (InterlockedCompareExchange(&g_net_started, 1, 0) != 0) {
        coop_log("start_net_once: already started, skipping");
        return;
    }
    InitializeCriticalSection(&g_net_lock);
    if(g_lobby_mode){HANDLE discovery=CreateThread(NULL,0,lb_discovery_thread,NULL,0,NULL);if(discovery)CloseHandle(discovery);coop_log("lobby: menu networking idle");return;}
    HANDLE t = CreateThread(NULL, 0, net_server, NULL, 0, NULL);
    coop_log("start_net_once: net thread handle=%p err=%u",
             t, t ? 0 : GetLastError());
    if (t) CloseHandle(t);
}

// Matches a queued item whose seat equals `name` (after space->_). The
// queue is FIFO but we scan from head looking for a seat-match so multiple
// pending actions for different seats don't block each other.
static int queue_find_for_seat(const char *name, int *out_action, int *out_amount) {
    char norm[32] = {0};
    strncpy(norm, name ? name : "", sizeof(norm) - 1);
    seat_normalize(norm);
    EnterCriticalSection(&g_net_lock);
    for (int i = g_q_head; i != g_q_tail; i = (i + 1) % Q_CAP) {
        if (!strcmp(g_queue[i].seat, norm) || g_queue[i].seat[0] == 0) {
            *out_action = g_queue[i].action;
            *out_amount = g_queue[i].amount;
            LeaveCriticalSection(&g_net_lock);
            return i;
        }
    }
    LeaveCriticalSection(&g_net_lock);
    return -1;
}

static void queue_consume_at(int idx) {
    EnterCriticalSection(&g_net_lock);
    // Shift everything after idx down by one slot.
    while (idx != g_q_head) {
        int prev = (idx - 1 + Q_CAP) % Q_CAP;
        g_queue[idx] = g_queue[prev];
        idx = prev;
    }
    g_q_head = (g_q_head + 1) % Q_CAP;
    LeaveCriticalSection(&g_net_lock);
}

// COOP_HasPendingAction(name) -> truthy if an action is queued for this seat.
static int __cdecl coop_has_pending_action(void *L) {
    const char *name = p_lua_tolstring ? p_lua_tolstring(L, 1, NULL) : NULL;
    int a, amt;
    int idx = queue_find_for_seat(name, &a, &amt);
    if (idx >= 0) { p_lua_pushinteger(L, 1); return 1; }
    return 0;
}

// COOP_TakePendingAction(name) -> (action, actionString, amount). Consumes one.
static int __cdecl coop_take_pending_action(void *L) {
    const char *name = p_lua_tolstring ? p_lua_tolstring(L, 1, NULL) : "?";
    int action = 3, amount = 0;
    int idx = queue_find_for_seat(name, &action, &amount);
    if (idx >= 0) queue_consume_at(idx);
    const char *as;
    switch (action) {
        case 1: as = "call";  break;
        case 2: as = "raise"; break;
        case 4: as = "check"; break;
        default: action = 3; as = "fold"; break;
    }
    coop_log("COOP: TakePendingAction seat=%s -> action=%d (%s) amount=%d",
             name, action, as, amount);
    p_lua_settop(L, 0);
    p_lua_pushinteger(L, action);
    p_lua_pushstring(L, as);
    p_lua_pushinteger(L, amount);
    return 3;
}

// COOP_IsGuest() -> truthy if this DLL is in guest mode.
static int __cdecl coop_is_guest(void *L) {
    if (g_role_is_guest) { p_lua_pushinteger(L, 1); return 1; }
    return 0;
}


// COOP_PollDeck() -> string of host's last SHUFFLE deck, or nil.
static int __cdecl coop_poll_deck(void *L) {
    EnterCriticalSection(&g_net_lock);
    int has = g_latest_deck[0] != 0;
    char buf[1024];
    if (has) { strncpy(buf, g_latest_deck, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0; }
    if (has && g_test_mode) g_latest_deck[0] = 0;
    LeaveCriticalSection(&g_net_lock);
    if (!has) return 0;
    if (g_test_mode) coop_log("net: hand_deck consumed (%zu chars)", strlen(buf));
    p_lua_pushstring(L, buf);
    return 1;
}

// COOP_ConsumeHandStart() -> truthy iff host sent HAND_START since last call.
static int __cdecl coop_consume_hand_start(void *L) {
    if (InterlockedExchange(&g_hand_start_pending, 0)) {
        p_lua_pushinteger(L, 1);
        return 1;
    }
    return 0;
}

// COOP_HandStartSeed() -> integer math.random seed from last HAND_START (0 if none).
// Non-destructive read; guest's InitRound wrapper reads this right after
// ConsumeHandStart returns truthy, then math.randomseed's it before origInit.
static int __cdecl coop_hand_start_seed(void *L) {
    p_lua_pushinteger(L, (int)g_hand_start_seed);
    return 1;
}

// COOP_HandStartDealer() -> host's dealerIndex from last HAND_START (0 if none).
static int __cdecl coop_hand_start_dealer(void *L) {
    p_lua_pushinteger(L, (int)g_hand_start_dealer);
    return 1;
}

// COOP_HandStartChips() -> five integers (seat 1..5 chip counts) pushed in order.
// Guest's InitRound wrapper reads these and SetChips on each seat before orig.
static int __cdecl coop_hand_start_chips(void *L) {
    for (int i = 0; i < 5; ++i) p_lua_pushinteger(L, (int)g_hand_start_chips[i]);
    return 5;
}

// COOP_SetGameplayActive(flag) â€” Lua tells the C broadcast gate whether
// Logic.bInGame is currently true. Logs transitions so we can see when it
// toggles.
static int __cdecl coop_set_gameplay_active(void *L) {
    const char *s = p_lua_tolstring ? p_lua_tolstring(L, 1, NULL) : NULL;
    int v = (s && s[0] == '1') ? 1 : 0;
    LONG prev = InterlockedExchange(&g_gameplay_active, v);
    if (prev != v) {
        coop_log("gameplay_active: %d -> %d", (int)prev, v);
    }
    return 0;
}

// COOP_TakeChore() -> next queued chore name string, or nothing if empty.
// Guest's Lua replay thread drains this in a tight loop and calls ChorePlay.
static int __cdecl coop_take_chore(void *L) {
    char buf[CHORE_NAME_MAX];
    int has = 0;
    EnterCriticalSection(&g_net_lock);
    if (g_chore_q_head != g_chore_q_tail) {
        strncpy(buf, g_chore_queue[g_chore_q_head], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        g_chore_q_head = (g_chore_q_head + 1) % CHORE_Q_CAP;
        has = 1;
    }
    LeaveCriticalSection(&g_net_lock);
    if (!has) return 0;
    p_lua_pushstring(L, buf);
    return 1;
}

// COOP_HasBanterRoot() -> truthy if a banter-root is queued (non-destructive).
static int __cdecl coop_has_banter_root(void *L) {
    if (g_banter_pending) { p_lua_pushinteger(L, 1); return 1; }
    return 0;
}

// COOP_TakeBanterRoot() -> (dlg, node) strings if a banter-root is pending;
// clears the pending flag. Returns nothing if nothing is queued.
static int __cdecl coop_take_banter_root(void *L) {
    if (!InterlockedExchange(&g_banter_pending, 0)) return 0;
    char dlg[64], node[64];
    EnterCriticalSection(&g_net_lock);
    strncpy(dlg,  g_banter_dlg,  sizeof(dlg)  - 1); dlg[sizeof(dlg)  - 1] = 0;
    strncpy(node, g_banter_node, sizeof(node) - 1); node[sizeof(node) - 1] = 0;
    LeaveCriticalSection(&g_net_lock);
    p_lua_pushstring(L, dlg);
    p_lua_pushstring(L, node);
    return 2;
}

// Legacy fold stub â€” retained in case COOP_NetworkAction is still referenced
// anywhere. Not registered as a global any more.
static int __cdecl coop_network_action(void *L) {
    const char *name = p_lua_tolstring ? p_lua_tolstring(L, 1, NULL) : NULL;
    coop_log("COOP: NetworkAction fallback for seat=\"%s\" -> fold",
             name ? name : "<null>");
    p_lua_settop(L, 0);
    p_lua_pushinteger(L, 3);
    p_lua_pushstring(L, "fold");
    return 2;
}

// Install closure: called under pcall by setagent hook.
//
// Registers COOP_SeatClaimed + COOP_NetworkAction as globals, then runs the
// wrapper Lua source via _G.loadstring(src)() â€” which compiles the wrapper
// and installs it as AIPlayer.EvaluateAndAct. The wrapper is idempotent
// (guarded by AIPlayer.COOP_Wrapped) so running it on a scene where it's
// already wrapped is a no-op.
//
// Errors anywhere (AIPlayer not yet defined, loadstring failure, pcall
// of the chunk) propagate to the setagent-hook's pcall which swallows them;
// we'll retry next SetAgent for this L.
static int __cdecl coop_install_ai_hook(void *L) {
    if(g_lobby_mode) {
        if (!g_orig_save_preferences) return 0;
        p_lua_pushstring(L, "COOP_ProtectPreferences");
        p_lua_pushcclosure(L, coop_protect_preferences, 0);
        p_lua_rawset(L, LUA_GLOBALSINDEX);
        lb_register_lua(L);
        p_lua_pushstring(L, "COOP_DisplayWindow");
        p_lua_pushcclosure(L, coop_display_window, 0);
        p_lua_rawset(L, LUA_GLOBALSINDEX);
        p_lua_getfield(L,LUA_GLOBALSINDEX,"loadstring");p_lua_pushstring(L,COOP_DISPLAY_MODE_LUA);
        if(p_lua_pcall(L,1,1,0)!=0 || p_lua_pcall(L,0,0,0)!=0)return 0;
        p_lua_getfield(L,LUA_GLOBALSINDEX,"loadstring");p_lua_pushstring(L,COOP_SESSION_LUA);
        if (p_lua_pcall(L,1,1,0)!=0 || p_lua_pcall(L,0,0,0)!=0) {
            coop_log("install: session error: %s", p_lua_tolstring(L, -1, NULL));
            return 0;
        }
        p_lua_getfield(L,LUA_GLOBALSINDEX,"loadstring");p_lua_pushstring(L,COOP_LOBBY_LUA);
        if(p_lua_pcall(L,1,1,0)!=0 || p_lua_pcall(L,0,0,0)!=0)return 0;
        if(lb_phase!=3)return 0; // Role-dependent gameplay wrappers wait for the lobby commit.
    }
    exchange_register_lua(L);
    seat_control_register_lua(L);
    presentation_register_lua(L);
    voice_register_lua(L);
#ifndef PN2_SPECTATING_ONLY
    if (g_lobby_mode) {
        p_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
        p_lua_pushstring(L, COOP_HAND_READY_LUA);
        if (p_lua_pcall(L, 1, 1, 0) != 0 || p_lua_pcall(L, 0, 0, 0) != 0) {
            coop_log("install: hand-ready error: %s", p_lua_tolstring(L, -1, NULL));
            return 0;
        }
        p_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
        p_lua_pushstring(L, COOP_MATCH_RESULTS_LUA);
        if (p_lua_pcall(L, 1, 1, 0) != 0 || p_lua_pcall(L, 0, 0, 0) != 0) {
            coop_log("install: match results error: %s", p_lua_tolstring(L, -1, NULL));
            return 0;
        }
    }
#endif
    // 1. Register the two C stubs as globals. rawset(GLOBALS) is idempotent.
    p_lua_pushstring(L, "COOP_SeatClaimed");
    p_lua_pushcclosure(L, coop_seat_claimed, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_HasPendingAction");
    p_lua_pushcclosure(L, coop_has_pending_action, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_TakePendingAction");
    p_lua_pushcclosure(L, coop_take_pending_action, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_Broadcast");
    p_lua_pushcclosure(L, coop_broadcast, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_IsGuest");
    p_lua_pushcclosure(L, coop_is_guest, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_PollDeck");
    p_lua_pushcclosure(L, coop_poll_deck, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_ConsumeHandStart");
    p_lua_pushcclosure(L, coop_consume_hand_start, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_HandStartSeed");
    p_lua_pushcclosure(L, coop_hand_start_seed, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_HandStartDealer");
    p_lua_pushcclosure(L, coop_hand_start_dealer, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_HandStartChips");
    p_lua_pushcclosure(L, coop_hand_start_chips, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_TakeChore");
    p_lua_pushcclosure(L, coop_take_chore, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_SetGameplayActive");
    p_lua_pushcclosure(L, coop_set_gameplay_active, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_HasBanterRoot");
    p_lua_pushcclosure(L, coop_has_banter_root, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    p_lua_pushstring(L, "COOP_TakeBanterRoot");
    p_lua_pushcclosure(L, coop_take_banter_root, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);

    // 2. Compile the wrapper source: _G.loadstring(COOP_WRAPPER_SRC) -> chunk.
    //    Stack: []  ->  [loadstring]
    p_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
    //    Stack: [loadstring]  ->  [loadstring, src]
    p_lua_pushstring(L, COOP_WRAPPER_SRC);
    //    Stack: [loadstring, src]  ->  [chunk]  (pcall discards the function)
    //    loadstring returns (chunk) on success or (nil, msg) on compile error.
    //    We request 1 result; on error the result is nil, and the following
    //    pcall-of-chunk will fail with "attempt to call a nil value" which
    //    the outer pcall catches.
    int lerr = p_lua_pcall(L, 1, 1, 0);
    if (lerr != 0) {
        return 0;  // outer pcall sees error and clears stack
    }
    // 3. Run the compiled chunk. It installs the wrapper into AIPlayer.
    //    Stack: [chunk]  ->  []
    int cerr = p_lua_pcall(L, 0, 0, 0);
    if (cerr != 0) {
        coop_log("install: wrapper error: %s", p_lua_tolstring(L, -1, NULL));
        return 0;
    }
    if (g_test_mode) {
        const char *chunks[] = { COOP_EXCHANGE_LUA, COOP_SHOWDOWN_NODES_LUA, COOP_SHOWDOWN_LUA, COOP_BUSTOUT_NODES_LUA, COOP_BUSTOUT_LUA, COOP_CONTROL_LUA, COOP_CARD_UI_LUA, COOP_PACING_LUA, COOP_DIAGNOSTICS_LUA };
        const char *chunk_names[] = { "exchange", "showdown_nodes", "showdown", "bustout_nodes", "bustout", "control", "card_ui", "pacing", "diagnostics" };
        for (int i = 0; i < 9; ++i) {
            p_lua_getfield(L, LUA_GLOBALSINDEX, "loadstring");
            p_lua_pushstring(L, chunks[i]);
            if (p_lua_pcall(L, 1, 1, 0) != 0 || p_lua_pcall(L, 0, 0, 0) != 0) {
                coop_log("install: %s error: %s", chunk_names[i], p_lua_tolstring(L, -1, NULL));
                return 0;
            }
        }
    }
    p_lua_pushstring(L, "ok");
    return 1;
}

// PN2 uses multiple lua_State instances (observed: 10B80E84 boot, 10BF3E30
// menu, 10C3C794/10C53314/â€¦ gameplay). Each has its own globals, so our
// AIPlayer override must be installed per unique L. A small LRU-ish set
// tracks which Ls we've already installed on.
#define MAX_TRACKED_LS 64
static void *g_installed_ls[MAX_TRACKED_LS];
static volatile LONG g_installed_count = 0;

static void coop_reset_install_cache(void) {
    InterlockedExchange(&g_installed_count, 0);
}

static int is_ai_hook_installed_on(void *L) {
    LONG n = g_installed_count;
    if (n > MAX_TRACKED_LS) n = MAX_TRACKED_LS;
    for (LONG i = 0; i < n; ++i) if (g_installed_ls[i] == L) return 1;
    return 0;
}

static void mark_ai_hook_installed_on(void *L) {
    LONG n = InterlockedIncrement(&g_installed_count);
    if (n <= MAX_TRACKED_LS) g_installed_ls[n - 1] = L;
}

static int __cdecl hook_setagent(void *L) {
    const int result = g_orig_setagent(L);
    if (!p_lua_gettop || !p_lua_settop || !p_lua_checkstack ||
        !p_lua_pushstring || !p_lua_pushcclosure || !p_lua_rawset ||
        !p_lua_getfield || !p_lua_setfield || !p_lua_pcall) {
        return result;
    }
    int saved = p_lua_gettop(L);
    if (!p_lua_checkstack(L, 8)) {
        coop_log("setagent hook: checkstack(8) failed at L=%p", L);
        return result;
    }

    // Always-on: overwrite _G.Print (idempotent, cheap).
    p_lua_pushstring(L, "Print");
    p_lua_pushcclosure(L, coop_print_hook, 0);
    p_lua_rawset(L, LUA_GLOBALSINDEX);
    p_lua_settop(L, saved);

    // Install the AI override ONCE per unique L. Install is pcall-wrapped so
    // early fires (AIPlayer not yet defined on this L) silently fail; we try
    // again on the next SetAgent on the same L.
    if (!is_ai_hook_installed_on(L)) {
        p_lua_pushcclosure(L, coop_install_ai_hook, 0);
        int err = p_lua_pcall(L, 0, 1, 0);
        const char *installed = err == 0 ? p_lua_tolstring(L, -1, NULL) : NULL;
        int success = installed && strcmp(installed, "ok") == 0;
        p_lua_settop(L, saved);
        if (success) {
            mark_ai_hook_installed_on(L);
            coop_log("setagent hook: AIPlayer:EvaluateAndAct override installed on L=%p", L);
        }
        // else pcall left the error on stack; settop above cleared it.
    }

    LONG n = InterlockedIncrement(&g_setagent_install_count);
    if (n == 1) {
        coop_log("setagent hook: first fire on L=%p", L);
    } else if ((n % 100) == 0) {
        coop_log("setagent hook: fire #%ld on L=%p (unique Ls installed=%ld)",
                 n, L, g_installed_count);
    }
    return result;
}

// ---- Install all hooks ----------------------------------------------------

static BOOL install_coop_hooks(void) {
    // Resolve Lua C API pointers.
    p_lua_gettop       = (PFN_lua_gettop)      resolve_va(VA_LUA_GETTOP);
    p_lua_settop       = (PFN_lua_settop)      resolve_va(VA_LUA_SETTOP);
    p_lua_checkstack   = (PFN_lua_checkstack)  resolve_va(VA_LUA_CHECKSTACK);
    p_lua_pushinteger  = (PFN_lua_pushinteger) resolve_va(VA_LUA_PUSHINTEGER);
    p_lua_pushstring   = (PFN_lua_pushstring)  resolve_va(VA_LUA_PUSHSTRING);
    p_lua_pushcclosure = (PFN_lua_pushcclosure)resolve_va(VA_LUA_PUSHCCLOSURE);
    p_lua_rawset       = (PFN_lua_rawset)      resolve_va(VA_LUA_RAWSET);
    p_lua_pcall        = (PFN_lua_pcall)       resolve_va(VA_LUA_PCALL);
    p_lua_tolstring    = (PFN_lua_tolstring)   resolve_va(VA_LUA_TOLSTRING);
    p_lua_getfield     = (PFN_lua_getfield)    resolve_va(VA_LUA_GETFIELD);
    p_lua_setfield     = (PFN_lua_setfield)    resolve_va(VA_LUA_SETFIELD);
    coop_log("install: base_delta=0x%08X api: gettop=%p pushstring=%p pushclosure=%p rawset=%p getfield=%p setfield=%p",
             (unsigned)g_base_delta, p_lua_gettop, p_lua_pushstring,
             p_lua_pushcclosure, p_lua_rawset, p_lua_getfield, p_lua_setfield);

    // Shared RWX region for the trampolines.
    void *tr_page = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_EXECUTE_READWRITE);
    if (!tr_page) {
        coop_log("install: VirtualAlloc for trampolines failed, err=%u", GetLastError());
        return FALSE;
    }
    g_choreplay_trampoline = tr_page;
    g_setagent_trampoline  = (uint8_t *)tr_page + 16;

    if (g_lobby_mode) {
        // push ebp; mov ebp,esp; mov eax,fs:[0]. Resume at the next instruction.
        void *target = resolve_va(VA_SAVE_PREFERENCES);
        void *trampoline = (uint8_t *)tr_page + 32;
        if (install_trampoline_span(target, (void *)&hook_save_preferences,
                                    trampoline, sizeof(g_save_prologue))) {
            g_orig_save_preferences = (PFN_save_preferences)trampoline;
            coop_log("install: native profile write protection ready");
        } else {
            coop_log("install: native profile write protection unavailable");
            VirtualFree(tr_page, 0, MEM_RELEASE);
            return FALSE;
        }
    }

    // Install ChorePlay trampoline.
    g_choreplay_addr = resolve_va(VA_LUA_CHOREPLAY);
    if (install_trampoline(g_choreplay_addr, (void *)&hook_lua_ChorePlay,
                           g_choreplay_trampoline)) {
        g_orig_choreplay = (PFN_lua_CFunction)g_choreplay_trampoline;
        coop_log("install: choreplay trampoline at %p, orig=%p",
                 g_choreplay_addr, g_orig_choreplay);
    } else {
        coop_log("install: choreplay trampoline install failed");
    }

    // Install SetAgent trampoline.
    g_setagent_addr = resolve_va(VA_GAME_SET_AGENT);
    if (install_trampoline(g_setagent_addr, (void *)&hook_setagent,
                           g_setagent_trampoline)) {
        g_orig_setagent = (PFN_lua_CFunction)g_setagent_trampoline;
        coop_log("install: setagent trampoline at %p, orig=%p",
                 g_setagent_addr, g_orig_setagent);
    } else {
        coop_log("install: setagent trampoline install failed");
    }
    return g_orig_choreplay && g_orig_setagent;
}

// ---- Guest mutex rename (defeat single-instance block) --------------------
// PN2 (or Goldberg Steam emu) creates a named mutex on startup to prevent a
// second instance. When guest tries the same name, the mutex already exists,
// GetLastError() returns ERROR_ALREADY_EXISTS, and the game exits before
// creating its window. Fix: hook CreateMutex/OpenMutex in guest mode and
// append "_coop_guest" so guest uses a parallel universe of names. Must run
// in DllMain before Goldberg's own code â€” proxy DLL load order gives us this.

typedef HANDLE(WINAPI *PFN_CreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
typedef HANDLE(WINAPI *PFN_CreateMutexW)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
typedef HANDLE(WINAPI *PFN_OpenMutexA)  (DWORD, BOOL, LPCSTR);
typedef HANDLE(WINAPI *PFN_OpenMutexW)  (DWORD, BOOL, LPCWSTR);
typedef HANDLE(WINAPI *PFN_CreateEventA)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
typedef HANDLE(WINAPI *PFN_CreateEventW)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
typedef HANDLE(WINAPI *PFN_OpenEventA)  (DWORD, BOOL, LPCSTR);
typedef HANDLE(WINAPI *PFN_OpenEventW)  (DWORD, BOOL, LPCWSTR);
typedef HANDLE(WINAPI *PFN_CreateFileMappingA)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
typedef HANDLE(WINAPI *PFN_CreateFileMappingW)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
typedef HANDLE(WINAPI *PFN_OpenFileMappingA)  (DWORD, BOOL, LPCSTR);
typedef HANDLE(WINAPI *PFN_OpenFileMappingW)  (DWORD, BOOL, LPCWSTR);

static PFN_CreateMutexA g_orig_CreateMutexA = NULL;
static PFN_CreateMutexW g_orig_CreateMutexW = NULL;
static PFN_OpenMutexA   g_orig_OpenMutexA   = NULL;
static PFN_OpenMutexW   g_orig_OpenMutexW   = NULL;
static PFN_CreateEventA g_orig_CreateEventA = NULL;
static PFN_CreateEventW g_orig_CreateEventW = NULL;
static PFN_OpenEventA   g_orig_OpenEventA   = NULL;
static PFN_OpenEventW   g_orig_OpenEventW   = NULL;
static PFN_CreateFileMappingA g_orig_CreateFileMappingA = NULL;
static PFN_CreateFileMappingW g_orig_CreateFileMappingW = NULL;
static PFN_OpenFileMappingA   g_orig_OpenFileMappingA   = NULL;
static PFN_OpenFileMappingW   g_orig_OpenFileMappingW   = NULL;

// Only rename names matching patterns known to be used for single-instance
// detection by PN2 / Goldberg Steam emu. Leaves Windows internals untouched.
static int should_rename_a(const char *name) {
    if (!name || !name[0]) return 0;
    const char *patterns[] = { "Steam", "Pn2", "PN2", "Poker", "Telltale", "SKUCPP", NULL };
    for (int i = 0; patterns[i]; ++i)
        if (strstr(name, patterns[i])) return 1;
    return 0;
}
static int should_rename_w(const wchar_t *name) {
    if (!name || !name[0]) return 0;
    const wchar_t *patterns[] = { L"Steam", L"Pn2", L"PN2", L"Poker", L"Telltale", L"SKUCPP", NULL };
    for (int i = 0; patterns[i]; ++i)
        if (wcsstr(name, patterns[i])) return 1;
    return 0;
}

static HANDLE WINAPI hook_CreateMutexA(LPSECURITY_ATTRIBUTES a, BOOL b, LPCSTR name) {
    if (g_role_is_guest && should_rename_a(name)) {
        char new_name[256];
        _snprintf_s(new_name, sizeof(new_name), _TRUNCATE, "%s_coop_guest%s", name, g_guest_instance==2 ? "2" : "");
        coop_log("mutex/A create: '%s' -> '%s'", name, new_name);
        return g_orig_CreateMutexA(a, b, new_name);
    }
    return g_orig_CreateMutexA(a, b, name);
}
static HANDLE WINAPI hook_CreateMutexW(LPSECURITY_ATTRIBUTES a, BOOL b, LPCWSTR name) {
    if (g_role_is_guest && should_rename_w(name)) {
        wchar_t new_name[256];
        _snwprintf_s(new_name, 256, _TRUNCATE, L"%s_coop_guest%s", name, g_guest_instance==2 ? L"2" : L"");
        coop_log("mutex/W create: '%ls' -> '%ls'", name, new_name);
        return g_orig_CreateMutexW(a, b, new_name);
    }
    return g_orig_CreateMutexW(a, b, name);
}
static HANDLE WINAPI hook_OpenMutexA(DWORD a, BOOL b, LPCSTR name) {
    if (g_role_is_guest && should_rename_a(name)) {
        char new_name[256];
        _snprintf_s(new_name, sizeof(new_name), _TRUNCATE, "%s_coop_guest%s", name, g_guest_instance==2 ? "2" : "");
        coop_log("mutex/A open: '%s' -> '%s'", name, new_name);
        return g_orig_OpenMutexA(a, b, new_name);
    }
    return g_orig_OpenMutexA(a, b, name);
}
static HANDLE WINAPI hook_OpenMutexW(DWORD a, BOOL b, LPCWSTR name) {
    if (g_role_is_guest && should_rename_w(name)) {
        wchar_t new_name[256];
        _snwprintf_s(new_name, 256, _TRUNCATE, L"%s_coop_guest%s", name, g_guest_instance==2 ? L"2" : L"");
        coop_log("mutex/W open: '%ls' -> '%ls'", name, new_name);
        return g_orig_OpenMutexW(a, b, new_name);
    }
    return g_orig_OpenMutexW(a, b, name);
}

static HANDLE WINAPI hook_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL init, LPCSTR name) {
    if (g_role_is_guest && should_rename_a(name)) {
        char new_name[256];
        _snprintf_s(new_name, sizeof(new_name), _TRUNCATE, "%s_coop_guest%s", name, g_guest_instance==2 ? "2" : "");
        coop_log("event/A create: '%s' -> '%s'", name, new_name);
        return g_orig_CreateEventA(sa, manual, init, new_name);
    }
    return g_orig_CreateEventA(sa, manual, init, name);
}
static HANDLE WINAPI hook_CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL init, LPCWSTR name) {
    if (g_role_is_guest && should_rename_w(name)) {
        wchar_t new_name[256];
        _snwprintf_s(new_name, 256, _TRUNCATE, L"%s_coop_guest%s", name, g_guest_instance==2 ? L"2" : L"");
        coop_log("event/W create: '%ls' -> '%ls'", name, new_name);
        return g_orig_CreateEventW(sa, manual, init, new_name);
    }
    return g_orig_CreateEventW(sa, manual, init, name);
}
static HANDLE WINAPI hook_OpenEventA(DWORD acc, BOOL inherit, LPCSTR name) {
    if (g_role_is_guest && should_rename_a(name)) {
        char new_name[256];
        _snprintf_s(new_name, sizeof(new_name), _TRUNCATE, "%s_coop_guest%s", name, g_guest_instance==2 ? "2" : "");
        coop_log("event/A open: '%s' -> '%s'", name, new_name);
        return g_orig_OpenEventA(acc, inherit, new_name);
    }
    return g_orig_OpenEventA(acc, inherit, name);
}
static HANDLE WINAPI hook_OpenEventW(DWORD acc, BOOL inherit, LPCWSTR name) {
    if (g_role_is_guest && should_rename_w(name)) {
        wchar_t new_name[256];
        _snwprintf_s(new_name, 256, _TRUNCATE, L"%s_coop_guest%s", name, g_guest_instance==2 ? L"2" : L"");
        coop_log("event/W open: '%ls' -> '%ls'", name, new_name);
        return g_orig_OpenEventW(acc, inherit, new_name);
    }
    return g_orig_OpenEventW(acc, inherit, name);
}
static HANDLE WINAPI hook_CreateFileMappingA(HANDLE f, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD szh, DWORD szl, LPCSTR name) {
    if (g_role_is_guest && should_rename_a(name)) {
        char new_name[256];
        _snprintf_s(new_name, sizeof(new_name), _TRUNCATE, "%s_coop_guest%s", name, g_guest_instance==2 ? "2" : "");
        coop_log("map/A create: '%s' -> '%s'", name, new_name);
        return g_orig_CreateFileMappingA(f, sa, prot, szh, szl, new_name);
    }
    return g_orig_CreateFileMappingA(f, sa, prot, szh, szl, name);
}
static HANDLE WINAPI hook_CreateFileMappingW(HANDLE f, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD szh, DWORD szl, LPCWSTR name) {
    if (g_role_is_guest && should_rename_w(name)) {
        wchar_t new_name[256];
        _snwprintf_s(new_name, 256, _TRUNCATE, L"%s_coop_guest%s", name, g_guest_instance==2 ? L"2" : L"");
        coop_log("map/W create: '%ls' -> '%ls'", name, new_name);
        return g_orig_CreateFileMappingW(f, sa, prot, szh, szl, new_name);
    }
    return g_orig_CreateFileMappingW(f, sa, prot, szh, szl, name);
}
static HANDLE WINAPI hook_OpenFileMappingA(DWORD acc, BOOL inherit, LPCSTR name) {
    if (g_role_is_guest && should_rename_a(name)) {
        char new_name[256];
        _snprintf_s(new_name, sizeof(new_name), _TRUNCATE, "%s_coop_guest%s", name, g_guest_instance==2 ? "2" : "");
        coop_log("map/A open: '%s' -> '%s'", name, new_name);
        return g_orig_OpenFileMappingA(acc, inherit, new_name);
    }
    return g_orig_OpenFileMappingA(acc, inherit, name);
}
static HANDLE WINAPI hook_OpenFileMappingW(DWORD acc, BOOL inherit, LPCWSTR name) {
    if (g_role_is_guest && should_rename_w(name)) {
        wchar_t new_name[256];
        _snwprintf_s(new_name, 256, _TRUNCATE, L"%s_coop_guest%s", name, g_guest_instance==2 ? L"2" : L"");
        coop_log("map/W open: '%ls' -> '%ls'", name, new_name);
        return g_orig_OpenFileMappingW(acc, inherit, new_name);
    }
    return g_orig_OpenFileMappingW(acc, inherit, name);
}

static void hook_one(HMODULE k32, const char *api, void *our_hook,
                     void **orig_slot, const char *label) {
    void *target = (void *)GetProcAddress(k32, api);
    if (!target) { coop_log("mutex: %s not found", api); return; }
    void *tr = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { coop_log("mutex: VirtualAlloc for %s trampoline failed", api); return; }
    if (install_trampoline(target, our_hook, tr)) {
        *orig_slot = tr;
        coop_log("mutex: hooked %s (target=%p tramp=%p) %s", api, target, tr, label);
    } else {
        coop_log("mutex: install_trampoline failed for %s", api);
    }
}

static void install_mutex_rename(void) {
    // Disabled: the kernel32 trampolines break LoadLibraryA's internal
    // event sync (observed: g_real_dinput8 returns NULL on guest process,
    // then PN2's DirectInput8Create jumps to null and crashes before any
    // window appears). Single-instance is handled by the exe-name rename.
    return;
    if (!g_role_is_guest) return;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) { coop_log("mutex: GetModuleHandleA(kernel32) failed"); return; }
    hook_one(k32, "CreateMutexA", (void *)&hook_CreateMutexA,
             (void **)&g_orig_CreateMutexA, "A");
    hook_one(k32, "CreateMutexW", (void *)&hook_CreateMutexW,
             (void **)&g_orig_CreateMutexW, "W");
    hook_one(k32, "OpenMutexA",   (void *)&hook_OpenMutexA,
             (void **)&g_orig_OpenMutexA,   "A");
    hook_one(k32, "OpenMutexW",   (void *)&hook_OpenMutexW,
             (void **)&g_orig_OpenMutexW,   "W");
    hook_one(k32, "CreateEventA", (void *)&hook_CreateEventA,
             (void **)&g_orig_CreateEventA, "A");
    hook_one(k32, "CreateEventW", (void *)&hook_CreateEventW,
             (void **)&g_orig_CreateEventW, "W");
    hook_one(k32, "OpenEventA",   (void *)&hook_OpenEventA,
             (void **)&g_orig_OpenEventA,   "A");
    hook_one(k32, "OpenEventW",   (void *)&hook_OpenEventW,
             (void **)&g_orig_OpenEventW,   "W");
    // NOTE: Skipping CreateFileMapping* / OpenFileMapping*. Those have 6-byte
    // JMP-indirect (`FF 25 xx xx xx xx`) prologues on modern kernel32 and our
    // 5-byte patch corrupts them, which breaks LoadLibrary. If Steam IPC shared
    // memory turns out to be the single-instance mechanism, we'll revisit with
    // a longer-prologue aware trampoline or IAT hooking in steam_api.dll.
}

// ---- PN2 single-instance / "background run" byte patch -------------------
// Jackbox's prior-attempt documented this at RVA 0x557D0 (VA 0x4557D0 under
// preferred image base 0x00400000). The function originally returns
// (external_value == this->member); we force it to always return true.
// Jackbox called it "BackgroundCodePatch" â€” speculation is it's PN2's
// "is this the active app" check consulted elsewhere in the engine.
//
// Byte signature verified identical in our PN2 build (SHA256 matches).
static void patch_background_run(HMODULE exe) {
    uint8_t *addr = resolve_va(VA_BACKGROUND_RUN);
    if (!matches_game_code(exe, VA_BACKGROUND_RUN,
                           g_background_prologue, sizeof(g_background_prologue))) {
        coop_log("patch: unexpected bytes at %p, skipping", addr);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(addr, 3, PAGE_EXECUTE_READWRITE, &old)) {
        coop_log("patch: VirtualProtect failed err=%u", GetLastError());
        return;
    }
    addr[0] = 0xB0;  // mov al, 1
    addr[1] = 0x01;
    addr[2] = 0xC3;  // ret
    VirtualProtect(addr, 3, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, 3);
    coop_log("patch: background-run check at %p forced to always-true", addr);
}

// ---- Focus-pause defeat ---------------------------------------------------
// PN2 pauses when it loses foreground. Needed for two-window sync testing:
// subclass the game window's wndproc to lie about focus state.

static WNDPROC g_orig_wndproc = NULL;

static LRESULT CALLBACK coop_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ACTIVATEAPP: w = TRUE; break;         // always active
        case WM_ACTIVATE:    w = WA_ACTIVE; break;    // never WA_INACTIVE
        case WM_KILLFOCUS:   return 0;                // swallow focus loss
    }
    return CallWindowProcW(g_orig_wndproc, h, m, w, l);
}

static BOOL CALLBACK enum_find_pn2_window(HWND h, LPARAM lp) {
    DWORD wp = 0;
    GetWindowThreadProcessId(h, &wp);
    if (wp == GetCurrentProcessId()
        && GetWindow(h, GW_OWNER) == NULL
        && IsWindowVisible(h)) {
        *(HWND *)lp = h;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI focus_patch_thread(LPVOID unused) {
    (void)unused;
    coop_log("focus: window-watcher thread started (tid=%u)", GetCurrentThreadId());
    for (int tries = 0; tries < 300; ++tries) {
        HWND h = NULL;
        EnumWindows(enum_find_pn2_window, (LPARAM)&h);
        if (h) {
            InterlockedExchangePointer(&g_game_hwnd, h);
            g_orig_wndproc =
                (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)coop_wndproc);
            coop_log("focus: subclassed PN2 window %p after %d tries, orig wndproc=%p",
                     h, tries, g_orig_wndproc);
            return 0;
        }
        if ((tries % 50) == 0 && tries > 0) {
            coop_log("focus: still waiting for window, tries=%d", tries);
        }
        Sleep(100);
    }
    coop_log("focus: PN2 window not found after 30s â€” focus patch not applied");
    return 1;
}

// ---- DllMain --------------------------------------------------------------

// Plain images can be hooked during attach. A wrapped image may make the
// game code available only after its entry point runs.
static BOOL try_start_coop(BOOL retry) {
    HMODULE exe = GetModuleHandleW(NULL);
    if (!exe) return TRUE;
    g_base_delta = (uintptr_t)exe - PN2_PREFERRED_BASE;
    if (!validate_game_code(exe, g_lobby_mode)) {
        coop_log(retry ? "install: unsupported executable layout, multiplayer disabled"
                       : "install: waiting for game code at DirectInput8Create");
        return FALSE;
    }
    if (!install_coop_hooks()) return TRUE;
    patch_background_run(exe);
    install_mutex_rename();   // leaves Windows internals untouched
    start_net_once();
    HANDLE focus = CreateThread(NULL, 0, focus_patch_thread, NULL, 0, NULL);
    if (focus) CloseHandle(focus);
    coop_log("install: multiplayer hooks ready during %s",
             retry ? "DirectInput8Create" : "process attach");
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        load_real_dinput8();
        if (!coop_init_paths(hinst)) return TRUE;
        InitializeCriticalSection(&g_log_lock);
        g_log_ready = TRUE;
        HANDLE h = CreateFileW(g_log_path,
                               GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        coop_log("DllMain PROCESS_ATTACH hinst=%p", hinst);
        coop_log("load_real_dinput8 done, g_real=%p DirectInput8Create=%p",
                 g_real_dinput8, g_real_DirectInput8Create);
        if (coop_read_config() && !try_start_coop(FALSE)) g_retry_coop = 1;
    }
    return TRUE;
}
