// The committed lobby owns this data across the engine's Lua reload.
static unsigned mr_places[5], mr_finished, mr_rematch, mr_lobby;
static int mr_detached;
static void coop_reset_install_cache(void);
static void lb_lock_locked(void);

static void mr_clear_locked(void) {
    memset(mr_places, 0, sizeof(mr_places));
    mr_finished = mr_rematch = mr_lobby = 0;
    mr_detached = 0;
}

static int mr_valid_places(const unsigned places[5]) {
    unsigned counts[6] = {0};
    unsigned seat_count=lb_cast_only?4:5;
    if(lb_cast_only && places[4])return 0;
    for (unsigned i = 0; i < seat_count; ++i) {
        if (places[i] < 1 || places[i] > seat_count) return 0;
        ++counts[places[i]];
    }
    if (counts[1] != 1) return 0;
    unsigned expected = 1;
    for (unsigned place = 1; place <= 5; ++place) {
        if (counts[place]) {
            if (place != expected) return 0;
            expected += counts[place];
        }
    }
    return 1;
}

static void mr_stop_gameplay_locked(void) {
    x_active = x_local_ready = x_peer_ready = 0;
    x_head = x_tail = x_next_seq = x_received_seq = 0;
    for (int i = 0; i < mp_count(); ++i) mp_peers[i].ready = 0;
    seat_reservation_clear_locked(0);
    hand_ready_hand = hand_ready_votes = 0;
    g_latest_deck[0] = 0;
    g_q_head = g_q_tail = g_chore_q_head = g_chore_q_tail = 0;
    InterlockedExchange(&g_hand_start_pending, 0);
    InterlockedExchange(&g_banter_pending, 0);
    InterlockedExchange(&g_gameplay_active, 0);
}

static void mr_enter_results_locked(void) {
    mr_stop_gameplay_locked();
    lb_phase = 5;
    coop_log("match: results revision=%u places=%u,%u,%u,%u,%u", lb_revision,
             mr_places[0], mr_places[1], mr_places[2], mr_places[3], mr_places[4]);
}

static void mr_try_show_locked(void) {
    if (!mr_places[0] || mr_finished != hand_ready_required()) return;
    char line[150];
    sprintf_s(line, sizeof(line), "LRESULT_SHOW id=%u rev=%u p1=%u p2=%u p3=%u p4=%u p5=%u\n",
              lb_nonce, lb_revision, mr_places[0], mr_places[1], mr_places[2], mr_places[3], mr_places[4]);
    if (exchange_send_locked(line)) mr_enter_results_locked();
}

static void mr_enter_lobby_locked(unsigned choice) {
    mr_clear_locked();
    ++lb_revision;
    lb_phase = 1;
    lb_ready = lb_lock_acks = lb_error = 0;
    if (choice == 2) {
        memset(lb_seats, 0, sizeof(lb_seats));
        lb_host_seat=0;
        for (int i = 0; i < mp_count(); ++i) mp_peers[i].session_seat = 0;
        sr_picker_choice = 0;
    }
    coop_log("match: transition choice=%u revision=%u", choice, lb_revision);
    if (!g_role_is_guest) {
        if (choice == 1) lb_ready = hand_ready_required();
        lb_state_locked();
        lb_lock_locked();
    }
}

static int mr_vote_locked(unsigned bit, unsigned choice) {
    if (choice > 2) return 0;
    mr_rematch = (mr_rematch & ~bit) | (choice == 1 ? bit : 0);
    mr_lobby = (mr_lobby & ~bit) | (choice == 2 ? bit : 0);
    coop_log("match: votes revision=%u rematch=%u lobby=%u", lb_revision, mr_rematch, mr_lobby);
    char line[128];
    unsigned full = hand_ready_required();
    unsigned accepted = mr_rematch == full ? 1 : mr_lobby == full ? 2 : 0;
    if (accepted) {
        sprintf_s(line, sizeof(line), "LRESULT_NEXT id=%u rev=%u mode=%u\n", lb_nonce, lb_revision, accepted);
        if (!exchange_send_locked(line)) return 0;
        mr_enter_lobby_locked(accepted);
        return 1;
    }
    sprintf_s(line, sizeof(line), "LRESULT_VOTES id=%u rev=%u rematch=%u lobby=%u\n",
              lb_nonce, lb_revision, mr_rematch, mr_lobby);
    return exchange_send_locked(line);
}

