/* Preserve the native window while adding a monitor-sized windowed mode. */
static struct {
    HWND window;
    LONG_PTR style, extended_style;
    WINDOWPLACEMENT placement;
} borderless;

static BOOL display_set_style(HWND window, int index, LONG_PTR style) {
    SetLastError(ERROR_SUCCESS);
    return SetWindowLongPtrW(window, index, style) != 0 || GetLastError() == ERROR_SUCCESS;
}

static BOOL display_restore(void) {
    HWND window = borderless.window;
    if (!window) return TRUE;
    BOOL style = display_set_style(window, GWL_STYLE, borderless.style);
    BOOL extended = display_set_style(window, GWL_EXSTYLE, borderless.extended_style);
    BOOL placement = SetWindowPlacement(window, &borderless.placement);
    BOOL frame = SetWindowPos(window, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!(style && extended && placement && frame)) return FALSE;
    borderless.window = NULL;
    return TRUE;
}

static BOOL display_fit_monitor(void) {
    HWND window = borderless.window;
    MONITORINFO monitor = {sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return FALSE;
    const RECT *area = &monitor.rcMonitor;
    const LONG_PTR frame = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    const LONG_PTR edges = WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME;
    if (IsZoomed(window)) ShowWindow(window, SW_RESTORE);
    return display_set_style(window, GWL_STYLE, (borderless.style & ~(frame | WS_MAXIMIZE)) | WS_POPUP) &&
        display_set_style(window, GWL_EXSTYLE, borderless.extended_style & ~edges) &&
        SetWindowPos(window, NULL, area->left, area->top, area->right-area->left, area->bottom-area->top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static BOOL display_enter(HWND window) {
    DWORD owner = 0;
    if (!window || !GetWindowThreadProcessId(window, &owner) || owner != GetCurrentProcessId() ||
        GetWindow(window, GW_OWNER) || IsIconic(window)) return FALSE;
    if (borderless.window) return borderless.window == window;

    borderless.placement.length = sizeof(borderless.placement);
    if (!GetWindowPlacement(window, &borderless.placement)) return FALSE;
    borderless.style = GetWindowLongPtrW(window, GWL_STYLE);
    borderless.extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!borderless.style || (borderless.style & WS_CHILD)) return FALSE;
    borderless.window = window;
    if (display_fit_monitor()) return TRUE;
    display_restore();
    return FALSE;
}

static int __cdecl coop_display_window(void *L) {
    const char *action = p_lua_tolstring(L, 1, NULL);
    BOOL ok = FALSE;
    if (borderless.window && !IsWindow(borderless.window)) borderless.window = NULL;
    if (action && !strcmp(action, "active")) {
        ok = borderless.window != NULL;
    } else if (action && !strcmp(action, "enter")) {
        HWND window = (HWND)InterlockedCompareExchangePointer(&g_game_hwnd, NULL, NULL);
        if (!window || !IsWindow(window)) window = GetActiveWindow();
        ok = display_enter(window);
        coop_log("display: enter_borderless=%d", ok);
    } else if (action && !strcmp(action, "leave")) {
        ok = display_restore();
        coop_log("display: leave_borderless=%d", ok);
    } else if (action && !strcmp(action, "refresh")) {
        ok = borderless.window && display_fit_monitor();
    }
    p_lua_pushinteger(L, ok ? 1 : 0);
    return 1;
}
