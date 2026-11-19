// Test-only selected-exchange protocol. Call *_locked with g_net_lock held.
// Separate queue and epoch prevent stale menu/connection events being replayed.
#define X_CAP 256
#define X_DLG 96
#define X_NODE 64
#define X_CHORE 192
#define X_KEY 128
typedef struct {
    unsigned epoch, seq, kind;
    char dlg[X_DLG], node[X_NODE], chore[X_CHORE], key[X_KEY];
} ExchangeEvent;
static ExchangeEvent x_queue[X_CAP];
static unsigned x_head, x_tail, x_epoch, x_next_seq, x_received_seq;
static unsigned x_epoch_counter;
static unsigned x_connection_generation;
static int x_peer, x_local_ready, x_peer_ready, x_active, x_fault;
static int exchange_send_locked(const char *line);
static void seat_reservation_clear_locked(int connection);
static void seat_reservation_open_locked(void);
static void seat_reservation_connected_locked(void);

static void exchange_fault_locked(const char *reason) {
    int first = !x_fault;
    if (first) coop_log("exchange: FAULT %s epoch=%u seq=%u", reason, x_epoch, x_received_seq);
    x_fault = 1;
    x_active = 0;
    x_head = x_tail = 0;
    seat_reservation_clear_locked(0);
    if (first && x_peer && g_client_sock != INVALID_SOCKET) exchange_send_locked("XFAULT\n");
    if(g_lobby_mode && lb_phase) {
        lb_phase=4;if(!lb_error)lb_error=3;
        if(g_role_is_guest && g_client_sock!=INVALID_SOCKET)shutdown(g_client_sock,SD_BOTH);
    }
}

static int exchange_send_locked(const char *line) {
    if (g_two_guests && !g_role_is_guest) return mp_send_locked(-1,line);
    if (g_client_sock == INVALID_SOCKET) return 0;
    int length = (int)strlen(line), sent = 0;
    while (sent < length) {
        int n = send(g_client_sock, line + sent, length - sent, 0);
        if (n <= 0) { exchange_fault_locked("send_failed"); return 0; }
        sent += n;
    }
    return 1;
}

static void exchange_start_locked(void) {
    if (g_role_is_guest || !x_peer || !x_local_ready || !x_peer_ready || x_active || x_fault) return;
    char line[96];
    x_epoch = ++x_epoch_counter;
    x_next_seq = x_received_seq = 0;
    x_head = x_tail = 0;
    _snprintf_s(line, sizeof(line), _TRUNCATE, "XRESET epoch=%u\n", x_epoch);
    if (exchange_send_locked(line)) {
        x_active = 1;
        coop_log("exchange: session_start epoch=%u", x_epoch);
        seat_reservation_open_locked();
    }
}

static void exchange_connected_locked(void) {
    ++x_connection_generation;
    x_peer = x_peer_ready = x_active = x_fault = 0;
    x_epoch = x_next_seq = x_received_seq = 0;
    x_head = x_tail = 0;
    if (g_test_mode) {
        g_latest_deck[0] = 0;
        InterlockedExchange(&g_hand_start_pending, 0);
        DWORD timeout = 1000;
        setsockopt(g_client_sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
        exchange_send_locked(g_lobby_mode ? "XHELLO version=" PN2_LOBBY_PROTOCOL_TEXT "\n" : g_two_guests ? (g_presentation_sync ? (g_voice_sync?"XHELLO version=9\n":"XHELLO version=8\n") : "XHELLO version=7\n") : "XHELLO version=6\n");
    }
    seat_reservation_connected_locked();
}

static void exchange_disconnected_locked(void) {
    seat_reservation_clear_locked(1);
    if (g_test_mode) coop_log("exchange: disconnected; replay stopped");
    x_peer = x_peer_ready = x_active = 0;
    x_head = x_tail = 0;
    if (g_test_mode) {
        g_latest_deck[0] = 0;
        InterlockedExchange(&g_hand_start_pending, 0);
    }
}

static int exchange_token(const char *s, size_t cap) {
    if (!s || !s[0] || strlen(s) >= cap) return 0;
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c <= 32 || c >= 127 || c == '=') return 0;
    }
    return 1;
}

