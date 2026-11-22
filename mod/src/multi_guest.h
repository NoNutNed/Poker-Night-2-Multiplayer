// Protocol10 adds a participating count; protocols7/8/9 keep their accepted two-peer path.
// Socket identity, readiness and roster acknowledgments are per peer.
static int mp_send_locked(int target,const char *line) {
    int count=0,failed=0;
    for(int i=0;i<mp_count();++i) {
        if ((target>=0 && target!=i) || mp_peers[i].socket==INVALID_SOCKET) continue;
        int length=(int)strlen(line),sent=0;
        while(sent<length) {
            int n=send(mp_peers[i].socket,line+sent,length-sent,0);
            if(n<=0) {failed=1;break;}
            sent+=n;
        }
        if(sent==length)++count;
    }
    if(failed){exchange_fault_locked("multi_send_failed");return 0;}
    return count>0;
}
static void mp_clear_locked(int connection) {
    presentation_reset_locked();
    voice_reset_locked();
    mp_epoch=0;mp_held=mp_rejected=0;
    memset(mp_seats,0,sizeof(mp_seats));
    for(int i=0;i<mp_count();++i) mp_peers[i].ack=0;
    if(connection && g_role_is_guest) mp_id=0;
}
static int mp_scope_locked(unsigned epoch,unsigned revision,unsigned seat) {
    if(!seat_control_enabled_locked() || epoch!=x_epoch || revision!=3 || seat<1 || seat>4)return 0;
    if(g_lobby_mode && lb_cast_only && seat==lb_host_seat)return 1;
    for(int i=0;i<mp_count();++i)if(mp_seats[i]==seat)return 1;
    return 0;
}
static int mp_seats_valid(const unsigned *seats) {
    unsigned mask=0;
    for(int i=0;i<MP_COUNT;++i) {
        if(i>=mp_count()){if(seats[i])return 0;continue;}
        if(seats[i]<1 || seats[i]>4 || (mask&(1u<<seats[i])))return 0;
        mask|=1u<<seats[i];
    }
    return 1;
}
static void mp_roster_locked(void) {
    if(!mp_seats_valid(mp_seats)) return;
    char line[160];sr_revision=3;
    if(g_lobby_mode)_snprintf_s(line,sizeof(line),_TRUNCATE,"MROSTER epoch=%u rev=3 seat2=%u seat3=%u seat4=%u seat5=%u\n",mp_epoch,mp_seats[0],mp_seats[1],mp_seats[2],mp_seats[3]);
    else _snprintf_s(line,sizeof(line),_TRUNCATE,"MROSTER epoch=%u rev=3 seat2=%u seat3=%u\n",mp_epoch,mp_seats[0],mp_seats[1]);
    exchange_send_locked(line);
}
static void mp_open_locked(void) {
    if(g_role_is_guest || !x_active || x_fault || !sr_control_peer || mp_epoch==x_epoch) return;
    seat_reservation_clear_locked(0);
    mp_epoch=sr_epoch=x_epoch;sr_revision=1;
    char line[96];_snprintf_s(line,sizeof(line),_TRUNCATE,"MOPEN epoch=%u\n",mp_epoch);
    exchange_send_locked(line);
}
static int mp_request_locked(void) {
    if(!g_role_is_guest || !mp_id || !x_active || x_fault || mp_epoch!=x_epoch ||
       !mp_epoch || mp_held || (seat_picker_enabled() && !sr_picker_choice)) return 0;
    mp_rejected=0;
    char line[96];_snprintf_s(line,sizeof(line),_TRUNCATE,"MCLAIM epoch=%u seat=%u\n",mp_epoch,g_requested_seat);
    return exchange_send_locked(line);
}
static void mp_aggregate_locked(void) {
    int hello=1,ready=1;
    for(int i=0;i<mp_count();++i) {
        hello=hello && mp_peers[i].socket!=INVALID_SOCKET && mp_peers[i].hello;
        ready=ready && mp_peers[i].hello && mp_peers[i].ready;
    }
    x_peer=sr_peer=sr_control_peer=hello;
    x_peer_ready=hello && ready;
    if(!x_peer_ready) {x_active=0;seat_reservation_clear_locked(0);}
    exchange_send_locked(x_local_ready && x_peer_ready ? "XREADY active=1\n" : "XREADY active=0\n");
    exchange_start_locked();
}
static int mp_handle_line(const char *line) {
    EnterCriticalSection(&g_net_lock);
    int handled=1;unsigned epoch=0,a=0,b=0;int end=0;
    if(!g_role_is_guest && mp_source>=0 && mp_source<mp_count()) {
        MultiPeer *peer=&mp_peers[mp_source];
        if(!strcmp(line,g_lobby_mode?"XHELLO version=" PN2_LOBBY_PROTOCOL_TEXT:g_presentation_sync ? (g_voice_sync?"XHELLO version=9":"XHELLO version=8") : "XHELLO version=7")) {
            peer->hello=1;mp_aggregate_locked();
        } else if(!strcmp(line,"XREADY active=0") || !strcmp(line,"XREADY active=1")) {
            if(peer->hello) {peer->ready=line[14]=='1';mp_aggregate_locked();}
        } else if(!strncmp(line,"M",1)) {
            if(!seat_reservation_numbers_valid(line) || !x_active || x_fault || !peer->hello || mp_epoch!=x_epoch) {
                coop_log("multi: ignored invalid_claim_scope peer=%d",mp_source+2);
            } else if(sscanf(line,"MCLAIM epoch=%u seat=%u%n",&epoch,&a,&end)==2 && end && !line[end]) {
                if(epoch!=mp_epoch || a<1 || a>4) {
                    coop_log("multi: ignored stale_or_invalid_claim");
                } else {
                  int occupied=peer->session_seat && peer->session_seat!=a;
                  for(int i=0;i<mp_count();++i)if(i!=mp_source && mp_seats[i]==a)occupied=1;
                  if(occupied) {
                    char reject[96];_snprintf_s(reject,sizeof(reject),_TRUNCATE,"MREJECT epoch=%u reason=occupied\n",epoch);
                    mp_send_locked(mp_source,reject);
                } else {
                    peer->session_seat=mp_seats[mp_source]=a;
                    char held[96];_snprintf_s(held,sizeof(held),_TRUNCATE,"MHELD epoch=%u seat=%u\n",epoch,a);
                    mp_send_locked(mp_source,held);
                    coop_log("multi: reserved epoch=%u peer=%d seat=%u",epoch,mp_source+2,a);
                    mp_roster_locked();
                  }
                }
            } else if(sscanf(line,"MACK epoch=%u rev=3 seat=%u%n",&epoch,&a,&end)==2 && end && !line[end]) {
                if(epoch==mp_epoch && sr_revision==3 && a && a==mp_seats[mp_source]) {
                    peer->ack=1;
                    int all=1;for(int i=0;i<mp_count();++i)if(!mp_peers[i].ack)all=0;
                    if(all) {
                        char go[96];_snprintf_s(go,sizeof(go),_TRUNCATE,"MGO epoch=%u rev=3\n",epoch);
                        if(exchange_send_locked(go)) {sr_confirmed=1;sr_reserved=mp_seats[0];
                            coop_log("multi: ready epoch=%u seat2=%u seat3=%u",epoch,mp_seats[0],mp_seats[1]);}
                    }
                }
            } else coop_log("multi: ignored malformed_or_wrong_direction");
        } else if(line[0]=='S') {exchange_fault_locked("multi_incompatible_seat_protocol");}
        else handled=0;
    } else if(g_role_is_guest && line[0]=='M') {
        if(!seat_reservation_numbers_valid(line)) exchange_fault_locked("multi_malformed_numbers");
        else if(g_lobby_mode && sscanf(line,"MWELCOME peer=%u guests=%u%n",&a,&b,&end)==2 && end && !line[end] && b>=1 && b<=4 && a>=2 && a<=b+1 && (!mp_id || (mp_id==a && g_guest_count==(int)b))) {
            mp_id=a;g_guest_count=b;sr_peer=sr_control_peer=1;
        } else if(!g_lobby_mode && sscanf(line,"MWELCOME peer=%u%n",&a,&end)==1 && end && !line[end] && a>=2 && a<=3 && (!mp_id || mp_id==a)) {
            mp_id=a;sr_peer=sr_control_peer=1;
        } else if(!x_active || x_fault || !mp_id) {coop_log("multi: ignored inactive_roster");}
        else if(sscanf(line,"MOPEN epoch=%u%n",&epoch,&end)==1 && end && !line[end]) {
            if(epoch==x_epoch && epoch>mp_epoch) {
                seat_reservation_clear_locked(0);mp_epoch=sr_epoch=epoch;sr_revision=1;
                mp_request_locked();
            }
        } else if(sscanf(line,"MHELD epoch=%u seat=%u%n",&epoch,&a,&end)==2 && end && !line[end]) {
            if(epoch==mp_epoch && a==g_requested_seat && a>=1 && a<=4) mp_held=1;
        } else if(g_lobby_mode && !strncmp(line,"MROSTER",7)) {
            unsigned seats[MP_COUNT]={0};end=0;
            if(sscanf(line,"MROSTER epoch=%u rev=3 seat2=%u seat3=%u seat4=%u seat5=%u%n",&epoch,&seats[0],&seats[1],&seats[2],&seats[3],&end)!=5 || !end || line[end] ||
               epoch!=mp_epoch || epoch!=x_epoch || !mp_held || !mp_seats_valid(seats) || seats[mp_id-2]!=g_requested_seat || (sr_revision==3 && memcmp(mp_seats,seats,sizeof(seats))))exchange_fault_locked("multi_invalid_roster");
            else {
                memcpy(mp_seats,seats,sizeof(seats));sr_reserved=seats[mp_id-2];sr_revision=3;
                char ack[96];_snprintf_s(ack,sizeof(ack),_TRUNCATE,"MACK epoch=%u rev=3 seat=%u\n",epoch,sr_reserved);exchange_send_locked(ack);
            }
        } else if(!g_lobby_mode && sscanf(line,"MROSTER epoch=%u rev=3 seat2=%u seat3=%u%n",&epoch,&a,&b,&end)==3 && end && !line[end]) {
            unsigned local=mp_id==2?a:b;
            if(epoch!=mp_epoch || epoch!=x_epoch || !mp_held || a<1 || a>4 || b<1 || b>4 || a==b || local!=g_requested_seat ||
               (sr_revision==3 && (mp_seats[0]!=a || mp_seats[1]!=b))) exchange_fault_locked("multi_invalid_roster");
            else {
                mp_seats[0]=a;mp_seats[1]=b;sr_reserved=local;sr_revision=3;
                char ack[96];_snprintf_s(ack,sizeof(ack),_TRUNCATE,"MACK epoch=%u rev=3 seat=%u\n",epoch,local);
                exchange_send_locked(ack);
            }
        } else if(sscanf(line,"MGO epoch=%u rev=3%n",&epoch,&end)==1 && end && !line[end]) {
            if(epoch==mp_epoch && sr_revision==3 && mp_seats_valid(mp_seats)) sr_confirmed=1;
        } else if(sscanf(line,"MREJECT epoch=%u reason=occupied%n",&epoch,&end)==1 && end && !line[end]) {
            if(epoch==mp_epoch && !mp_held) {mp_rejected=1;sr_picker_choice=0;coop_log("multi: seat occupied; choose again");}
        } else exchange_fault_locked("multi_invalid_message");
    } else handled=0;
    LeaveCriticalSection(&g_net_lock);return handled;
}
static int __cdecl mp_options(void *L) {
    if(!g_two_guests) return 0;
    EnterCriticalSection(&g_net_lock);
    unsigned values[]={1,g_role_is_guest?sr_reserved:0,x_epoch,sr_revision,mp_seats[0],mp_seats[1],
        seat_control_enabled_locked(),mp_held,mp_rejected,mp_seats[2],mp_seats[3],(unsigned)mp_count()};
    LeaveCriticalSection(&g_net_lock);
    for(int i=0;i<12;++i)p_lua_pushinteger(L,values[i]);return 12;
}
// The network thread attributes complete lines to a socket under the lock.
// One blocking reader per client would starve the other guest's input.
static void mp_server_loop(SOCKET listener) {
    ++x_connection_generation;
    for(;;) {
        fd_set readset;FD_ZERO(&readset);FD_SET(listener,&readset);
        for(int i=0;i<mp_count();++i)if(mp_peers[i].socket!=INVALID_SOCKET)FD_SET(mp_peers[i].socket,&readset);
        struct timeval poll={0,50000};
        if(select(0,&readset,NULL,NULL,g_lobby_mode?&poll:NULL)==SOCKET_ERROR)break;
        if(g_lobby_mode){EnterCriticalSection(&g_net_lock);lb_tick_locked();LeaveCriticalSection(&g_net_lock);if(lb_phase==4 || lb_phase==0)break;}
        if(FD_ISSET(listener,&readset)) {
            SOCKET c=accept(listener,NULL,NULL);
            if(c!=INVALID_SOCKET) {
                EnterCriticalSection(&g_net_lock);
                int slot=-1;for(int i=0;i<mp_count();++i)if(mp_peers[i].socket==INVALID_SOCKET && slot<0)slot=i;
                if(slot<0 || mp_stopped || x_fault || (g_lobby_mode && lb_phase>=2)) closesocket(c);
                else {
                    MultiPeer *p=&mp_peers[slot];memset(p,0,sizeof(*p));p->socket=c;
                    DWORD timeout=1000;setsockopt(c,SOL_SOCKET,SO_SNDTIMEO,(const char *)&timeout,sizeof(timeout));
                    if(g_client_sock==INVALID_SOCKET)g_client_sock=c;
                    char welcome[64];_snprintf_s(welcome,sizeof(welcome),_TRUNCATE,"MWELCOME peer=%d\n",slot+2);
                    if(g_lobby_mode)_snprintf_s(welcome,sizeof(welcome),_TRUNCATE,"MWELCOME peer=%d guests=%d\n",slot+2,mp_count());
                    mp_send_locked(slot,welcome);mp_send_locked(slot,g_lobby_mode?"XHELLO version=" PN2_LOBBY_PROTOCOL_TEXT "\n":g_presentation_sync ? (g_voice_sync?"XHELLO version=9\n":"XHELLO version=8\n") : "XHELLO version=7\n");
                    if(g_lobby_mode)lb_connected_locked(slot);
                    coop_log("multi: connected peer=%d",slot+2);
                }
                LeaveCriticalSection(&g_net_lock);
            }
        }
        for(int j=0;j<mp_count();++j) {
            MultiPeer *p=&mp_peers[j];SOCKET c=p->socket;
            if(c==INVALID_SOCKET || !FD_ISSET(c,&readset))continue;
            int n=recv(c,p->rx+p->rxn,sizeof(p->rx)-1-p->rxn,0);
            EnterCriticalSection(&g_net_lock);
            if(n<=0) {
              if(g_lobby_mode && lb_phase>0 && lb_phase<3) {
                closesocket(c);p->socket=INVALID_SOCKET;p->hello=p->ready=0;p->rxn=0;p->session_seat=0;
                if(g_client_sock==c){g_client_sock=INVALID_SOCKET;for(int k=0;k<mp_count();++k)if(mp_peers[k].socket!=INVALID_SOCKET){g_client_sock=mp_peers[k].socket;break;}}
                lb_disconnected_locked(j);
              } else {
                mp_stopped=1;exchange_fault_locked("multi_peer_disconnected");
                closesocket(c);p->socket=INVALID_SOCKET;p->hello=p->ready=0;
                x_peer=x_peer_ready=0;coop_log("multi: disconnected peer=%d; restart all copies",j+2);
                if(g_lobby_mode){lb_phase=4;lb_error=5;}
              }
            } else {
                p->rxn+=n;p->rx[p->rxn]=0;int start=0;
                mp_source=j;
                for(int k=0;k<p->rxn;++k)if(p->rx[k]=='\n' || p->rx[k]=='\r') {
                    p->rx[k]=0;if(k>start)net_handle_line(p->rx+start);start=k+1;
                }
                mp_source=-1;
                if(start) {memmove(p->rx,p->rx+start,p->rxn-start);p->rxn-=start;}
                if(p->rxn==(int)sizeof(p->rx)-1)exchange_fault_locked("multi_line_too_long");
            }
            LeaveCriticalSection(&g_net_lock);
        }
    }
    EnterCriticalSection(&g_net_lock);mp_stopped=1;if(!g_lobby_mode || lb_phase!=0)exchange_fault_locked("multi_listener_stopped");
    for(int i=0;i<mp_count();++i)if(mp_peers[i].socket!=INVALID_SOCKET){closesocket(mp_peers[i].socket);mp_peers[i].socket=INVALID_SOCKET;}
    g_client_sock=INVALID_SOCKET;LeaveCriticalSection(&g_net_lock);
}
