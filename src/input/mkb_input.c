/*
 * mkb_input.c -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * A hidden window on its own thread registers for Raw Input with
 * RIDEV_INPUTSINK, so mouse deltas and key presses arrive whether or
 * not the game's own window is the one with focus, and no matter which
 * window the runtime happens to present through (the GDI debug window,
 * the FMV window, a D3D11 swap chain -- the input layer should not
 * have to care).
 *
 * The poll side drains the accumulated deltas and turns them into a
 * stick deflection that is proportional to the movement since the last
 * poll: the title integrates deflection over its own frame, so the
 * total rotation comes out proportional to total mouse travel at any
 * frame rate, which is the property mouse aim has to keep.
 *
 * Small movements get a floor of RECOMP_MKB_DZ of full throw, because
 * every console title applies its own dead zone to the stick, and a
 * mouse correction smaller than it would be silently eaten.
 */

/* Raw input needs a modern _WIN32_WINNT; the toolkit's platform header
 * includes windows.h first, so these have to land before anything else. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define _CRT_SECURE_NO_WARNINGS

#include "mkb_input.h"

#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ---- configuration ----------------------------------------------------- */

typedef struct {
    double sens;        /* multiplier on the aim deflection          */
    double dz;          /* assumed game deadzone, 0..1               */
    double acc;         /* aim acceleration exponent, 1 = linear     */
    long   full;        /* mouse counts for full throw, per 16 ms    */
    int    invert_y;    /* invert the vertical aim axis              */
    int    swap_sticks; /* mouse aims with the left stick instead    */
} MkbConfig;

static MkbConfig s_cfg = { 1.0, 0.18, 1.0, 8, 0, 0 };

/* ---- bindings ---------------------------------------------------------- */

/* An input is a virtual-key code (1-255), a mouse button (MOUSE_BTN + 1..5)
 * or a wheel direction. Left and right Shift/Ctrl/Alt are told apart by the
 * raw input handler, so VK_SHIFT and friends mean "either". */
#define MOUSE_BTN   0x100
#define WHEEL_UP    0x110
#define WHEEL_DOWN  0x111
#define MAX_BIND    4

static const char *const CONTROL_NAMES[MKB_NCONTROLS] = {
    "A", "B", "X", "Y", "Black", "White", "LeftTrigger", "RightTrigger",
    "Start", "Back", "LeftStickClick", "RightStickClick",
    "DpadUp", "DpadDown", "DpadLeft", "DpadRight",
    "MoveForward", "MoveBack", "MoveLeft", "MoveRight",
    "CameraUp", "CameraDown", "CameraLeft", "CameraRight",
};

