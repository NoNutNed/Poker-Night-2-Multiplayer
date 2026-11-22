// Redirect only the standard banter sequence's child at the graph dispatch
// call. Sequence initialization can happen several times for one conversation.
#include <bcrypt.h>
#include <intrin.h>
#include <stddef.h>
#include "banter_entries.h"
#pragma comment(lib, "bcrypt.lib")

#define BANTER_POOL_ID UINT64_C(3419402991008094992)
#define BANTER_POOL_SIZE ARRAYSIZE(banter_entries)
#define VA_BANTER_VTABLE 0x96742c
#define VA_BANTER_GET_CHILD 0x7aa730
#define VA_BANTER_DISPATCH_CALL 0x7ac3e9

typedef struct {
    const void *vtable;
    uint8_t pad_to_asset[0x48];
    const uint8_t *const *asset;
    uint8_t pad_to_cursor[0x34];
    int cursor;
    uint8_t pad_to_exhausted[4];
    uint8_t exhausted;
    uint8_t pad_to_order[0x17];
    int count;
    int capacity;
    const uint8_t **order;
} BanterSequence;

typedef char BanterSequenceLayout[
    offsetof(BanterSequence, asset) == 0x4c &&
    offsetof(BanterSequence, cursor) == 0x84 &&
    offsetof(BanterSequence, exhausted) == 0x8c &&
    offsetof(BanterSequence, count) == 0xa4 &&
    offsetof(BanterSequence, order) == 0xac ? 1 : -1];

// Native thiscall takes the receiver in ECX and pops its one stack argument.
// The unused fastcall EDX argument leaves that ABI intact.
typedef const uint8_t **(__fastcall *BanterGetChild)(BanterSequence *, void *, const uint8_t **);
static BanterGetChild banter_get_child_original;
static const void *banter_vtable, *banter_dispatch_return;
static BOOL banter_hook_ready;

static unsigned banter_request;
static BOOL banter_request_open, banter_confirmed;
static unsigned banter_dispatch_calls;
static int banter_candidate = -1;
static BOOL banter_heard[BANTER_POOL_SIZE];
static unsigned banter_heard_count;
static int banter_last_heard = -1;