static void mr_handle_locked(const char *line) {
    unsigned id, rev, a, b, places[5];
    int end = 0;
    if (x_fault || !x_peer) return;
    if (!g_role_is_guest) {
        if (mp_source < 0 || mp_source >= mp_count() || !mp_peers[mp_source].hello ||
            !mp_peers[mp_source].session_seat) return;
        unsigned bit = 1u << (mp_source + 1);
        if (lb_phase == 3 && sscanf(line, "LRESULT_DONE id=%u rev=%u%n", &id, &rev, &end) == 2 &&
            end && !line[end] && id == lb_nonce && rev == lb_revision) {
            mr_finished |= bit;
            mr_try_show_locked();
        } else if (lb_phase == 5 && sscanf(line, "LRESULT_VOTE id=%u rev=%u mode=%u%n", &id, &rev, &a, &end) == 3 &&
                   end && !line[end] && id == lb_nonce && rev == lb_revision) {
            mr_vote_locked(bit, a);
        }
    } else if (lb_phase == 3 &&
               sscanf(line, "LRESULT_SHOW id=%u rev=%u p1=%u p2=%u p3=%u p4=%u p5=%u%n",
                      &id, &rev, &places[0], &places[1], &places[2], &places[3], &places[4], &end) == 7 &&
               end && !line[end] && id == lb_nonce && rev == lb_revision && mr_valid_places(places) && mr_finished) {
        memcpy(mr_places, places, sizeof(mr_places));
        mr_enter_results_locked();
    } else if (lb_phase == 5 && sscanf(line, "LRESULT_VOTES id=%u rev=%u rematch=%u lobby=%u%n",
                                      &id, &rev, &a, &b, &end) == 4 &&
               end && !line[end] && id == lb_nonce && rev == lb_revision &&
               !(a & b) && (a | b) <= hand_ready_required()) {
        mr_rematch = a;
        mr_lobby = b;
    } else if (lb_phase == 5 && sscanf(line, "LRESULT_NEXT id=%u rev=%u mode=%u%n", &id, &rev, &a, &end) == 3 &&
               end && !line[end] && id == lb_nonce && rev == lb_revision && (a == 1 || a == 2) && mr_detached) {
        mr_enter_lobby_locked(a);
    }
}

static int __cdecl mr_action(void *L) {
    const char *action = p_lua_tolstring(L, 1, NULL);
    const char *arg = p_lua_tolstring(L, 2, NULL);
    int ok = 0;
    EnterCriticalSection(&g_net_lock);
    if (action && g_lobby_mode && x_peer && !x_fault) {
        if (lb_phase == 3 && !strcmp(action, "finish") && !mr_finished && g_role_is_guest) {
            char line[96];
            sprintf_s(line, sizeof(line), "LRESULT_DONE id=%u rev=%u\n", lb_nonce, lb_revision);
            ok = exchange_send_locked(line);
            if (ok) mr_finished = 1;
        } else if (lb_phase == 3 && !strcmp(action, "finish") && !g_role_is_guest && !(mr_finished & 1) &&
                   arg && strlen(arg) == 5) {
            unsigned places[5];
            for (int i = 0; i < 5; ++i) places[i] = (unsigned)(arg[i] - '0');
            if (mr_valid_places(places)) {
                memcpy(mr_places, places, sizeof(mr_places));
                mr_finished |= 1;
                mr_try_show_locked();
                ok = !x_fault;
            }
        } else if (lb_phase == 5 && !strcmp(action, "detach") && !mr_detached) {
            mr_detached = 1;
            coop_reset_install_cache();
            ok = 1;
        } else if (lb_phase == 5 && mr_detached && !strcmp(action, "vote") &&
                   arg && arg[0] >= '0' && arg[0] <= '2' && !arg[1]) {
            unsigned choice = arg[0] - '0';
            if (g_role_is_guest) {
                char line[100];
                sprintf_s(line, sizeof(line), "LRESULT_VOTE id=%u rev=%u mode=%u\n", lb_nonce, lb_revision, choice);
                ok = exchange_send_locked(line);
            } else ok = mr_vote_locked(1, choice);
        }
    }
    LeaveCriticalSection(&g_net_lock);
    p_lua_pushinteger(L, ok);
    return 1;
}

static int __cdecl mr_status(void *L) {
    EnterCriticalSection(&g_net_lock);
    unsigned values[] = {mr_rematch, mr_lobby, mr_places[0], mr_places[1], mr_places[2], mr_places[3], mr_places[4]};
    LeaveCriticalSection(&g_net_lock);
    for (int i = 0; i < 7; ++i) p_lua_pushinteger(L, values[i]);
    return 7;
}