static const struct { const char *name; int code; } INPUT_NAMES[] = {
    { "Space", VK_SPACE }, { "Enter", VK_RETURN }, { "Esc", VK_ESCAPE }, { "Escape", VK_ESCAPE },
    { "Tab", VK_TAB }, { "Backspace", VK_BACK }, { "CapsLock", VK_CAPITAL },
    { "Shift", VK_SHIFT }, { "LShift", VK_LSHIFT }, { "RShift", VK_RSHIFT },
    { "Ctrl", VK_CONTROL }, { "LCtrl", VK_LCONTROL }, { "RCtrl", VK_RCONTROL },
    { "Alt", VK_MENU }, { "LAlt", VK_LMENU }, { "RAlt", VK_RMENU },
    { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
    { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME }, { "End", VK_END },
    { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT },
    { "Mouse1", MOUSE_BTN + 1 }, { "Mouse2", MOUSE_BTN + 2 }, { "Mouse3", MOUSE_BTN + 3 },
    { "Mouse4", MOUSE_BTN + 4 }, { "Mouse5", MOUSE_BTN + 5 },
    { "WheelUp", WHEEL_UP }, { "WheelDown", WHEEL_DOWN },
};

/* The built-in layout, kept from the first title this layer served: a
 * shooter, mouse buttons on the triggers. A game ships its own in mkb.ini. */
static int s_bind[MKB_NCONTROLS][MAX_BIND] = {
    [MKB_A]          = { VK_SPACE },
    [MKB_B]          = { 'E' },
    [MKB_X]          = { 'R' },
    [MKB_Y]          = { MOUSE_BTN + 3, WHEEL_UP, WHEEL_DOWN },
    [MKB_BLACK]      = { 'F', MOUSE_BTN + 5 },
    [MKB_WHITE]      = { 'Q', MOUSE_BTN + 4 },
    [MKB_LT]         = { MOUSE_BTN + 2 },
    [MKB_RT]         = { MOUSE_BTN + 1 },
    [MKB_START]      = { VK_RETURN, VK_ESCAPE },
    [MKB_BACK]       = { VK_TAB },
    [MKB_LS]         = { VK_LSHIFT },
    [MKB_RS]         = { VK_LCONTROL },
    [MKB_DUP]        = { VK_UP },
    [MKB_DDOWN]      = { VK_DOWN },
    [MKB_DLEFT]      = { VK_LEFT },
    [MKB_DRIGHT]     = { VK_RIGHT },
    [MKB_MOVE_UP]    = { 'W' },
    [MKB_MOVE_DOWN]  = { 'S' },
    [MKB_MOVE_LEFT]  = { 'A' },
    [MKB_MOVE_RIGHT] = { 'D' },
};

static int input_code(const char *name)
{
    size_t i;
    if (!name[0])
        return 0;
    if (!name[1]) {
        int c = toupper((unsigned char)name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return c;
    }
    if ((name[0] == 'F' || name[0] == 'f') && atoi(name + 1) >= 1 && atoi(name + 1) <= 12)
        return VK_F1 + atoi(name + 1) - 1;
    if (!_strnicmp(name, "Num", 3) && name[3] >= '0' && name[3] <= '9' && !name[4])
        return VK_NUMPAD0 + name[3] - '0';
    for (i = 0; i < sizeof INPUT_NAMES / sizeof *INPUT_NAMES; i++)
        if (!_stricmp(name, INPUT_NAMES[i].name))
            return INPUT_NAMES[i].code;
    return -1;
}

static const char *input_name(int code)
{
    static char buf[8];
    size_t i;
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) {
        buf[0] = (char)code;
        buf[1] = 0;
        return buf;
    }
    if (code >= VK_F1 && code <= VK_F12) {
        snprintf(buf, sizeof buf, "F%d", code - VK_F1 + 1);
        return buf;
    }
    if (code >= VK_NUMPAD0 && code <= VK_NUMPAD9) {
        snprintf(buf, sizeof buf, "Num%d", code - VK_NUMPAD0);
        return buf;
    }
    for (i = 0; i < sizeof INPUT_NAMES / sizeof *INPUT_NAMES; i++)
        if (INPUT_NAMES[i].code == code)
            return INPUT_NAMES[i].name;
    return NULL;
}

/* "A = Space, Enter": returns 1 when the key names a control. */
static int mkb_apply_binding(const char *key, char *val)
{
    int c, n = 0;
    char *tok, *ctx = NULL;

    for (c = 0; c < MKB_NCONTROLS && _stricmp(key, CONTROL_NAMES[c]); c++)
        ;
    if (c == MKB_NCONTROLS)
        return 0;
    memset(s_bind[c], 0, sizeof s_bind[c]);
    for (tok = strtok_s(val, ", \t", &ctx); tok && n < MAX_BIND; tok = strtok_s(NULL, ", \t", &ctx)) {
        int code = input_code(tok);
        if (code > 0)
            s_bind[c][n++] = code;
        else
            fprintf(stderr, "  [MKB] mkb.ini: unknown input \"%s\" for %s\n", tok, key);
    }
    return 1;
}

/* One "key=value" line; returns 1 when the key was recognised. */
static int mkb_apply_kv(const char *key, const char *val)
{
    double d;
    long   l;

    if (!strcmp(key, "sens")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.sens = d;
    } else if (!strcmp(key, "dz")) {
        d = strtod(val, NULL); if (d >= 0.0 && d < 0.9) s_cfg.dz = d;
    } else if (!strcmp(key, "acc")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.acc = d;
    } else if (!strcmp(key, "full")) {
        l = strtol(val, NULL, 0); if (l > 0 && l < 1000) s_cfg.full = l;
    } else if (!strcmp(key, "invert_y")) {
        s_cfg.invert_y = atoi(val) != 0;
    } else if (!strcmp(key, "swap_sticks")) {
        s_cfg.swap_sticks = atoi(val) != 0;
    } else {
        return 0;
    }
    return 1;
}

static void mkb_load_config(void)
{
    static const char *kv[] = {
        "RECOMP_MKB_SENS", "sens",
        "RECOMP_MKB_DZ",   "dz",
        "RECOMP_MKB_ACC",  "acc",
        "RECOMP_MKB_FULL", "full",
        "RECOMP_MKB_INVERT_Y",  "invert_y",
        "RECOMP_MKB_SWAP_STICKS", "swap_sticks",
        NULL
    };
    char path[MAX_PATH];
    const char *ini;
    DWORD n;
    FILE *f;
    int i;

    for (i = 0; kv[i]; i += 2) {
        const char *v = getenv(kv[i]);
        if (v && *v)
            mkb_apply_kv(kv[i + 1], v);
    }

    ini = getenv("RECOMP_MKB_INI");
    if (ini && *ini) {
        snprintf(path, sizeof path, "%s", ini);
    } else {
        n = GetModuleFileNameA(NULL, path, sizeof path - 12);
        if (!n || n >= sizeof path - 12)
            return;
        snprintf(path + n, 12, "\\..\\mkb.ini");   /* beside the .exe */
    }
    f = fopen(path, "r");
    if (!f)
        return;
    {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            char *k, *v, *semi;
            if (!eq || line[strspn(line, " \t")] == ';' || line[strspn(line, " \t")] == '#')
                continue;
            *eq = 0;
            k = line; v = eq + 1;
            semi = strchr(v, ';');          /* trailing comment */
            if (semi)
                *semi = 0;
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            k[strcspn(k, " \t\r\n")] = 0;
            v[strcspn(v, "\r\n")] = 0;
            if (*k && !mkb_apply_binding(k, v)) {
                v[strcspn(v, " \t")] = 0;
                if (*v)
                    mkb_apply_kv(k, v);
            }
        }
    }
    fclose(f);
}

