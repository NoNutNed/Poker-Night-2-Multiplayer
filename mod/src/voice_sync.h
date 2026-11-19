// Independent finite-voice ledger: may coexist with a camera/reveal fence.
// Network thread only parses attributed messages; Lua owns playback completion.
#define VS_CAP 64
#define VS_KEY 64
typedef struct {
    int active,ended,cancelled,ack[MP_COUNT];
    unsigned epoch,revision,generation,token;
    SOCKET expected[MP_COUNT];
    ULONGLONG ended_at;
    char key[VS_KEY];
} VoiceEntry;
static VoiceEntry vs_entries[VS_CAP];
static unsigned vs_next_token,vs_seen_token,vs_cancel_head,vs_cancel_tail;
static unsigned vs_cancel_tokens[VS_CAP+1];
static void voice_reset_locked(void) {
    memset(vs_entries,0,sizeof(vs_entries));vs_cancel_head=vs_cancel_tail=0;
}
static int voice_key(const char *key) {
    if(!key || strlen(key)>=VS_KEY)return 0;
    unsigned instance=0,count=0;char node[21],canonical[VS_KEY];int end=0;
    if(sscanf(key,"%u.%20[0-9].%u%n",&instance,node,&count,&end)!=3 || !end || key[end] ||
       instance>0x7fffffff || count>0x7fffffff || (node[0]=='0' && node[1]))return 0;
    _snprintf_s(canonical,sizeof(canonical),_TRUNCATE,"%u.%s.%u",instance,node,count);
    return !strcmp(key,canonical);
}
static int voice_ready_locked(void){return g_voice_sync && presentation_ready_locked();}
static VoiceEntry *voice_find_locked(const char *key) {
    for(int i=0;i<VS_CAP;++i)if(vs_entries[i].active && !strcmp(vs_entries[i].key,key))return &vs_entries[i];
    return NULL;
}
static int voice_scope_locked(VoiceEntry *e) {
    if(!e || !voice_ready_locked() || e->epoch!=x_epoch || e->revision!=sr_revision || e->generation!=x_connection_generation)return 0;
    if(!g_role_is_guest)for(int i=0;i<mp_count();++i)if(mp_peers[i].socket==INVALID_SOCKET || e->expected[i]!=mp_peers[i].socket)return 0;
    return 1;
}
static void voice_wire(char *line,size_t cap,const char *verb,VoiceEntry *e) {
    _snprintf_s(line,cap,_TRUNCATE,"%s epoch=%u rev=%u token=%u key=%s\n",verb,e->epoch,e->revision,e->token,e->key);
}
static VoiceEntry *voice_allocate_locked(const char *key,unsigned token) {
    if(voice_find_locked(key))return NULL;
    for(int i=0;i<VS_CAP;++i)if(!vs_entries[i].active) {
        VoiceEntry *e=&vs_entries[i];memset(e,0,sizeof(*e));e->active=1;e->epoch=x_epoch;e->revision=sr_revision;
        e->generation=x_connection_generation;e->token=token;strcpy_s(e->key,sizeof(e->key),key);
        for(int j=0;j<mp_count();++j)e->expected[j]=mp_peers[j].socket;
        return e;
    }
    return NULL;
}
static int voice_handle_line(const char *line) {
    if(strncmp(line,"VOPEN",5) && strncmp(line,"VCANCEL",7) && strncmp(line,"VACK",4))return 0;
    if(!g_voice_sync)return 1;
    EnterCriticalSection(&g_net_lock);
    unsigned epoch=0,rev=0,token=0;char verb[8],key[VS_KEY];int end=0;
    int valid=seat_reservation_numbers_valid(line) &&
        sscanf(line,"%7s epoch=%u rev=%u token=%u key=%63s%n",verb,&epoch,&rev,&token,key,&end)==5 && end && !line[end] &&
        token && token<=0x7fffffff && voice_key(key) && voice_ready_locked() && epoch==x_epoch && rev==sr_revision;
    if(!valid)coop_log("voice: rejected invalid_or_stale_message");
    else if(g_role_is_guest && !strcmp(verb,"VOPEN")) {
        if(token<=vs_seen_token || !voice_allocate_locked(key,token))exchange_fault_locked("voice_invalid_or_full_open");
        else {vs_seen_token=token;coop_log("voice: received epoch=%u token=%u key=%s",epoch,token,key);}
    } else {
        VoiceEntry *e=voice_find_locked(key);
        if(!voice_scope_locked(e) || token!=e->token)coop_log("voice: rejected mismatched_entry");
        else if(g_role_is_guest && !strcmp(verb,"VCANCEL")) {
            if(!e->cancelled) {
                unsigned next=(vs_cancel_tail+1)%(VS_CAP+1);
                if(next==vs_cancel_head)exchange_fault_locked("voice_cancel_queue_overflow");
                else {e->cancelled=1;vs_cancel_tokens[vs_cancel_tail]=token;vs_cancel_tail=next;}
            }
        } else if(!g_role_is_guest && !strcmp(verb,"VACK") && e->ended && !e->cancelled && mp_source>=0 && mp_source<mp_count() && !e->ack[mp_source]) {
            e->ack[mp_source]=1;coop_log("voice: peer_complete epoch=%u token=%u peer=%d key=%s",epoch,token,mp_source+2,key);
        } else coop_log("voice: rejected wrong_direction_or_duplicate_ack");
    }
    LeaveCriticalSection(&g_net_lock);return 1;
}
static int __cdecl voice_options(void *L){p_lua_pushinteger(L,g_voice_sync && g_presentation_sync && g_two_guests);return 1;}
static int __cdecl voice_open(void *L) {
    const char *key=p_lua_tolstring(L,1,NULL);int result=-1;
    EnterCriticalSection(&g_net_lock);
    if(!g_role_is_guest && voice_key(key) && voice_ready_locked() && vs_next_token<0x7fffffff) {
        VoiceEntry *e=voice_allocate_locked(key,++vs_next_token);
        if(e){char line[160];voice_wire(line,sizeof(line),"VOPEN",e);if(exchange_send_locked(line)){result=1;coop_log("voice: opened epoch=%u token=%u key=%s",e->epoch,e->token,key);}}
    }
    LeaveCriticalSection(&g_net_lock);p_lua_pushinteger(L,result);return 1;
}
static int __cdecl voice_end(void *L) {
    const char *key=p_lua_tolstring(L,1,NULL);int result=-1;
    EnterCriticalSection(&g_net_lock);
    VoiceEntry *e=voice_key(key)?voice_find_locked(key):NULL;
    if(!g_role_is_guest && voice_scope_locked(e) && !e->cancelled){if(!e->ended)e->ended_at=GetTickCount64();e->ended=1;result=1;}
    LeaveCriticalSection(&g_net_lock);p_lua_pushinteger(L,result);return 1;
}
static int __cdecl voice_status(void *L) {
    const char *key=p_lua_tolstring(L,1,NULL);int result=-1;
    EnterCriticalSection(&g_net_lock);
    VoiceEntry *e=voice_key(key)?voice_find_locked(key):NULL;
    if(!g_role_is_guest && voice_scope_locked(e)) {
        if(e->cancelled || (e->ended && mp_all(e->ack))) {
            result=1;coop_log("voice: all_complete epoch=%u token=%u key=%s cancelled=%d",e->epoch,e->token,e->key,e->cancelled);e->active=0;
        } else if(e->ended && GetTickCount64()-e->ended_at>=PB_WAIT_MS)exchange_fault_locked("voice_completion_timeout");
        else result=0;
    }
    LeaveCriticalSection(&g_net_lock);p_lua_pushinteger(L,result);return 1;
}
static int __cdecl voice_state(void *L) {
    const char *key=p_lua_tolstring(L,1,NULL);int result=-1;
    EnterCriticalSection(&g_net_lock);VoiceEntry *e=voice_key(key)?voice_find_locked(key):NULL;
    if(g_role_is_guest && voice_scope_locked(e))result=e->cancelled?2:1;
    LeaveCriticalSection(&g_net_lock);p_lua_pushinteger(L,result);return 1;
}
static int __cdecl voice_done(void *L) {
    const char *key=p_lua_tolstring(L,1,NULL);int result=-1;
    EnterCriticalSection(&g_net_lock);VoiceEntry *e=voice_key(key)?voice_find_locked(key):NULL;
    if(g_role_is_guest && voice_scope_locked(e)) {
        if(e->cancelled)result=1;
        else {char line[160];voice_wire(line,sizeof(line),"VACK",e);if(exchange_send_locked(line)){coop_log("voice: local_complete epoch=%u token=%u key=%s",e->epoch,e->token,key);e->active=0;result=1;}}
    }
    LeaveCriticalSection(&g_net_lock);p_lua_pushinteger(L,result);return 1;
}
static int __cdecl voice_cancel(void *L) {
    const char *key=p_lua_tolstring(L,1,NULL);int result=-1;
    EnterCriticalSection(&g_net_lock);VoiceEntry *e=voice_key(key)?voice_find_locked(key):NULL;
    if(!g_role_is_guest && voice_scope_locked(e)) {
        e->cancelled=1;char line[160];voice_wire(line,sizeof(line),"VCANCEL",e);if(exchange_send_locked(line))result=1;
    }
    LeaveCriticalSection(&g_net_lock);p_lua_pushinteger(L,result);return 1;
}
static int __cdecl voice_take_cancel(void *L) {
    char key[VS_KEY]={0};
    EnterCriticalSection(&g_net_lock);
    if(g_role_is_guest && voice_ready_locked() && vs_cancel_head!=vs_cancel_tail) {
        unsigned token=vs_cancel_tokens[vs_cancel_head];vs_cancel_head=(vs_cancel_head+1)%(VS_CAP+1);
        for(int i=0;i<VS_CAP;++i)if(vs_entries[i].active && vs_entries[i].token==token) {
            strcpy_s(key,sizeof(key),vs_entries[i].key);vs_entries[i].active=0;break;
        }
    }
    LeaveCriticalSection(&g_net_lock);if(!key[0])return 0;p_lua_pushstring(L,key);return 1;
}
static void voice_register_lua(void *L) {
    const char *names[]={"COOP_VoiceOptions","COOP_VoiceOpen","COOP_VoiceEnd","COOP_VoiceStatus","COOP_VoiceState","COOP_VoiceDone","COOP_VoiceCancel","COOP_VoiceTakeCancel"};
    PFN_lua_CFunction funcs[]={voice_options,voice_open,voice_end,voice_status,voice_state,voice_done,voice_cancel,voice_take_cancel};
    for(int i=0;i<8;++i){p_lua_pushstring(L,names[i]);p_lua_pushcclosure(L,funcs[i],0);p_lua_rawset(L,LUA_GLOBALSINDEX);}
}
