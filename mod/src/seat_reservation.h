// Phase-5 preflight only: reservations do NOT change engine controllers.
// All access is under g_net_lock; the socket thread never calls Lua.
// Peer 1 = host, peer 2 = the one accepted guest socket, 0 = AI.
static int g_seat_probe;                 // also requires test_mode=1
static int g_seat_control, sr_control_peer;
static int g_seat_picker;               // guest opt-in, defaults off
static unsigned sr_picker_choice;       // fixed until process restart
static void seat_control_reset_locked(void);
static unsigned g_requested_seat = 1;   // 1 Brock, 2 Claptrap, 3 Ash, 4 Sam
static int sr_peer, sr_confirmed;
static unsigned sr_epoch, sr_revision, sr_reserved;
static const char *sr_names[] = {"none", "Brock", "Claptrap", "Ash", "Sam"};

static int seat_reservation_numbers_valid(const char *line) {
    if (strlen(line) >= 160) return 0;
    // sscanf %u accepts signs and overflow; reject those before conversion.
    for (const char *p=line; *p; ++p) {
        if (*p=='+' || *p=='-') return 0;
        if (*p=='=' && p[1]>='0' && p[1]<='9') {
            unsigned n=0;
            ++p;
            if (*p=='0' && p[1]>='0' && p[1]<='9') return 0;
            while (*p>='0' && *p<='9') {
                unsigned digit=(unsigned)(*p-'0');
                if (n > (0x7fffffffU-digit)/10) return 0;
                n=n*10+digit; ++p;
            }
            if (!*p) break;
        }
    }
    return 1;
}

static void seat_reservation_connected_locked(void) {
    seat_reservation_clear_locked(1);
    if (g_two_guests) return;
    if (g_test_mode && g_seat_probe) exchange_send_locked(g_seat_control ? "SHELLO version=2\n" : "SHELLO version=1\n");
}

static void seat_reservation_clear_locked(int connection) {
    if (g_two_guests) mp_clear_locked(connection);
    if (sr_epoch || sr_reserved)
        coop_log("seat: cleared epoch=%u reserved=%u control_unchanged=1", sr_epoch, sr_reserved);
    sr_epoch = sr_revision = sr_reserved = 0;
    sr_confirmed = 0;
    seat_control_reset_locked();
    if (connection) sr_peer = sr_control_peer = 0;
}

static void seat_reservation_open_locked(void) {
    if (g_two_guests) {mp_open_locked();return;}
    if (!g_test_mode || !g_seat_probe || g_role_is_guest || !sr_peer || !x_active || x_fault || sr_epoch == x_epoch) return;
    seat_reservation_clear_locked(0);
    sr_epoch = x_epoch; sr_revision = 1;
    char line[128];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "SOPEN epoch=%u revision=1\n", sr_epoch);
    if (exchange_send_locked(line))
        coop_log("seat: open epoch=%u revision=1 mode=reservation_only", sr_epoch);
}

static int seat_reservation_roster_locked(void) {
    char line[160];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "SROSTER epoch=%u revision=%u reserved=%u peer=2 control=ai\n", sr_epoch, sr_revision, sr_reserved);
    return exchange_send_locked(line);
}

static void seat_reservation_reject_locked(const char *reason) {
    char line[160];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "SREJECT epoch=%u reason=%s\n", x_epoch, reason);
    exchange_send_locked(line);
    coop_log("seat: reject reason=%s epoch=%u", reason, x_epoch);
}

static int seat_picker_enabled(void) {
    return g_test_mode && g_seat_probe && g_seat_control && g_role_is_guest && g_seat_picker;
}

static int seat_reservation_request_locked(void) {
    if (g_two_guests) return mp_request_locked();
    if (!g_role_is_guest || !sr_peer || !x_active || x_fault || sr_epoch!=x_epoch ||
        sr_revision!=1 || sr_reserved || (seat_picker_enabled() && !sr_picker_choice)) return 0;
    char request[128];
    _snprintf_s(request,sizeof(request),_TRUNCATE,"SCLAIM epoch=%u revision=1 seat=%u\n",sr_epoch,g_requested_seat);
    if (!exchange_send_locked(request)) return 0;
    coop_log("seat: requested epoch=%u seat=%u mode=reservation_only",sr_epoch,g_requested_seat);
    return 1;
}

