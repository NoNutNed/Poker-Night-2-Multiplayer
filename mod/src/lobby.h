// Pre-game membership is independent of gameplay epochs. Socket thread never calls Lua.
static unsigned lb_nonce,lb_revision,lb_connected,lb_ready,lb_seats[MP_COUNT],lb_lock_acks;
static ULONGLONG lb_lock_started;
static volatile LONG lb_worker_running;
static SOCKET lb_listener=INVALID_SOCKET;
static void net_pump_socket(SOCKET socket);
static int __cdecl lb_input(void *L);
static int __cdecl lb_scan(void *L);
static int __cdecl lb_found(void *L);
static void lb_state_locked(void);
static int __cdecl coop_install_ai_hook(void *L);
static unsigned lb_full(void){return (1u<<mp_count())-1;}
#include "hand_ready.h"
#include "match_results.h"
static void lb_changed_locked(void){++lb_revision;lb_ready=lb_lock_acks=0;lb_phase=1;lb_state_locked();}
static void lb_state_locked(void) {
    char line[256];
    _snprintf_s(line,sizeof(line),_TRUNCATE,"LSTATE id=%u rev=%u phase=%d guests=%d connected=%u ready=%u seat2=%u seat3=%u seat4=%u seat5=%u\n",
        lb_nonce,lb_revision,lb_phase,mp_count(),lb_connected,lb_ready,lb_seats[0],lb_seats[1],lb_seats[2],lb_seats[3]);
    mp_send_locked(-1,line);
}
static void lb_connected_locked(int slot){lb_connected|=1u<<slot;lb_seats[slot]=0;lb_changed_locked();}
static void lb_disconnected_locked(int slot){lb_connected&=~(1u<<slot);lb_seats[slot]=0;lb_changed_locked();}
static void lb_lock_locked(void) {
    if(lb_phase!=1 || lb_connected!=lb_full() || lb_ready!=((lb_full()<<1)|1) || !mp_seats_valid(lb_seats))return;
    for(int i=0;i<mp_count();++i)if(!mp_peers[i].hello)return;
    lb_phase=2;lb_lock_acks=0;lb_lock_started=GetTickCount64();lb_state_locked();
}
static void lb_tick_locked(void){if(lb_phase==2 && GetTickCount64()-lb_lock_started>=15000){lb_phase=4;lb_error=4;exchange_fault_locked("lobby_start_ack_timeout");}}
static int lb_handle_line(const char *line) {
    if(line[0]!='L')return 0;
    EnterCriticalSection(&g_net_lock);
    unsigned id=0,rev=0,value=0,count=0,connected=0,ready=0,phase=0,seats[MP_COUNT]={0};int end=0;
    if(!seat_reservation_numbers_valid(line)){lb_error=3;goto done;}
    if (!strncmp(line, "LDEAL_", 6)) { hand_ready_handle_locked(line);goto done; }
    if (!strncmp(line, "LRESULT_", 8)) { mr_handle_locked(line);goto done; }
    if(!g_role_is_guest && mp_source>=0 && mp_source<mp_count()) {
        unsigned slot=(unsigned)mp_source;
        if(sscanf(line,"LCLAIM id=%u rev=%u seat=%u%n",&id,&rev,&value,&end)==3 && end && !line[end] &&
           lb_phase==1 && id==lb_nonce && rev==lb_revision && value>=1 && value<=4 && mp_peers[slot].hello) {
            int occupied=0;for(int i=0;i<mp_count();++i)if(i!=(int)slot && lb_seats[i]==value)occupied=1;
            if(!occupied){if(lb_seats[slot]!=value){lb_seats[slot]=value;lb_changed_locked();}}
            else {char reject[96];_snprintf_s(reject,sizeof(reject),_TRUNCATE,"LREJECT id=%u rev=%u\n",lb_nonce,lb_revision);mp_send_locked(slot,reject);}
        } else if(sscanf(line,"LREADY id=%u rev=%u value=%u%n",&id,&rev,&value,&end)==3 && end && !line[end] &&
                  lb_phase==1 && id==lb_nonce && rev==lb_revision && value<=1 && lb_seats[slot] && mp_peers[slot].hello) {
            if(value)lb_ready|=1u<<(slot+1);else lb_ready&=~(1u<<(slot+1));lb_state_locked();lb_lock_locked();
        } else if(sscanf(line,"LACK id=%u rev=%u seat=%u%n",&id,&rev,&value,&end)==3 && end && !line[end] &&
                  lb_phase==2 && id==lb_nonce && rev==lb_revision && value==lb_seats[slot]) {
            lb_lock_acks|=1u<<slot;
            if(lb_lock_acks==lb_full()) {
                for(int i=0;i<mp_count();++i)mp_peers[i].session_seat=lb_seats[i];
                lb_phase=3;char begin[96];_snprintf_s(begin,sizeof(begin),_TRUNCATE,"LBEGIN id=%u rev=%u\n",lb_nonce,lb_revision);mp_send_locked(-1,begin);
            }
        } else coop_log("lobby: rejected stale_or_wrong_phase peer=%u",slot+2);
    } else if(g_role_is_guest && mp_id>=2 && mp_id<=(unsigned)mp_count()+1) {
        if(sscanf(line,"LSTATE id=%u rev=%u phase=%u guests=%u connected=%u ready=%u seat2=%u seat3=%u seat4=%u seat5=%u%n",
          &id,&rev,&phase,&count,&connected,&ready,&seats[0],&seats[1],&seats[2],&seats[3],&end)==10 && end && !line[end] &&
          id && rev && (phase==1 || phase==2) && count==(unsigned)mp_count() && connected<=lb_full() && ready<=((lb_full()<<1)|1) &&
          (!lb_nonce || id==lb_nonce) && rev>=lb_revision && lb_phase<3) {
            unsigned mask=0;int valid=1;
            for(int i=0;i<MP_COUNT;++i){if(seats[i]>4 || (i>=mp_count() && seats[i]) || (seats[i] && !(connected&(1u<<i))) || (seats[i] && (mask&(1u<<seats[i]))))valid=0;if(seats[i])mask|=1u<<seats[i];}
            if(!valid){lb_error=3;goto done;}
            lb_nonce=id;lb_revision=rev;lb_phase=(int)phase;lb_connected=connected;lb_ready=ready;memcpy(lb_seats,seats,sizeof(seats));lb_error=0;
            if(phase==2 && connected==lb_full() && ready==((lb_full()<<1)|1) && mp_seats_valid(seats)) {
                g_requested_seat=sr_picker_choice=seats[mp_id-2];
                char ack[96];_snprintf_s(ack,sizeof(ack),_TRUNCATE,"LACK id=%u rev=%u seat=%u\n",id,rev,g_requested_seat);exchange_send_locked(ack);
            }
        } else if(sscanf(line,"LBEGIN id=%u rev=%u%n",&id,&rev,&end)==2 && end && !line[end] && lb_phase==2 &&
                  id==lb_nonce && rev==lb_revision && mp_seats_valid(lb_seats))lb_phase=3;
        else if(sscanf(line,"LREJECT id=%u rev=%u%n",&id,&rev,&end)==2 && end && !line[end] && id==lb_nonce && rev==lb_revision)lb_error=6;
        else coop_log("lobby: ignored invalid_or_stale_state");
    }
done:
    LeaveCriticalSection(&g_net_lock);return 1;
}
static DWORD WINAPI lb_network_thread(LPVOID unused) {
    (void)unused;WSADATA data;int error=WSAStartup(MAKEWORD(2,2),&data);
    if(error)goto fail;
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(s==INVALID_SOCKET)goto cleanup;
    struct sockaddr_in address;memset(&address,0,sizeof(address));address.sin_family=AF_INET;address.sin_port=htons((u_short)g_host_port);
    if(!g_role_is_guest) {
        address.sin_addr.s_addr=htonl(INADDR_ANY);
        if(bind(s,(struct sockaddr*)&address,sizeof(address)) || listen(s,4)){closesocket(s);goto cleanup;}
        EnterCriticalSection(&g_net_lock);lb_listener=s;LeaveCriticalSection(&g_net_lock);
        mp_server_loop(s);
        EnterCriticalSection(&g_net_lock);if(lb_listener==s){closesocket(s);lb_listener=INVALID_SOCKET;}LeaveCriticalSection(&g_net_lock);
    } else {
        InetPtonA(AF_INET,g_host_ip,&address.sin_addr);
        u_long nonblocking=1;ioctlsocket(s,FIONBIO,&nonblocking);
        int result=connect(s,(struct sockaddr*)&address,sizeof(address));
        if(result && WSAGetLastError()!=WSAEWOULDBLOCK){closesocket(s);goto cleanup;}
        ULONGLONG start=GetTickCount64();int connected=!result;
        while(!connected && GetTickCount64()-start<5000 && lb_phase==1) {
            fd_set write,errorSet;FD_ZERO(&write);FD_ZERO(&errorSet);FD_SET(s,&write);FD_SET(s,&errorSet);struct timeval wait={0,50000};
            if(select(0,NULL,&write,&errorSet,&wait)>0){int status=1,len=sizeof(status);getsockopt(s,SOL_SOCKET,SO_ERROR,(char*)&status,&len);connected=!status;break;}
        }
        if(!connected){closesocket(s);goto cleanup;}
        nonblocking=0;ioctlsocket(s,FIONBIO,&nonblocking);
        EnterCriticalSection(&g_net_lock);g_client_sock=s;LeaveCriticalSection(&g_net_lock);
        net_pump_socket(s);closesocket(s);
    }
cleanup:
    WSACleanup();
fail:
    EnterCriticalSection(&g_net_lock);if(lb_phase){lb_phase=4;if(!lb_error)lb_error=2;}LeaveCriticalSection(&g_net_lock);
    InterlockedExchange(&lb_worker_running,0);return 0;
}
static int lb_ipv4(const char *text) {
    unsigned a,b,c,d;int end=0;char canonical[32];
    if(!text || sscanf(text,"%u.%u.%u.%u%n",&a,&b,&c,&d,&end)!=4 || !end || text[end] || a>255 || b>255 || c>255 || d>255 || !a || a>=224)return 0;
    _snprintf_s(canonical,sizeof(canonical),_TRUNCATE,"%u.%u.%u.%u",a,b,c,d);return !strcmp(text,canonical);
}
static int lb_target(const char *text,char *ip,unsigned *port) {
    if(!text || strlen(text)>21)return 0;
    const char *colon=strchr(text,':');char host[32];unsigned value=(unsigned)g_host_port;
    size_t length=colon?(size_t)(colon-text):strlen(text);
    if(length>=sizeof(host))return 0;memcpy(host,text,length);host[length]=0;
    if(!lb_ipv4(host))return 0;
    if(colon){int end=0;if(sscanf(colon+1,"%u%n",&value,&end)!=1 || !end || colon[1+end] || !value || value>65535)return 0;char canonical[8];sprintf_s(canonical,sizeof(canonical),"%u",value);if(strcmp(canonical,colon+1))return 0;}
    strcpy_s(ip,64,host);*port=value;return 1;
}
static int __cdecl lb_action(void *L) {
    const char *action=p_lua_tolstring(L,1,NULL),*arg=p_lua_tolstring(L,2,NULL);int ok=0;
    if(!g_lobby_mode || !action){p_lua_pushinteger(L,0);return 1;}
    EnterCriticalSection(&g_net_lock);
    if((!strcmp(action,"host") || !strcmp(action,"join")) && !lb_worker_running && lb_phase==0) {
        int host=!strcmp(action,"host");
        char ip[64];unsigned port=0;
        if((host && arg && arg[0]>='1' && arg[0]<='4' && !arg[1]) || (!host && lb_target(arg,ip,&port))) {
            exchange_disconnected_locked();g_client_sock=INVALID_SOCKET;
            memset(mp_peers,0,sizeof(mp_peers));for(int i=0;i<MP_COUNT;++i)mp_peers[i].socket=INVALID_SOCKET;
            g_role_is_guest=!host;if(host)g_guest_count=arg[0]-'0';else {strcpy_s(g_host_ip,sizeof(g_host_ip),ip);g_host_port=(int)port;}
            g_two_guests=g_test_mode=g_seat_probe=g_seat_control=g_seat_picker=g_presentation_sync=g_voice_sync=1;
            lb_phase=1;lb_error=0;lb_nonce=host?(unsigned)(GetTickCount64()%0x7ffffffe)+1:0;lb_revision=1;lb_connected=lb_ready=lb_lock_acks=0;memset(lb_seats,0,sizeof(lb_seats));
            hand_ready_hand=hand_ready_votes=0;
            mr_clear_locked();
            mp_stopped=x_fault=0;sr_picker_choice=0;mp_id=0;x_peer=x_local_ready=x_peer_ready=x_active=0;
            InterlockedExchange(&lb_worker_running,1);HANDLE thread=CreateThread(NULL,0,lb_network_thread,NULL,0,NULL);
            if(thread){CloseHandle(thread);ok=1;}else{lb_phase=4;lb_error=2;InterlockedExchange(&lb_worker_running,0);}
        }
    } else if(lb_phase==1 && !strcmp(action,"claim") && g_role_is_guest && arg && arg[0]>='1' && arg[0]<='4' && !arg[1] && lb_nonce) {
        char line[96];_snprintf_s(line,sizeof(line),_TRUNCATE,"LCLAIM id=%u rev=%u seat=%c\n",lb_nonce,lb_revision,arg[0]);ok=exchange_send_locked(line);
    } else if(lb_phase==1 && !strcmp(action,"ready") && arg && (arg[0]=='0' || arg[0]=='1') && !arg[1]) {
        if(g_role_is_guest){if(lb_nonce && mp_id>=2 && lb_seats[mp_id-2]){char line[96];_snprintf_s(line,sizeof(line),_TRUNCATE,"LREADY id=%u rev=%u value=%c\n",lb_nonce,lb_revision,arg[0]);ok=exchange_send_locked(line);}}
        else{if(arg[0]=='1')lb_ready|=1;else lb_ready&=~1u;lb_state_locked();lb_lock_locked();ok=1;}
    } else if(!strcmp(action,"leave") && lb_phase!=3 && lb_phase!=5) {
        lb_phase=0;lb_error=0;
        if(lb_listener!=INVALID_SOCKET){closesocket(lb_listener);lb_listener=INVALID_SOCKET;}
        for(int i=0;i<MP_COUNT;++i)if(mp_peers[i].socket!=INVALID_SOCKET)shutdown(mp_peers[i].socket,SD_BOTH);
        if(g_client_sock!=INVALID_SOCKET)shutdown(g_client_sock,SD_BOTH);
        ok=1;
    }
    LeaveCriticalSection(&g_net_lock);p_lua_pushinteger(L,ok);return 1;
}
static int __cdecl lb_status(void *L) {
    EnterCriticalSection(&g_net_lock);
    int values[]={g_lobby_mode,lb_phase,g_role_is_guest,(int)mp_id,(int)lb_nonce,(int)lb_revision,mp_count(),(int)lb_connected,(int)lb_ready,
        (int)lb_seats[0],(int)lb_seats[1],(int)lb_seats[2],(int)lb_seats[3],lb_error};
    LeaveCriticalSection(&g_net_lock);for(int i=0;i<14;++i)p_lua_pushinteger(L,values[i]);return 14;
}
static void lb_register_lua(void *L) {
    const char *names[]={"COOP_LobbyAction","COOP_LobbyStatus","COOP_LobbyInstall","COOP_LobbyInput","COOP_LobbyScan","COOP_LobbyFound","COOP_HandReadyAction","COOP_HandReadyStatus","COOP_MatchAction","COOP_MatchStatus"};
    PFN_lua_CFunction funcs[]={lb_action,lb_status,coop_install_ai_hook,lb_input,lb_scan,lb_found,hand_ready_action,hand_ready_status,mr_action,mr_status};
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i){p_lua_pushstring(L,names[i]);p_lua_pushcclosure(L,funcs[i],0);p_lua_rawset(L,LUA_GLOBALSINDEX);}
}
static int __cdecl lb_input(void *L) {
    static int mouse,held[256];int x=-1,y=-1,click=0,key=0;DWORD owner=0;
    HWND window=GetForegroundWindow();GetWindowThreadProcessId(window,&owner);
    int focused=owner==GetCurrentProcessId();
    const char *mode=p_lua_tolstring(L,1,NULL);
    /* Event callbacks query focus without consuming the IP keyboard edges. */
    if(mode && !strcmp(mode,"focus")) {
        p_lua_pushinteger(L,-1);p_lua_pushinteger(L,-1);p_lua_pushinteger(L,0);p_lua_pushinteger(L,0);p_lua_pushinteger(L,focused);return 5;
    }
    if(focused) {
        POINT point;RECT rect;
        if(GetCursorPos(&point) && ScreenToClient(window,&point) && GetClientRect(window,&rect) && rect.right>0 && rect.bottom>0){x=point.x*1000/rect.right;y=point.y*1000/rect.bottom;}
        int down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;click=down && !mouse;mouse=down;
        for(int code=0;code<256;++code) {
            int eligible=(code>='0' && code<='9') || (code>=VK_NUMPAD0 && code<=VK_NUMPAD9) || code==VK_OEM_PERIOD || code==VK_DECIMAL || code==VK_OEM_1 || code==VK_BACK || code==VK_RETURN;
            if(!eligible)continue;
            int pressed=(GetAsyncKeyState(code)&0x8000)!=0;
            if(mode && !strcmp(mode,"ip") && pressed && !held[code] && !key) {
                key=code>=VK_NUMPAD0 && code<=VK_NUMPAD9?'0'+code-VK_NUMPAD0:code;
                if(code==VK_OEM_PERIOD || code==VK_DECIMAL)key='.';
                if(code==VK_OEM_1)key=':';
            }
            held[code]=pressed;
        }
    } else {mouse=0;memset(held,0,sizeof(held));}
    p_lua_pushinteger(L,x);p_lua_pushinteger(L,y);p_lua_pushinteger(L,click);p_lua_pushinteger(L,key);p_lua_pushinteger(L,focused);return 5;
}

