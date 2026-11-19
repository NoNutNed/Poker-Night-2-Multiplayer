// Capacity4; legacy protocols use2, opt-in lobby negotiates1..4. State under g_net_lock.
#define MP_COUNT 4
typedef struct {
    SOCKET socket;
    int hello, ready, ack;
    unsigned session_seat;
    char rx[2048];
    int rxn;
} MultiPeer;
static int g_two_guests, g_guest_instance=1;
static int g_lobby_mode, g_guest_count=2; // protocol10; legacy path still expects2
static int mp_count(void) { return g_lobby_mode?g_guest_count:2; }
static int mp_all(const int *values) { for(int i=0;i<mp_count();++i)if(!values[i])return 0;return 1; }
static int g_presentation_sync; // optional protocol8; exactly two guests
static int g_voice_sync; // optional protocol9 foreground voice completion
static void presentation_reset_locked(void);
static void voice_reset_locked(void);
static MultiPeer mp_peers[MP_COUNT]={{INVALID_SOCKET},{INVALID_SOCKET},{INVALID_SOCKET},{INVALID_SOCKET}};
static unsigned mp_seats[MP_COUNT], mp_id, mp_epoch;
static int mp_held, mp_rejected, mp_stopped;
static int mp_source=-1; // set only while processing an attributed socket line
static int mp_send_locked(int target,const char *line);
static void mp_clear_locked(int connection);
static void mp_open_locked(void);
static int mp_request_locked(void);
static int mp_handle_line(const char *line);
static int mp_scope_locked(unsigned epoch,unsigned revision,unsigned seat);
static int __cdecl mp_options(void *L);
static void mp_server_loop(SOCKET listener);
static void net_handle_line(const char *line);
static int lb_phase, lb_error;
static void lb_connected_locked(int slot);
static void lb_disconnected_locked(int slot);
static int lb_handle_line(const char *line);
static void lb_tick_locked(void);