static void mkb_config_once(void)
{
    static LONG done;
    if (InterlockedCompareExchange(&done, 1, 0) == 0)
        mkb_load_config();
}

const char *mkb_binding_label(int control)
{
    if (control < 0 || control >= MKB_NCONTROLS)
        return NULL;
    mkb_config_once();
    return s_bind[control][0] ? input_name(s_bind[control][0]) : NULL;
}

/* ---- raw input thread -------------------------------------------------- */

/* Accumulators, written by the input thread and drained by the poll.
 * InterlockedExchange is the whole protocol: the input thread adds
 * with InterlockedAdd, the poll takes everything with Exchange(0). */
static volatile LONG s_dx, s_dy;
static volatile LONG s_wheel;        /* wheel steps since last poll   */
static volatile LONG s_mouse_btn;    /* 1 L, 2 R, 4 M, 8 X1, 16 X2    */
static volatile LONG s_mouse_tap;    /* pressed since the last poll   */
static volatile unsigned char s_vk_down[256];
static volatile unsigned char s_vk_tap[256];   /* pressed since the last poll */

/* -1 until known, then 1 keyboard/mouse, 0 controller. */
static volatile LONG s_last_kb = -1;

static int game_is_foreground(void);

void mkb_note_pad_activity(void)
{
    if (s_last_kb != 0)
        InterlockedExchange(&s_last_kb, 0);
}

static volatile LONG s_presses;

unsigned mkb_activity(void)
{
    return (unsigned)s_presses;
}

static void note_kb_activity(void)
{
    if (s_last_kb != 1 && game_is_foreground())
        InterlockedExchange(&s_last_kb, 1);
}