static int seat_reservation_handle_line(const char *line) {
    if (line[0] != 'S' || (strncmp(line,"SHELLO",6) && strncmp(line,"SOPEN",5) &&
        strncmp(line,"SCLAIM",6) && strncmp(line,"SROSTER",7) && strncmp(line,"SACK",4) && strncmp(line,"SREJECT",7))) return 0;
    if (!g_test_mode || !g_seat_probe) return 1;
    EnterCriticalSection(&g_net_lock);
    unsigned epoch=0, revision=0, seat=0; int end=0;
    if (!seat_reservation_numbers_valid(line)) {
        coop_log("seat: ignored malformed_numbers");
    } else if (!strcmp(line,"SHELLO version=1") || !strcmp(line,"SHELLO version=2")) {
        sr_peer=1;
        sr_control_peer=!strcmp(line,"SHELLO version=2");
        seat_reservation_open_locked();
    } else if (!sr_peer || !x_active || x_fault) {
        coop_log("seat: ignored inactive_or_unnegotiated");
    } else if (g_role_is_guest && sscanf(line,"SOPEN epoch=%u revision=1%n",&epoch,&end)==1 && end && !line[end]) {
        if (epoch != x_epoch || (sr_epoch && epoch <= sr_epoch)) {
            coop_log("seat: ignored stale_open epoch=%u",epoch);
        } else {
            seat_reservation_clear_locked(0);
            sr_epoch=epoch; sr_revision=1;
            seat_reservation_request_locked();
        }
    } else if (!g_role_is_guest && sscanf(line,"SCLAIM epoch=%u revision=%u seat=%u%n",&epoch,&revision,&seat,&end)==3 && end && !line[end]) {
        if (!sr_epoch || epoch != x_epoch || epoch != sr_epoch) seat_reservation_reject_locked("stale_epoch");
        else if (seat < 1 || seat > 4) seat_reservation_reject_locked("invalid_seat");
        // Exact duplicate requests are idempotent; a new reservation needs the current revision.
        else if (sr_reserved == seat && sr_revision == 2 && revision == 1) seat_reservation_roster_locked();
        else if (revision != sr_revision) seat_reservation_reject_locked("stale_revision");
        else if (sr_reserved) seat_reservation_reject_locked("peer_already_reserved");
        else {
            sr_reserved=seat; ++sr_revision;
            if (seat_reservation_roster_locked())
                coop_log("seat: reserved epoch=%u revision=%u seat=%u name=%s peer=2 control=ai player_peer=1",sr_epoch,sr_revision,seat,sr_names[seat]);
        }
    } else if (g_role_is_guest && sscanf(line,"SROSTER epoch=%u revision=%u reserved=%u peer=2 control=ai%n",&epoch,&revision,&seat,&end)==3 && end && !line[end]) {
        if ((seat_picker_enabled() && !sr_picker_choice) || !sr_epoch || epoch != x_epoch || epoch != sr_epoch || revision != 2 || seat < 1 || seat > 4 || seat != g_requested_seat || (sr_reserved && sr_reserved != seat)) {
            coop_log("seat: ignored invalid_roster epoch=%u",epoch);
        } else {
            sr_revision=revision; sr_reserved=seat; sr_confirmed=1;
            char ack[128];
            _snprintf_s(ack,sizeof(ack),_TRUNCATE,"SACK epoch=%u revision=%u seat=%u\n",epoch,revision,seat);
            if (exchange_send_locked(ack))
                coop_log("seat: roster_applied epoch=%u revision=%u seat=%u name=%s peer=2 control=ai player_peer=1",epoch,revision,seat,sr_names[seat]);
        }
    } else if (!g_role_is_guest && sscanf(line,"SACK epoch=%u revision=%u seat=%u%n",&epoch,&revision,&seat,&end)==3 && end && !line[end]) {
        if (sr_epoch && epoch==x_epoch && epoch==sr_epoch && revision==sr_revision && sr_reserved && seat==sr_reserved) {
            sr_confirmed=1;
            coop_log("seat: confirmed epoch=%u revision=%u seat=%u control=ai",epoch,revision,seat);
        } else coop_log("seat: ignored invalid_ack");
    } else if (g_role_is_guest && !strncmp(line,"SREJECT ",8)) {
        coop_log("seat: request_rejected (reservation only)");
    } else coop_log("seat: ignored malformed_or_wrong_direction");
    LeaveCriticalSection(&g_net_lock);
    return 1;
}
