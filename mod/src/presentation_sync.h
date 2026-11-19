// Scope completion is application state, separate from TCP receipt/application.
// Only zoom/reveal scopes are supported in this first opt-in protocol8 slice.
// All fields are guarded by g_net_lock; native socket code never calls Lua.
#define PB_KEY 64
#define PB_WAIT_MS 120000ULL
typedef struct {
    int active,closed,local_done,complete,ack[MP_COUNT];
    unsigned epoch,revision,token,generation;
    SOCKET expected[MP_COUNT];
    ULONGLONG started,closed_at;
    char key[PB_KEY];
} PresentationBarrier;
static PresentationBarrier pb;
static unsigned pb_next_token,pb_seen_token;
static char pb_wait_key[PB_KEY];
static ULONGLONG pb_wait_started;
static void presentation_reset_locked(void) {
    memset(&pb,0,sizeof(pb));pb_wait_key[0]=0;pb_wait_started=0;
    // Tokens are monotonic across readiness/hand epochs within this process.
}
static int presentation_key(const char *key) {
    unsigned hand=0,street=0,occurrence=0;char kind[16],canonical[PB_KEY];int end=0;
    if(!key || strlen(key)>=PB_KEY ||
       sscanf(key,"%u.%u.%15[a-z].%u%n",&hand,&street,kind,&occurrence,&end)!=4 ||
       !end || key[end] || !hand || hand>0x7fffffff || street>4 || !occurrence || occurrence>0x7fffffff ||
       (strcmp(kind,"zoom") && strcmp(kind,"reveal"))) return 0;
    _snprintf_s(canonical,sizeof(canonical),_TRUNCATE,"%u.%u.%s.%u",hand,street,kind,occurrence);
    return !strcmp(key,canonical);
}
static int presentation_ready_locked(void) {
    return g_presentation_sync && g_two_guests && seat_control_enabled_locked();
}
static int presentation_scope_locked(const char *key) {
    if(!presentation_ready_locked() || !pb.active || strcmp(pb.key,key) ||
       pb.epoch!=x_epoch || pb.revision!=sr_revision || pb.generation!=x_connection_generation) return 0;
    if(!g_role_is_guest)for(int i=0;i<mp_count();++i)
        if(mp_peers[i].socket==INVALID_SOCKET || pb.expected[i]!=mp_peers[i].socket) return 0;
    return 1;
}
static int presentation_timed_out_locked(ULONGLONG start) {
    if(GetTickCount64()-start<PB_WAIT_MS)return 0;
    exchange_fault_locked("presentation_completion_timeout");return 1;
}
static void presentation_wire(char *line,size_t cap,const char *verb) {
    _snprintf_s(line,cap,_TRUNCATE,"%s epoch=%u rev=%u token=%u key=%s\n",verb,pb.epoch,pb.revision,pb.token,pb.key);
}
static int presentation_handle_line(const char *line) {
    if(strncmp(line,"PBEGIN",6) && strncmp(line,"PCLOSE",6) && strncmp(line,"PACK",4))return 0;
    if(!g_presentation_sync)return 1;
    unsigned epoch=0,revision=0,token=0;char key[PB_KEY],verb[8];int end=0;
    EnterCriticalSection(&g_net_lock);
    int valid=seat_reservation_numbers_valid(line) &&
        sscanf(line,"%7s epoch=%u rev=%u token=%u key=%63s%n",verb,&epoch,&revision,&token,key,&end)==5 &&
        end && !line[end] && token && token<=0x7fffffff && presentation_key(key) &&
        presentation_ready_locked() && epoch==x_epoch && revision==sr_revision;
    if(!valid)coop_log("presentation: rejected invalid_or_stale_message");
    else if(g_role_is_guest && !strcmp(verb,"PBEGIN")) {
        if(token<=pb_seen_token || (pb.active && !pb.local_done) ||
           (pb_wait_key[0] && strcmp(pb_wait_key,key))) exchange_fault_locked("presentation_invalid_begin");
        else {
            memset(&pb,0,sizeof(pb));pb.active=1;pb.epoch=epoch;pb.revision=revision;
            pb.token=pb_seen_token=token;pb.generation=x_connection_generation;pb.started=GetTickCount64();
            strcpy_s(pb.key,sizeof(pb.key),key);
            coop_log("presentation: scope_received epoch=%u token=%u key=%s",epoch,token,key);
        }
    } else if(g_role_is_guest && !strcmp(verb,"PCLOSE")) {
        if(presentation_scope_locked(key) && token==pb.token && !pb.closed) {
            pb.closed=1;pb.closed_at=GetTickCount64();
        } else coop_log("presentation: rejected mismatched_close");
    } else if(!g_role_is_guest && !strcmp(verb,"PACK")) {
        if(presentation_scope_locked(key) && token==pb.token && pb.closed && mp_source>=0 && mp_source<mp_count() &&
           pb.expected[mp_source]==mp_peers[mp_source].socket && !pb.ack[mp_source]) {
            pb.ack[mp_source]=1;
            coop_log("presentation: peer_complete epoch=%u token=%u peer=%d key=%s",epoch,token,mp_source+2,key);
        } else coop_log("presentation: rejected wrong_peer_scope_or_duplicate_ack");
    } else coop_log("presentation: rejected wrong_direction");
    LeaveCriticalSection(&g_net_lock);return 1;
}
static int __cdecl presentation_options(void *L) {
    p_lua_pushinteger(L,g_presentation_sync && g_two_guests);return 1;
}
static int __cdecl presentation_begin(void *L) {
    const char *key=p_lua_tolstring(L,1,NULL);int result=-1;
    EnterCriticalSection(&g_net_lock);
    if(presentation_key(key) && presentation_ready_locked()) {
        if(g_role_is_guest) {
            if(presentation_scope_locked(key)) {pb_wait_key[0]=0;result=1;}
            else if(!pb.active || pb.local_done) {
                if(!pb_wait_key[0]){strcpy_s(pb_wait_key,sizeof(pb_wait_key),key);pb_wait_started=GetTickCount64();}
                if(!strcmp(pb_wait_key,key) && !presentation_timed_out_locked(pb_wait_started))result=0;
            }
        } else if(!pb.active || pb.complete) {
            if(pb_next_token<0x7fffffff) {
                memset(&pb,0,sizeof(pb));pb.active=1;pb.epoch=x_epoch;pb.revision=sr_revision;
                pb.token=++pb_next_token;pb.generation=x_connection_generation;pb.started=GetTickCount64();
                strcpy_s(pb.key,sizeof(pb.key),key);
                for(int i=0;i<mp_count();++i)pb.expected[i]=mp_peers[i].socket;
                char line[160];presentation_wire(line,sizeof(line),"PBEGIN");
                if(exchange_send_locked(line))result=1;
                coop_log("presentation: scope_begin epoch=%u token=%u key=%s",pb.epoch,pb.token,key);
            }
        }
    }
    LeaveCriticalSection(&g_net_lock);p_lua_pushinteger(L,result);return 1;
}
static int __cdecl presentation_finish(void *L) {
    const char *key=p_lua_tolstring(L,1,NULL);int result=-1;
    EnterCriticalSection(&g_net_lock);
    if(presentation_key(key) && presentation_scope_locked(key)) {
        ULONGLONG start=pb.closed?pb.closed_at:pb.started;
        if(!presentation_timed_out_locked(start)) {
            char line[160];
            if(g_role_is_guest) {
                if(!pb.closed)result=0;
                else if(pb.local_done)result=1;
                else {presentation_wire(line,sizeof(line),"PACK");if(exchange_send_locked(line)){pb.local_done=1;result=1;}}
            } else {
                if(pb.closed)result=1;
                else {
                    pb.closed=1;pb.closed_at=GetTickCount64();presentation_wire(line,sizeof(line),"PCLOSE");
                    if(exchange_send_locked(line))result=1;
                }
            }
        }
    }
    LeaveCriticalSection(&g_net_lock);p_lua_pushinteger(L,result);return 1;
}
static int __cdecl presentation_status(void *L) {
    const char *key=p_lua_tolstring(L,1,NULL);int result=-1;
    EnterCriticalSection(&g_net_lock);
    if(presentation_key(key) && presentation_scope_locked(key) && pb.closed &&
       !presentation_timed_out_locked(pb.closed_at)) {
        result=g_role_is_guest?pb.local_done:mp_all(pb.ack);
        if(result && !g_role_is_guest && !pb.complete) {
            pb.complete=1;
            coop_log("presentation: all_complete epoch=%u token=%u key=%s waited_ms=%llu",pb.epoch,pb.token,pb.key,GetTickCount64()-pb.closed_at);
        }
    }
    LeaveCriticalSection(&g_net_lock);p_lua_pushinteger(L,result);return 1;
}
static void presentation_register_lua(void *L) {
    const char *names[]={"COOP_PresentationOptions","COOP_PresentationBegin","COOP_PresentationFinish","COOP_PresentationStatus"};
    PFN_lua_CFunction funcs[]={presentation_options,presentation_begin,presentation_finish,presentation_status};
    for(int i=0;i<4;++i){p_lua_pushstring(L,names[i]);p_lua_pushcclosure(L,funcs[i],0);p_lua_rawset(L,LUA_GLOBALSINDEX);}
}