int mkb_last_device_keyboard(void)
{
    if (s_last_kb < 0) {
        /* Nothing touched yet: a controller if one is plugged in. */
        XINPUT_STATE st;
        DWORD i;
        LONG kb = 1;
        for (i = 0; i < 4; i++)
            if (XInputGetState(i, &st) == ERROR_SUCCESS)
                kb = 0;
        InterlockedCompareExchange(&s_last_kb, kb, -1);
    }
    return s_last_kb == 1;
}

static volatile LONG s_active = 1;   /* F9 toggles this               */
static volatile LONG s_thread_up;

/* F9 is read on the input thread, so the toggle works even while the
 * game is stalled on the poll side. */
#define MKB_TOGGLE_VK  VK_F9

static void mkb_handle_raw(HRAWINPUT h)
{
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof buf;
    RAWINPUT *ri = (RAWINPUT *)buf;

    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER))
            == (UINT)-1)
        return;

    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE *m = &ri->data.mouse;

        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (m->lLastX) InterlockedAdd(&s_dx, m->lLastX);
            if (m->lLastY) InterlockedAdd(&s_dy, m->lLastY);
            if (labs(m->lLastX) + labs(m->lLastY) > 6)   /* not a desk bump */
                note_kb_activity();
        }
        if (m->usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
                                RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN |
                                RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_WHEEL))
            note_kb_activity();
        if (m->usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
                                RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN |
                                RI_MOUSE_BUTTON_5_DOWN) && game_is_foreground())
            InterlockedIncrement(&s_presses);
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)   InterlockedOr(&s_mouse_tap, 1);
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)  InterlockedOr(&s_mouse_tap, 2);
        if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) InterlockedOr(&s_mouse_tap, 4);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_4_DOWN)      InterlockedOr(&s_mouse_tap, 8);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_5_DOWN)      InterlockedOr(&s_mouse_tap, 16);
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
            InterlockedOr(&s_mouse_btn, 1);
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~1);
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
            InterlockedOr(&s_mouse_btn, 2);
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~2);
        if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
            InterlockedOr(&s_mouse_btn, 4);
        if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~4);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_4_DOWN)
            InterlockedOr(&s_mouse_btn, 8);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_4_UP)
            InterlockedAnd(&s_mouse_btn, ~8);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_5_DOWN)
            InterlockedOr(&s_mouse_btn, 16);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_5_UP)
            InterlockedAnd(&s_mouse_btn, ~16);
        if (m->usButtonFlags & RI_MOUSE_WHEEL) {
            SHORT d = (SHORT)m->usButtonData;
            InterlockedAdd(&s_wheel, d / WHEEL_DELTA);
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri->data.keyboard;
        UINT vk = k->VKey;

        /* Raw input reports the generic modifier; bindings can name a side. */
        if (vk == VK_SHIFT)
            vk = MapVirtualKeyA(k->MakeCode, MAPVK_VSC_TO_VK_EX);
        else if (vk == VK_CONTROL)
            vk = (k->Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU)
            vk = (k->Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
        if (k->Message == WM_KEYDOWN || k->Message == WM_SYSKEYDOWN) {
            note_kb_activity();
            if (!s_vk_down[vk & 0xFF] && game_is_foreground()) {
                fprintf(stderr, "  [MKB] key %02X down", vk);
                fputc(10, stderr);
            }
            if (!s_vk_down[vk & 0xFF] && game_is_foreground())
                InterlockedIncrement(&s_presses);
            if (vk && vk < 256)
                s_vk_tap[vk] = 1;
        }

        if (vk == MKB_TOGGLE_VK) {
            if (k->Message == WM_KEYDOWN && !s_vk_down[vk & 0xFF]) {
                LONG now = !s_active;
                InterlockedExchange(&s_active, now);
                fprintf(stderr, "  [MKB] %s\n", now ? "captured"
                                                    : "released (F9)");
                fflush(stderr);
            }
        }
        if (vk && vk < 256) {
            if (k->Message == WM_KEYDOWN || k->Message == WM_SYSKEYDOWN)
                s_vk_down[vk] = 1;
            else
                s_vk_down[vk] = 0;
        }
    }
}

