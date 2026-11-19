// Numeric intent/result channel. Lua owns turn legality and engine mutation.
// Host accepts only intents from its current socket; guest accepts turn/results.
#define SC_CAP 32
typedef struct { unsigned epoch, revision, seat, hand, street, turn, kind, action, amount; } SeatControlEvent;
static SeatControlEvent sc_queue[SC_CAP];
static unsigned sc_head, sc_tail;
static void seat_control_reset_locked(void) { sc_head=sc_tail=0; }
static int seat_control_enabled_locked(void) {
    if (g_two_guests) return g_test_mode && g_seat_probe && g_seat_control &&
        sr_confirmed && mp_epoch==x_epoch && sr_revision==3 && x_active && !x_fault;
    return g_test_mode && g_seat_probe && g_seat_control && sr_control_peer &&
        sr_confirmed && sr_epoch==x_epoch && sr_revision==2 && sr_reserved>=1 && sr_reserved<=4 && x_active && !x_fault;
}
static int seat_control_parse(const char *line, SeatControlEvent *e) {
    int end=0;
    memset(e,0,sizeof(*e));
    return seat_reservation_numbers_valid(line) &&
        sscanf(line,"CSEAT epoch=%u rev=%u seat=%u hand=%u street=%u turn=%u kind=%u action=%u amount=%u%n",
        &e->epoch,&e->revision,&e->seat,&e->hand,&e->street,&e->turn,&e->kind,&e->action,&e->amount,&end)==9 &&
        end && !line[end] && e->epoch && e->hand && e->turn && e->street>=1 && e->street<=4 &&
        e->kind>=1 && e->kind<=4 && e->action<=4;
}
static int seat_control_scope_locked(const SeatControlEvent *e) {
    if (g_two_guests) return mp_scope_locked(e->epoch,e->revision,e->seat);
    return seat_control_enabled_locked() && e->epoch==x_epoch && e->revision==sr_revision && e->seat==sr_reserved;
}
static int seat_control_handle_line(const char *line) {
    if (strncmp(line,"CSEAT",5)) return 0;
    if (!g_test_mode || !g_seat_control) return 1;
    SeatControlEvent e;
    EnterCriticalSection(&g_net_lock);
    if (!seat_control_parse(line,&e) || !seat_control_scope_locked(&e) ||
        (g_role_is_guest ? e.kind==2 : e.kind!=2) ||
        (g_two_guests && !g_role_is_guest && (mp_source<0 || mp_source>=mp_count() || mp_seats[mp_source]!=e.seat))) {
        coop_log("control: reject invalid_direction_or_scope");
    } else if ((sc_tail+1)%SC_CAP==sc_head) exchange_fault_locked("control_queue_overflow");
    else {
        sc_queue[sc_tail]=e; sc_tail=(sc_tail+1)%SC_CAP;
        coop_log("control: receive epoch=%u hand=%u street=%u turn=%u kind=%u seat=%u action=%u amount=%u",
            e.epoch,e.hand,e.street,e.turn,e.kind,e.seat,e.action,e.amount);
    }
    LeaveCriticalSection(&g_net_lock);
    return 1;
}
static int __cdecl seat_control_options(void *L) {
    EnterCriticalSection(&g_net_lock);
    unsigned enabled=g_test_mode && g_seat_probe && g_seat_control, seat=seat_control_enabled_locked()?sr_reserved:0;
    unsigned epoch=x_epoch, revision=sr_revision, capable=sr_control_peer;
    LeaveCriticalSection(&g_net_lock);
    p_lua_pushinteger(L,enabled); p_lua_pushinteger(L,seat); p_lua_pushinteger(L,epoch);
    p_lua_pushinteger(L,revision); p_lua_pushinteger(L,capable);
    return 5;
}
static int __cdecl seat_control_send(void *L) {
    const char *line=p_lua_tolstring(L,1,NULL); SeatControlEvent e; int sent=0;
    EnterCriticalSection(&g_net_lock);
    if (line && seat_control_parse(line,&e) && seat_control_scope_locked(&e) &&
        (g_role_is_guest ? e.kind==2 && (!g_two_guests || e.seat==sr_reserved) : e.kind!=2)) {
        char wire[192]; _snprintf_s(wire,sizeof(wire),_TRUNCATE,"%s\n",line);
        sent=exchange_send_locked(wire);
    }
    LeaveCriticalSection(&g_net_lock);
    p_lua_pushinteger(L,sent); return 1;
}
static int __cdecl seat_control_take(void *L) {
    SeatControlEvent e; int found=0;
    EnterCriticalSection(&g_net_lock);
    if (seat_control_enabled_locked() && sc_head!=sc_tail) {e=sc_queue[sc_head];sc_head=(sc_head+1)%SC_CAP;found=1;}
    LeaveCriticalSection(&g_net_lock);
    if (!found) return 0;
    p_lua_pushinteger(L,e.kind);p_lua_pushinteger(L,e.epoch);p_lua_pushinteger(L,e.revision);
    p_lua_pushinteger(L,e.seat);p_lua_pushinteger(L,e.hand);p_lua_pushinteger(L,e.street);
    p_lua_pushinteger(L,e.turn);p_lua_pushinteger(L,e.action);p_lua_pushinteger(L,e.amount);return 9;
}
static int __cdecl seat_picker_options(void *L) {
    EnterCriticalSection(&g_net_lock);
    unsigned enabled=seat_picker_enabled(), chosen=sr_picker_choice, initial=g_requested_seat;
    LeaveCriticalSection(&g_net_lock);
    p_lua_pushinteger(L,enabled);p_lua_pushinteger(L,chosen);p_lua_pushinteger(L,initial);
    return 3;
}
static int __cdecl seat_picker_choose(void *L) {
    const char *text=p_lua_tolstring(L,1,NULL);
    int accepted=0;
    EnterCriticalSection(&g_net_lock);
    if (seat_picker_enabled() && !sr_picker_choice && !sr_reserved && !x_fault &&
        text && text[0]>='1' && text[0]<='4' && !text[1]) {
        sr_picker_choice=g_requested_seat=(unsigned)(text[0]-'0');
        coop_log("seat: picker_selected seat=%u name=%s",sr_picker_choice,sr_names[sr_picker_choice]);
        // Selection may precede SOPEN, or race a healthy readiness reset.
        // The next SOPEN automatically retries the same immutable choice.
        seat_reservation_request_locked();
        accepted=!x_fault;
    }
    LeaveCriticalSection(&g_net_lock);
    p_lua_pushinteger(L,accepted);return 1;
}
static void seat_control_register_lua(void *L) {
    const char *names[]={"COOP_ControlOptions","COOP_ControlSend","COOP_ControlTake","COOP_SeatPickerOptions","COOP_SeatPickerChoose","COOP_MultiOptions"};
    PFN_lua_CFunction funcs[]={seat_control_options,seat_control_send,seat_control_take,seat_picker_options,seat_picker_choose,mp_options};
    for(int i=0;i<6;++i) {p_lua_pushstring(L,names[i]);p_lua_pushcclosure(L,funcs[i],0);p_lua_rawset(L,LUA_GLOBALSINDEX);}
}