static int exchange_handle_line(const char *line) {
    if (line[0] != 'X') return 0;
    if (!g_test_mode) return 1;
    EnterCriticalSection(&g_net_lock);
    if (!strcmp(line, g_lobby_mode ? "XHELLO version=" PN2_LOBBY_PROTOCOL_TEXT : g_two_guests ? (g_presentation_sync ? (g_voice_sync?"XHELLO version=9":"XHELLO version=8") : "XHELLO version=7") : "XHELLO version=6")) {
        if (!x_peer) {
            x_peer = 1;
            coop_log("exchange: peer_hello version=%d",g_lobby_mode?PN2_LOBBY_PROTOCOL:g_two_guests?(g_presentation_sync?(g_voice_sync?9:8):7):6);
            exchange_send_locked(x_local_ready ? "XREADY active=1\n" : "XREADY active=0\n");
            exchange_start_locked();
        }
    } else if (!strcmp(line, "XFAULT")) {
        exchange_fault_locked("peer_reported_fault");
    } else if (!strcmp(line, "XREADY active=0") || !strcmp(line, "XREADY active=1")) {
        if (x_peer) {
            x_peer_ready = line[14] == '1';
            if (!x_peer_ready) { x_active = 0; x_head = x_tail = 0; seat_reservation_clear_locked(0); }
            coop_log("exchange: peer_ready=%d", x_peer_ready);
            exchange_start_locked();
        }
    } else if (!strncmp(line, "XRESET ", 7)) {
        unsigned epoch = 0; int end = 0;
        if (g_role_is_guest && x_peer && x_local_ready && x_peer_ready &&
            sscanf(line, "XRESET epoch=%u%n", &epoch, &end) == 1 && !line[end] && epoch > x_epoch && epoch <= 0x7fffffff) {
            x_epoch = epoch;
            x_received_seq = 0;
            x_head = x_tail = 0;
            x_active = 1;
            x_fault = 0;
            seat_reservation_clear_locked(0);
            coop_log("exchange: session_start epoch=%u", x_epoch);
        } else { exchange_fault_locked("invalid_reset"); }
    } else if (!strncmp(line, "XEVENT ", 7)) {
        ExchangeEvent item; int end = 0;
        memset(&item, 0, sizeof(item));
        int fields = sscanf(line, "XEVENT epoch=%u seq=%u kind=%u dlg=%95s node=%63s chore=%191s key=%127s%n",
                            &item.epoch, &item.seq, &item.kind, item.dlg, item.node, item.chore, item.key, &end);
        if (!g_role_is_guest || !x_active || x_fault) {
            coop_log("exchange: ignored_event inactive");
        } else if (fields != 7 || !end || line[end] || !item.epoch || item.epoch > 0x7fffffff ||
                   !item.seq || item.seq > 0x7fffffff || item.kind < 1 || item.kind > (g_voice_sync?11u:10u) ||
                   !exchange_token(item.dlg, X_DLG) || !exchange_token(item.node, X_NODE) ||
                   !exchange_token(item.chore, X_CHORE) || !exchange_token(item.key, X_KEY)) {
            exchange_fault_locked("malformed_event");
        } else if (item.epoch < x_epoch) {
            coop_log("exchange: stale_event epoch=%u", item.epoch);
        } else if (item.epoch != x_epoch) {
            exchange_fault_locked("unexpected_epoch");
        } else if (item.seq <= x_received_seq) {
            coop_log("exchange: duplicate_event seq=%u", item.seq);
        } else if (item.seq != x_received_seq + 1) {
            exchange_fault_locked("sequence_gap");
        } else if ((x_tail + 1) % X_CAP == x_head) {
            exchange_fault_locked("queue_overflow");
        } else {
            x_queue[x_tail] = item;
            x_tail = (x_tail + 1) % X_CAP;
            x_received_seq = item.seq;
            coop_log("exchange: receive epoch=%u seq=%u kind=%u dlg=%s node=%s", item.epoch, item.seq, item.kind, item.dlg, item.node);
        }
    } else { exchange_fault_locked("unknown_protocol"); }
    LeaveCriticalSection(&g_net_lock);
    return 1;
}

static int __cdecl exchange_test_mode(void *L) {
    if (!g_test_mode) return 0;
    p_lua_pushinteger(L, 1);
    return 1;
}

// Connection identity/health is distinct from scene/hand readiness epochs.
// Used only to let the first hand await normal menu-to-table readiness changes.
static int __cdecl exchange_connection(void *L) {
    EnterCriticalSection(&g_net_lock);
    unsigned generation=x_connection_generation;
    int connected=x_peer && g_client_sock!=INVALID_SOCKET, fault=x_fault;
    LeaveCriticalSection(&g_net_lock);
    p_lua_pushinteger(L,generation);p_lua_pushinteger(L,connected);p_lua_pushinteger(L,fault);
    return 3;
}

