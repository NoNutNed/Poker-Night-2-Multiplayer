// UDP discovery is advisory only; TCP lobby state owns membership and start.
#define LD_CAP 16
typedef struct {char ip[32];unsigned port,count,connected;ULONGLONG seen;} LobbyListing;
static LobbyListing ld_rows[LD_CAP];
static volatile LONG ld_request;
static ULONGLONG ld_auto_at;
static int __cdecl lb_scan(void *L){(void)L;EnterCriticalSection(&g_net_lock);ld_auto_at=GetTickCount64()+2000;InterlockedIncrement(&ld_request);LeaveCriticalSection(&g_net_lock);return 0;}
static int __cdecl lb_found(void *L) {
    const char *arg=p_lua_tolstring(L,1,NULL);int index=arg?atoi(arg)-1:-1;LobbyListing row;int found=0;
    EnterCriticalSection(&g_net_lock);
    if(GetTickCount64()>=ld_auto_at){ld_auto_at=GetTickCount64()+2000;InterlockedIncrement(&ld_request);}
    if(index>=0 && index<LD_CAP && ld_rows[index].seen && GetTickCount64()-ld_rows[index].seen<5000){row=ld_rows[index];found=1;}
    LeaveCriticalSection(&g_net_lock);
    if(!found)return 0;
    p_lua_pushstring(L,row.ip);p_lua_pushinteger(L,row.port);p_lua_pushinteger(L,row.count);p_lua_pushinteger(L,row.connected);return 4;
}
static int ld_parse(const char *line,unsigned query,LobbyListing *row) {
    unsigned version=0,received=0;int end=0;
    if(!seat_reservation_numbers_valid(line) || sscanf(line,"PN2LOBBY version=%u query=%u port=%u guests=%u connected=%u%n",
       &version,&received,&row->port,&row->count,&row->connected,&end)!=5 || !end || line[end] || version!=PN2_LOBBY_PROTOCOL ||
       received!=query || !query || !row->port || row->port>65535 || row->count<1 || row->count>4 || row->connected>row->count)return 0;
    return 1;
}
static void ld_record_locked(const LobbyListing *row) {
    int slot=-1;
    for(int i=0;i<LD_CAP;++i)if(ld_rows[i].seen && !strcmp(ld_rows[i].ip,row->ip) && ld_rows[i].port==row->port){slot=i;break;}
    if(slot<0)for(int i=0;i<LD_CAP;++i)if(!ld_rows[i].seen || GetTickCount64()-ld_rows[i].seen>=5000){slot=i;break;}
    if(slot>=0)ld_rows[slot]=*row;
}
static DWORD WINAPI lb_discovery_thread(LPVOID unused) {
    (void)unused;WSADATA data;if(WSAStartup(MAKEWORD(2,2),&data))return 1;
    SOCKET querySocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP),advertise=INVALID_SOCKET;
    if(querySocket==INVALID_SOCKET){WSACleanup();return 1;}
    BOOL yes=TRUE;setsockopt(querySocket,SOL_SOCKET,SO_BROADCAST,(const char*)&yes,sizeof(yes));
    struct sockaddr_in any;memset(&any,0,sizeof(any));any.sin_family=AF_INET;
    bind(querySocket,(struct sockaddr*)&any,sizeof(any));
    unsigned query=0;LONG seenRequest=0;
    for(;;) {
        EnterCriticalSection(&g_net_lock);int hosting=lb_phase==1 && !g_role_is_guest;LeaveCriticalSection(&g_net_lock);
        if(hosting && advertise==INVALID_SOCKET) {
            advertise=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);any.sin_port=htons(47104);
            if(advertise!=INVALID_SOCKET && bind(advertise,(struct sockaddr*)&any,sizeof(any))){closesocket(advertise);advertise=INVALID_SOCKET;}
        } else if(!hosting && advertise!=INVALID_SOCKET){closesocket(advertise);advertise=INVALID_SOCKET;}
        LONG request=InterlockedCompareExchange(&ld_request,0,0);
        if(request!=seenRequest) {
            seenRequest=request;query=(unsigned)(GetTickCount64()%0x7ffffffe)+1;char line[96];
            sprintf_s(line,sizeof(line),"PN2FIND version=" PN2_LOBBY_PROTOCOL_TEXT " query=%u",query);
            struct sockaddr_in destination;memset(&destination,0,sizeof(destination));destination.sin_family=AF_INET;destination.sin_port=htons(47104);destination.sin_addr.s_addr=INADDR_BROADCAST;
            sendto(querySocket,line,(int)strlen(line),0,(struct sockaddr*)&destination,sizeof(destination));
            destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);sendto(querySocket,line,(int)strlen(line),0,(struct sockaddr*)&destination,sizeof(destination));
        }
        fd_set sockets;FD_ZERO(&sockets);FD_SET(querySocket,&sockets);if(advertise!=INVALID_SOCKET)FD_SET(advertise,&sockets);
        struct timeval timeout={0,50000};
        if(select(0,&sockets,NULL,NULL,&timeout)<=0)continue;
        SOCKET candidates[2]={querySocket,advertise};
        for(int i=0;i<2;++i)if(candidates[i]!=INVALID_SOCKET && FD_ISSET(candidates[i],&sockets)) {
            char line[192];struct sockaddr_in from;int size=sizeof(from),length=recvfrom(candidates[i],line,sizeof(line)-1,0,(struct sockaddr*)&from,&size);
            if(length<=0)continue;line[length]=0;
            if((int)strlen(line)!=length)continue;
            if(i==1) {
                unsigned version=0,q=0;int end=0;
                if(!seat_reservation_numbers_valid(line) || sscanf(line,"PN2FIND version=%u query=%u%n",&version,&q,&end)!=2 || !end || line[end] || version!=PN2_LOBBY_PROTOCOL || !q)continue;
                EnterCriticalSection(&g_net_lock);
                int current=lb_phase==1 && !g_role_is_guest;unsigned count=0;
                for(int j=0;j<mp_count();++j)if(lb_connected&(1u<<j))++count;
                sprintf_s(line,sizeof(line),"PN2LOBBY version=" PN2_LOBBY_PROTOCOL_TEXT " query=%u port=%d guests=%d connected=%u",q,g_host_port,mp_count(),count);
                LeaveCriticalSection(&g_net_lock);
                if(current)sendto(advertise,line,(int)strlen(line),0,(struct sockaddr*)&from,sizeof(from));
            } else {
                LobbyListing row;memset(&row,0,sizeof(row));
                if(ld_parse(line,query,&row) && InetNtopA(AF_INET,&from.sin_addr,row.ip,sizeof(row.ip))) {
                    row.seen=GetTickCount64();EnterCriticalSection(&g_net_lock);ld_record_locked(&row);LeaveCriticalSection(&g_net_lock);
                }
            }
        }
    }
}

