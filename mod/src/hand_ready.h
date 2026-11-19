// One barrier per completed hand, scoped to the committed lobby. Keep the
// release across the exchange epoch change that precedes the next HAND_START.
static unsigned hand_ready_hand, hand_ready_votes;

static int hand_ready_active_locked(void) {
    if (!g_lobby_mode || lb_phase != 3 || x_fault || !x_peer) return 0;
    if (g_role_is_guest) {
        return g_client_sock != INVALID_SOCKET && mp_id >= 2 && mp_id <= (unsigned)mp_count() + 1;
    }
    return !mp_stopped && lb_connected == lb_full();
}

static unsigned hand_ready_required(void) {
    return (lb_full() << 1) | 1;
}

static int hand_ready_publish_locked(void) {
    char line[112];
    sprintf_s(line, sizeof(line), "LDEAL_STATE id=%u rev=%u hand=%u votes=%u\n",
              lb_nonce, lb_revision, hand_ready_hand, hand_ready_votes);
    return exchange_send_locked(line);
}

static int hand_ready_vote_locked(unsigned hand, unsigned bit) {
    if (!hand || hand != hand_ready_hand) return 0;
    if (hand_ready_votes & bit) return 1;
    hand_ready_votes |= bit;
    return hand_ready_publish_locked();
}

static void hand_ready_handle_locked(const char *line) {
    unsigned id, revision, hand, votes;
    int end = 0;
    if (!hand_ready_active_locked()) return;
    if (!g_role_is_guest && mp_source >= 0 && mp_source < mp_count() &&
        mp_peers[mp_source].hello && mp_peers[mp_source].session_seat &&
        sscanf(line, "LDEAL_VOTE id=%u rev=%u hand=%u%n", &id, &revision, &hand, &end) == 3 &&
        end && !line[end] && id == lb_nonce && revision == lb_revision) {
        hand_ready_vote_locked(hand, 1u << (mp_source + 1));
    } else if (g_role_is_guest &&
               sscanf(line, "LDEAL_STATE id=%u rev=%u hand=%u votes=%u%n", &id, &revision, &hand, &votes, &end) == 4 &&
               end && !line[end] && id == lb_nonce && revision == lb_revision && hand && votes <= hand_ready_required()) {
        if (hand == hand_ready_hand && (votes & hand_ready_votes) == hand_ready_votes) {
            hand_ready_votes = votes;
        } else if (hand == hand_ready_hand + 1 && !votes &&
                   (!hand_ready_hand || hand_ready_votes == hand_ready_required())) {
            hand_ready_hand = hand;
            hand_ready_votes = 0;
        }
    }
}

static int __cdecl hand_ready_action(void *L) {
    const char *action = p_lua_tolstring(L, 1, NULL);
    const char *number = p_lua_tolstring(L, 2, NULL);
    unsigned long parsed_hand = 0;
    char *end = NULL;
    int ok = 0;
    if (number && number[0] >= '1' && number[0] <= '9') parsed_hand = strtoul(number, &end, 10);
    if (action && parsed_hand && parsed_hand <= 0x7fffffffu && end && !*end) {
        unsigned hand = (unsigned)parsed_hand;
        EnterCriticalSection(&g_net_lock);
        if (hand_ready_active_locked()) {
            if (!strcmp(action, "open") && !g_role_is_guest &&
                hand == hand_ready_hand + 1 && (!hand_ready_hand || hand_ready_votes == hand_ready_required())) {
                hand_ready_hand = hand;
                hand_ready_votes = 0;
                ok = hand_ready_publish_locked();
            } else if (!strcmp(action, "vote") && hand == hand_ready_hand) {
                if (g_role_is_guest) {
                    char line[112];
                    sprintf_s(line, sizeof(line), "LDEAL_VOTE id=%u rev=%u hand=%u\n", lb_nonce, lb_revision, hand);
                    ok = exchange_send_locked(line);
                } else {
                    ok = hand_ready_vote_locked(hand, 1);
                }
            }
        }
        LeaveCriticalSection(&g_net_lock);
    }
    p_lua_pushinteger(L, ok);
    return 1;
}

static int __cdecl hand_ready_status(void *L) {
    EnterCriticalSection(&g_net_lock);
    unsigned values[] = {hand_ready_active_locked(), hand_ready_hand,
                         hand_ready_votes, hand_ready_required()};
    LeaveCriticalSection(&g_net_lock);
    for (int i = 0; i < 4; ++i) p_lua_pushinteger(L, values[i]);
    return 4;
}