// Called from the game's Lua thread, even when not ready. Never calls into Lua
// from the socket thread. Failure requires a new connection/readiness session.
static int __cdecl exchange_ready(void *L) {
    const char *s = p_lua_tolstring(L, 1, NULL);
    int ready = s && !strcmp(s, "1");
    EnterCriticalSection(&g_net_lock);
    if (g_lobby_mode && lb_phase == 5) ready = 0;
    if (ready != x_local_ready) {
        x_local_ready = ready;
        x_active = 0;
        x_head = x_tail = 0;
        seat_reservation_clear_locked(0);
        if (!ready) x_fault = 0;
        coop_log("exchange: local_ready=%d", ready);
        if (x_peer) exchange_send_locked(ready ? "XREADY active=1\n" : "XREADY active=0\n");
        exchange_start_locked();
    }
    int active = g_test_mode && x_active && !x_fault && x_peer && x_peer_ready && x_local_ready;
    unsigned epoch = x_epoch;
    LeaveCriticalSection(&g_net_lock);
    p_lua_pushinteger(L, active);
    p_lua_pushinteger(L, epoch);
    return 2;
}

static int __cdecl exchange_emit(void *L) {
    const char *kind = p_lua_tolstring(L, 1, NULL);
    const char *dlg = p_lua_tolstring(L, 2, NULL);
    const char *node = p_lua_tolstring(L, 3, NULL);
    const char *chore = p_lua_tolstring(L, 4, NULL);
    const char *key = p_lua_tolstring(L, 5, NULL);
    unsigned tag = kind && !strcmp(kind, "begin") ? 1 : kind && !strcmp(kind, "end") ? 2 :
                   kind && !strcmp(kind, "gate_enter") ? 3 : kind && !strcmp(kind, "gate_release") ? 4 :
                   kind && !strcmp(kind, "show_node") ? 5 : kind && !strcmp(kind, "show_done") ? 6 :
                   kind && !strcmp(kind, "reveal") ? 7 :
                   kind && !strcmp(kind, "bust_chore") ? 8 : kind && !strcmp(kind, "bust_state") ? 9 :
                   kind && !strcmp(kind, "bust_done") ? 10 :
                   g_voice_sync && kind && !strcmp(kind,"voice_begin") ? 11 : 0;
    int sent = 0;
    EnterCriticalSection(&g_net_lock);
    if (g_test_mode && !g_role_is_guest && x_active && !x_fault) {
        if (x_next_seq >= 0x7fffffff || !tag || !exchange_token(dlg, X_DLG) || !exchange_token(node, X_NODE) ||
            !exchange_token(chore, X_CHORE) || !exchange_token(key, X_KEY)) {
            exchange_fault_locked("invalid_host_event");
        } else {
            char line[768];
            unsigned seq = ++x_next_seq;
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                "XEVENT epoch=%u seq=%u kind=%u dlg=%s node=%s chore=%s key=%s\n",
                x_epoch, seq, tag, dlg, node, chore, key);
            sent = exchange_send_locked(line);
            coop_log("exchange: emit epoch=%u seq=%u kind=%u dlg=%s node=%s chore=%s key=%s",
                     x_epoch, seq, tag, dlg, node, chore, key);
        }
    }
    LeaveCriticalSection(&g_net_lock);
    p_lua_pushinteger(L, sent);
    return 1;
}

static int __cdecl exchange_new_hand(void *L) {
    (void)L;
    EnterCriticalSection(&g_net_lock);
    if (g_test_mode && !g_role_is_guest) {
        x_active = 0;
        exchange_start_locked();
    }
    LeaveCriticalSection(&g_net_lock);
    return 0;
}

static int __cdecl exchange_take(void *L) {
    ExchangeEvent item; int found = 0;
    EnterCriticalSection(&g_net_lock);
    if (x_active && !x_fault && x_head != x_tail) {
        item = x_queue[x_head];
        x_head = (x_head + 1) % X_CAP;
        found = 1;
    }
    LeaveCriticalSection(&g_net_lock);
    if (!found) return 0;
    p_lua_pushinteger(L, item.kind);
    p_lua_pushinteger(L, item.epoch);
    p_lua_pushinteger(L, item.seq);
    p_lua_pushstring(L, item.dlg);
    p_lua_pushstring(L, item.node);
    p_lua_pushstring(L, item.chore);
    p_lua_pushstring(L, item.key);
    return 7;
}

static int __cdecl exchange_abort(void *L) {
    EnterCriticalSection(&g_net_lock);
    exchange_fault_locked("lua_replay_or_lookup_failed");
    LeaveCriticalSection(&g_net_lock);
    return 0;
}

static void exchange_register_lua(void *L) {
    const char *names[] = { "COOP_TestMode", "COOP_ExchangeReady", "COOP_ExchangeEmit", "COOP_ExchangeTake", "COOP_ExchangeAbort", "COOP_ExchangeNewHand", "COOP_ExchangeConnection" };
    PFN_lua_CFunction funcs[] = { exchange_test_mode, exchange_ready, exchange_emit, exchange_take, exchange_abort, exchange_new_hand, exchange_connection };
    for (int i = 0; i < 7; ++i) {
        p_lua_pushstring(L, names[i]);
        p_lua_pushcclosure(L, funcs[i], 0);
        p_lua_rawset(L, LUA_GLOBALSINDEX);
    }
}