static LRESULT CALLBACK mkb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        mkb_handle_raw((HRAWINPUT)l);
        return 0;
    }
    if (m == WM_CLOSE || m == WM_DESTROY)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI mkb_thread(LPVOID unused)
{
    WNDCLASSA wc;
    HWND hwnd;
    RAWINPUTDEVICE rid[2];
    MSG msg;

    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = mkb_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "XboxRecompMKB";
    RegisterClassA(&wc);
    /* Not visible: it exists to own the WM_INPUT delivery, nothing else. */
    hwnd = CreateWindowExA(0, "XboxRecompMKB", "mkb", WS_OVERLAPPED,
                           0, 0, 0, 0, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        fprintf(stderr, "  [MKB] window creation failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;  rid[0].usUsage = 0x02;  /* mouse     */
    rid[1].usUsagePage = 0x01;  rid[1].usUsage = 0x06;  /* keyboard  */
    rid[0].dwFlags = RIDEV_INPUTSINK;   /* deliver without focus */
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fprintf(stderr, "  [MKB] RegisterRawInputDevices failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    InterlockedExchange(&s_thread_up, 1);
    fprintf(stderr, "  [MKB] mouse+keyboard captured (F9 releases; "
            "sens=%.2f dz=%.2f full=%ld)\n",
            s_cfg.sens, s_cfg.dz, s_cfg.full);
    fflush(stderr);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    InterlockedExchange(&s_thread_up, 0);
    return 0;
}

/* ---- poll side --------------------------------------------------------- */

int mkb_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        /* Opt-in, like RECOMP_KEYBOARD: a keyboard silently acting as
         * player 1 is surprising when a real pad is what you meant to use.
         * RECOMP_MKB=1 turns the layer on. */
        const char *v = getenv("RECOMP_MKB");
        on = v && *v && *v != '0';
        if (on) {
            mkb_config_once();
            if (InterlockedCompareExchange(&s_thread_up, 0, 0) == 0) {
                static LONG started = 0;
                if (InterlockedCompareExchange(&started, 1, 0) == 0)
                    CloseHandle(CreateThread(NULL, 0, mkb_thread,
                                             NULL, 0, NULL));
            }
        }
    }
    return on;
}

/* The recompiled game's process is the foreground one. Raw input with
 * INPUTSINK keeps arriving while the user is in another app, and the
 * mouse must go on behaving normally there. */
static int game_is_foreground(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Hide and confine the pointer while captured. Aiming with raw deltas
 * does not move the pointer by itself, but Windows still moves it from
 * the same physical movement, so it would wander across the screen and
 * end up clicking other apps' title bars. */
static void mkb_manage_cursor(int capture)
{
    static int hidden;          /* how many ShowCursor(FALSE) we owe  */
    static unsigned long last;

    unsigned long now = GetTickCount();
    if (now - last < 500 && (capture ? hidden : !hidden) == 0)
        return;
    last = now;

    if (capture) {
        /* The client area only: with the whole window rectangle the
         * pointer still reached the title bar. ShowCursor below counts
         * per thread and this runs on the game's poll thread, so it does
         * not hide anything over a window another thread owns; the window
         * procedure hides the pointer itself while mkb_captured() is true. */
        RECT r;
        HWND fg = GetForegroundWindow();
        if (fg && GetClientRect(fg, &r)) {
            MapWindowPoints(fg, NULL, (POINT *)&r, 2);
            ClipCursor(&r);
        }
        if (hidden < 5) { ShowCursor(FALSE); hidden++; }
    } else {
        if (hidden > 0) { ShowCursor(TRUE); hidden--; }
        ClipCursor(NULL);
    }
}

int mkb_captured(void)
{
    return mkb_enabled() && s_thread_up && s_active && game_is_foreground();
}

/* Radial deadzone compensation: an input radius in [0..1] comes out as
 * [dz..1], so the smallest mouse correction still clears the title's
 * own deadzone, and the response stays linear above it. */
static void mkb_aim_vector(double ax, double ay, SHORT *out_x, SHORT *out_y)
{
    double r = sqrt(ax * ax + ay * ay);
    double len, fx, fy;
    long v;

    if (r <= 0.0) { *out_x = 0; *out_y = 0; return; }
    if (r > 1.0) r = 1.0;
    len = s_cfg.dz + (1.0 - s_cfg.dz) * pow(r, s_cfg.acc);
    len *= s_cfg.sens;
    if (len > 1.0) len = 1.0;
    fx = ax * len / sqrt(ax * ax + ay * ay);
    fy = ay * len / sqrt(ax * ax + ay * ay);
    v = (long)(fx * 32767.0);
    *out_x = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = (long)(fy * 32767.0);
    *out_y = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Held now, or pressed and released since the last poll (a tap shorter
 * than a frame still reaches the title). VK_SHIFT, VK_CONTROL and VK_MENU
 * mean either side. */
static int vk_held(int vk)
{
    if (vk == VK_SHIFT)   return vk_held(VK_LSHIFT) || vk_held(VK_RSHIFT);
    if (vk == VK_CONTROL) return vk_held(VK_LCONTROL) || vk_held(VK_RCONTROL);
    if (vk == VK_MENU)    return vk_held(VK_LMENU) || vk_held(VK_RMENU);
    return (unsigned)vk < 256 && (s_vk_down[vk] || s_vk_tap[vk]);
}

static int control_held(int c, LONG btn, LONG wheel)
{
    int i;
    for (i = 0; i < MAX_BIND && s_bind[c][i]; i++) {
        int code = s_bind[c][i];
        if (code < 256 ? vk_held(code)
            : code == WHEEL_UP ? wheel > 0
            : code == WHEEL_DOWN ? wheel < 0
            : (btn >> (code - MOUSE_BTN - 1)) & 1)
            return 1;
    }
    return 0;
}

int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    static const struct { int control; WORD bit; } DIGITAL[] = {
        { MKB_START, XBOX_GAMEPAD_START }, { MKB_BACK, XBOX_GAMEPAD_BACK },
        { MKB_LS, XBOX_GAMEPAD_LEFT_THUMB }, { MKB_RS, XBOX_GAMEPAD_RIGHT_THUMB },
        { MKB_DUP, XBOX_GAMEPAD_DPAD_UP }, { MKB_DDOWN, XBOX_GAMEPAD_DPAD_DOWN },
        { MKB_DLEFT, XBOX_GAMEPAD_DPAD_LEFT }, { MKB_DRIGHT, XBOX_GAMEPAD_DPAD_RIGHT },
    };
    static const struct { int control; int index; } ANALOG[] = {
        { MKB_A, XBOX_BUTTON_A }, { MKB_B, XBOX_BUTTON_B }, { MKB_X, XBOX_BUTTON_X },
        { MKB_Y, XBOX_BUTTON_Y }, { MKB_BLACK, XBOX_BUTTON_BLACK }, { MKB_WHITE, XBOX_BUTTON_WHITE },
        { MKB_LT, XBOX_BUTTON_LTRIGGER }, { MKB_RT, XBOX_BUTTON_RTRIGGER },
    };
    SHORT aim_x = 0, aim_y = 0, mov_x = 0, mov_y = 0;
    WORD b = 0;
    BYTE analog[8];
    LONG dx, dy, wheel, btn;
    int any = 0, i;
    static DWORD packet;

    if (!pState || dwPort != 0 || !mkb_enabled())
        return 0;
    if (!s_thread_up)
        return 0;

    if (!s_active) {
        mkb_manage_cursor(0);
        return 0;
    }

    /* Foreground gate: while the user is elsewhere the mouse belongs to
     * whoever has it, and the game should read a pad at rest. */
    if (!game_is_foreground()) {
        mkb_manage_cursor(0);
        InterlockedExchange(&s_dx, 0);
        InterlockedExchange(&s_dy, 0);
        return 0;
    }
    mkb_manage_cursor(1);

    /* Mouse travel is taken once per ~16 ms window and the same deflection
     * is returned to every poll inside it. Taking it per poll gave it all
     * to whichever read came first: a title that reads its pad more than
     * once a frame (Spyro does) used a later, centred read, and the mouse
     * did nothing. */
    {
        static LARGE_INTEGER win_start, freq;
        static LONG win_dx, win_dy;
        LARGE_INTEGER now;
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - win_start.QuadPart) * 1000 >= 16 * freq.QuadPart) {
            win_dx = InterlockedExchange(&s_dx, 0);
            win_dy = InterlockedExchange(&s_dy, 0);
            win_start = now;
        }
        dx = win_dx;
        dy = win_dy;
    }
    wheel = InterlockedExchange(&s_wheel, 0);
    btn   = s_mouse_btn | InterlockedExchange(&s_mouse_tap, 0);

    memset(analog, 0, sizeof analog);

    for (i = 0; i < (int)(sizeof ANALOG / sizeof *ANALOG); i++)
        if (control_held(ANALOG[i].control, btn, wheel))
            analog[ANALOG[i].index] = 255;
    for (i = 0; i < (int)(sizeof DIGITAL / sizeof *DIGITAL); i++)
        if (control_held(DIGITAL[i].control, btn, wheel))
            b |= DIGITAL[i].bit;

    /* ---- keyboard: movement ---- */
    {
        int mx = 0, my = 0;
        if (control_held(MKB_MOVE_UP, btn, wheel))    my += 1;
        if (control_held(MKB_MOVE_DOWN, btn, wheel))  my -= 1;
        if (control_held(MKB_MOVE_LEFT, btn, wheel))  mx -= 1;
        if (control_held(MKB_MOVE_RIGHT, btn, wheel)) mx += 1;
        if (mx && my) { mx = (int)(mx * 70.71); my = (int)(my * 70.71); }
        else          { mx *= 100; my *= 100; }
        if (mx || my) mkb_aim_vector(mx / 100.0, my / 100.0, &mov_x, &mov_y);
    }

    /* ---- mouse: aim ---- */
    if (dx || dy) {
        double ax =  (double)dx / (double)s_cfg.full;
        double ay = -(double)dy / (double)s_cfg.full;
        if (s_cfg.invert_y) ay = -ay;
        mkb_aim_vector(ax, ay, &aim_x, &aim_y);
    } else {
        /* Keys on the camera stick, full throw, when the mouse is still. */
        int cx = control_held(MKB_CAM_RIGHT, btn, wheel) - control_held(MKB_CAM_LEFT, btn, wheel);
        int cy = control_held(MKB_CAM_UP, btn, wheel) - control_held(MKB_CAM_DOWN, btn, wheel);
        aim_x = (SHORT)(cx * 32767);
        aim_y = (SHORT)(cy * 32767);
    }
    memset((void *)s_vk_tap, 0, sizeof s_vk_tap);

    /* ---- merge on top of whatever the host pad produced ---- */
    if (b)        { pState->Gamepad.wButtons |= b; any = 1; }
    for (i = 0; i < 8; i++) {
        if (analog[i]) {
            if (analog[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = analog[i];
            any = 1;
        }
    }
    if (aim_x || aim_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbLX = aim_x;
            pState->Gamepad.sThumbLY = aim_y;
        } else {
            pState->Gamepad.sThumbRX = aim_x;
            pState->Gamepad.sThumbRY = aim_y;
        }
        any = 1;
    }
    if (mov_x || mov_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbRX = mov_x;
            pState->Gamepad.sThumbRY = mov_y;
        } else {
            pState->Gamepad.sThumbLX = mov_x;
            pState->Gamepad.sThumbLY = mov_y;
        }
        any = 1;
    }

    /* Edge detection: a title polling for changes ignores a state whose
     * packet number did not move. */
    if (any)
        pState->dwPacketNumber = ++packet;
    return any;
}