static uint64_t banter_symbol(const uint8_t *bytes) {
    uint64_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static BOOL banter_host_active(void) {
    return banter_hook_ready && g_lobby_mode && !g_role_is_guest &&
           InterlockedCompareExchange(&g_session_preferences_dirty, 0, 0);
}

static BOOL banter_is_main(const BanterSequence *sequence) {
    // 0x7AA640 keys the sequence state with this authored node ID.
    return sequence->vtable == banter_vtable && sequence->asset && *sequence->asset &&
           banter_symbol(*sequence->asset + 0x10) == BANTER_POOL_ID;
}

static int banter_pick(uint32_t random, uint64_t stock) {
    int candidates[BANTER_POOL_SIZE];
    unsigned count = 0;
    int stock_index = -1;
    const BOOL full_cycle = banter_heard_count == BANTER_POOL_SIZE;
    for (int i = 0; i < BANTER_POOL_SIZE; ++i) {
        if (full_cycle ? i == banter_last_heard : banter_heard[i]) continue;
        if (banter_entries[i].link == stock) stock_index = (int)count;
        candidates[count++] = i;
    }
    // Try a different entry from the engine's restored choice whenever there
    // is another unheard candidate. Every authored entry can still play.
    if (stock_index >= 0 && count > 1) candidates[stock_index] = candidates[--count];
    return count ? candidates[((uint64_t)random * count) >> 32] : -1;
}

static void banter_begin(void) {
    ++banter_request;
    banter_request_open = TRUE;
    banter_confirmed = FALSE;
    banter_candidate = -1;
    banter_dispatch_calls = 0;
}

static void banter_accept(uint64_t root) {
    if (banter_confirmed) return;
    banter_confirmed = TRUE;
    for (int i = 0; i < BANTER_POOL_SIZE; ++i) {
        if (banter_entries[i].root != root) continue;
        if (banter_heard_count == BANTER_POOL_SIZE) {
            memset(banter_heard, 0, sizeof(banter_heard));
            banter_heard_count = 0;
        }
        if (!banter_heard[i]) {
            banter_heard[i] = TRUE;
            ++banter_heard_count;
        }
        banter_last_heard = i;
        break;
    }
    coop_log("COOP_BANTER request=%u accepted_root=%" PRIu64 " dispatches=%u result=%s heard=%u",
             banter_request, root, banter_dispatch_calls,
             banter_candidate < 0 ? "stock" :
             banter_entries[banter_candidate].root == root ? "selected" : "engine_choice",
             banter_heard_count);
}

static const uint8_t **banter_dispatch(BanterSequence *sequence, const uint8_t **child) {
    ++banter_dispatch_calls;
    const int cursor = sequence->cursor;
    if (sequence->exhausted || sequence->count != BANTER_POOL_SIZE ||
        sequence->capacity < sequence->count || !sequence->order ||
        cursor < 0 || cursor >= sequence->count || !sequence->order[cursor]) {
        if (banter_dispatch_calls == 1)
            coop_log("COOP_BANTER request=%u dispatch=stock_layout cursor=%d count=%d",
                     banter_request, cursor, sequence->count);
        return banter_get_child_original(sequence, NULL, child);
    }

    const uint8_t *stock = sequence->order[cursor];
    const uint64_t stock_link = banter_symbol(stock + 0x50);
    if (banter_candidate < 0) {
        uint32_t random;
        if (BCryptGenRandom(NULL, (PUCHAR)&random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return banter_get_child_original(sequence, NULL, child);
        banter_candidate = banter_pick(random, stock_link);
        if (banter_candidate < 0) return banter_get_child_original(sequence, NULL, child);
        coop_log("COOP_BANTER request=%u stock_cursor=%d stock_entry=%" PRIu64
                 " selected_entry=%" PRIu64 " expected_root=%" PRIu64,
                 banter_request, cursor, stock_link, banter_entries[banter_candidate].link,
                 banter_entries[banter_candidate].root);
    }

    const uint64_t chosen = banter_entries[banter_candidate].link;
    for (int slot = 0; slot < sequence->count; ++slot) {
        const uint8_t *entry = sequence->order[slot];
        if (!entry || banter_symbol(entry + 0x50) != chosen) continue;
        // Borrow the chosen slot just for the native smart-pointer copy.
        // Dispatch and any runtime-state store run with the stock order back.
        sequence->order[cursor] = entry;
        const uint8_t **result = banter_get_child_original(sequence, NULL, child);
        sequence->order[cursor] = stock;
        return result;
    }
    return banter_get_child_original(sequence, NULL, child);
}

static const uint8_t **banter_get_child(BanterSequence *sequence, const uint8_t **child, const void *caller) {
    if (caller == banter_dispatch_return && banter_request_open &&
        banter_host_active() && banter_is_main(sequence)) return banter_dispatch(sequence, child);
    return banter_get_child_original(sequence, NULL, child);
}

static __declspec(noinline) const uint8_t **__fastcall hook_banter_get_child(
    BanterSequence *sequence, void *unused, const uint8_t **child) {
    (void)unused;
    return banter_get_child(sequence, child, _ReturnAddress());
}

static int __cdecl coop_banter_selection(void *L) {
    if (!banter_host_active()) return 0;
    const char *operation = p_lua_tolstring(L, 1, NULL);
    if (!operation) return 0;
    if (!strcmp(operation, "begin")) {
        banter_begin();
    } else if (banter_request_open && !strcmp(operation, "end")) {
        if (!banter_confirmed)
            coop_log("COOP_BANTER request=%u unconfirmed dispatches=%u",
                     banter_request, banter_dispatch_calls);
        banter_request_open = FALSE;
    } else if (banter_request_open && !strcmp(operation, "accept")) {
        const char *id = p_lua_tolstring(L, 2, NULL);
        if (id && !strncmp(id, "dlg_id: ", 8)) {
            char *end;
            const uint64_t root = _strtoui64(id + 8, &end, 10);
            if (root && !*end) banter_accept(root);
        }
    }
    return 0;
}

static void install_banter_hook(HMODULE exe, uint8_t *trampoline) {
    static const uint8_t getter_bytes[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
    static const uint8_t dispatch_bytes[] = {0xe8, 0x42, 0xe3, 0xff, 0xff, 0x8b, 0x4d, 0x1c};
    if (!matches_game_code(exe, VA_BANTER_GET_CHILD, getter_bytes, sizeof(getter_bytes)) ||
        !matches_game_code(exe, VA_BANTER_DISPATCH_CALL, dispatch_bytes, sizeof(dispatch_bytes))) {
        coop_log("COOP_BANTER native dispatch layout unavailable");
        return;
    }
    banter_vtable = resolve_va(VA_BANTER_VTABLE);
    banter_dispatch_return = resolve_va(VA_BANTER_DISPATCH_CALL + 5);
    banter_get_child_original = (BanterGetChild)trampoline;
    banter_hook_ready =
        install_trampoline(resolve_va(VA_BANTER_GET_CHILD), hook_banter_get_child, trampoline);
    coop_log("COOP_BANTER native dispatch %s", banter_hook_ready ? "ready" : "installation unavailable");
}
