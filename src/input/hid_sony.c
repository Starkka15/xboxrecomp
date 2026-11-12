/**
 * Sony controllers read straight from HID (Windows).
 *
 * XInput only sees Xbox-protocol pads, so a DualSense or DualShock 4 is
 * invisible to a game started outside Steam. This reads their HID reports
 * directly -- USB and Bluetooth, sticks, buttons, analog triggers -- and
 * drives their rumble motors, with nothing to install. xinput_device.c puts
 * them after the XInput pads.
 *
 * Report layouts (as in SDL's hidapi drivers and Linux hid-playstation):
 *
 *   DualShock 4  USB 0x01, BT 0x01 (basic) / 0x11 (full, data at +3)
 *     LX LY RX RY  hat|face  L1 R1 L2 R2 Share Options L3 R3  PS|pad  L2 R2
 *   DualSense    USB 0x01, BT 0x31 (full, data at +2)
 *     LX LY RX RY  L2 R2  counter  hat|face  buttons  PS|pad|mute
 *     Over Bluetooth it starts in the basic 0x01 report, laid out like the
 *     DualShock 4's.
 *
 * A Bluetooth pad switches to the full report when its calibration feature
 * report is read (0x05 DualSense, 0x02 DualShock 4); both kinds are handled.
 *
 * RECOMP_SONY_HID=0 turns this off (a remapper such as DS4Windows already
 * presents the pad as an Xbox one).
 */
#ifdef _WIN32

#include "hid_sony.h"
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

#define SONY_VID 0x054C

enum { KIND_DS4, KIND_DS };

typedef struct {
    volatile LONG  active;      /* reader thread running */
    int            kind, bt;
    HANDLE         h;
    wchar_t        path[512];
    USHORT         in_len, out_len, feat_len;
    SRWLOCK        lock;
    XINPUT_GAMEPAD pad;
    DWORD          packet;
    int            have;        /* a report has arrived */
    volatile LONG  rumble;      /* strong << 8 | weak, wanted */
    LONG           rumble_sent;
    BYTE           seq;
} SonyPad;

static SonyPad s_pads[SONY_MAX_PADS];
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static int s_off;

static int pad_kind(USHORT pid)
{
    switch (pid) {
    case 0x05C4: case 0x09CC: case 0x0BA0:   /* DualShock 4 v1, v2, USB dongle */
        return KIND_DS4;
    case 0x0CE6: case 0x0DF2:                /* DualSense, DualSense Edge */
        return KIND_DS;
    }
    return -1;
}

static DWORD crc32_bt(const BYTE *p, size_t n)
{
    DWORD c = 0xFFFFFFFFu;
    BYTE hdr = 0xA2;               /* Bluetooth HID output header */
    size_t i;
    int k;
    for (i = 0; i <= n; i++) {
        c ^= i == 0 ? hdr : p[i - 1];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static SHORT axis(BYTE v, int flip)
{
    int x = (int)v * 257 - 32768;          /* 0..255 -> -32768..32767 */
    return (SHORT)(flip ? -1 - x : x);     /* Sony Y grows downward */
}

/* Hat, face buttons and the rest, common to both pads. */
static void buttons(XINPUT_GAMEPAD *g, BYTE hatface, BYTE b, BYTE ps)
{
    static const WORD hat[9] = {
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT, 0
    };
    WORD w = (hatface & 0x0F) < 9 ? hat[hatface & 0x0F] : 0;
    if (hatface & 0x10) w |= XINPUT_GAMEPAD_X;              /* square */
    if (hatface & 0x20) w |= XINPUT_GAMEPAD_A;              /* cross */
    if (hatface & 0x40) w |= XINPUT_GAMEPAD_B;              /* circle */
    if (hatface & 0x80) w |= XINPUT_GAMEPAD_Y;              /* triangle */
    if (b & 0x01) w |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (b & 0x02) w |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (b & 0x10) w |= XINPUT_GAMEPAD_BACK;                 /* Share / Create */
    if (b & 0x20) w |= XINPUT_GAMEPAD_START;                /* Options */
    if (b & 0x40) w |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (b & 0x80) w |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (ps & 0x02) w |= XINPUT_GAMEPAD_BACK;                /* touchpad click */
    g->wButtons = w;
}

/* p at LX: LX LY RX RY hat|face buttons PS L2 R2 */
static void parse_ds4_layout(XINPUT_GAMEPAD *g, const BYTE *p)
{
    g->sThumbLX = axis(p[0], 0); g->sThumbLY = axis(p[1], 1);
    g->sThumbRX = axis(p[2], 0); g->sThumbRY = axis(p[3], 1);
    buttons(g, p[4], p[5], p[6]);
    g->bLeftTrigger = p[7];
    g->bRightTrigger = p[8];
}

/* p at LX: LX LY RX RY L2 R2 counter hat|face buttons PS */
static void parse_ds_layout(XINPUT_GAMEPAD *g, const BYTE *p)
{
    g->sThumbLX = axis(p[0], 0); g->sThumbLY = axis(p[1], 1);
    g->sThumbRX = axis(p[2], 0); g->sThumbRY = axis(p[3], 1);
    g->bLeftTrigger = p[4];
    g->bRightTrigger = p[5];
    buttons(g, p[7], p[8], p[9]);
}

static int parse(SonyPad *s, const BYTE *r, DWORD n, XINPUT_GAMEPAD *g)
{
    memset(g, 0, sizeof *g);
    if (s->kind == KIND_DS4) {
        if (r[0] == 0x01 && n >= 10) { parse_ds4_layout(g, r + 1); return 1; }
        if (r[0] == 0x11 && n >= 12) { parse_ds4_layout(g, r + 3); return 1; }
    } else {
        if (r[0] == 0x01 && !s->bt && n >= 11) { parse_ds_layout(g, r + 1); return 1; }
        if (r[0] == 0x01 && s->bt && n >= 10)  { parse_ds4_layout(g, r + 1); return 1; }
        if (r[0] == 0x31 && n >= 12)           { parse_ds_layout(g, r + 2); return 1; }
    }
    return 0;
}

static void send_rumble(SonyPad *s, LONG want)
{
    BYTE strong = (BYTE)(want >> 8), weak = (BYTE)want;
    BYTE *b;
    DWORD len = s->out_len, done;
    OVERLAPPED ov;

    if (len < 78 && s->bt)
        return;
    if (len < 32)
        return;
    b = (BYTE *)calloc(1, len);
    if (!b)
        return;
    if (s->kind == KIND_DS4 && !s->bt) {
        b[0] = 0x05; b[1] = 0x01; b[4] = weak; b[5] = strong;
    } else if (s->kind == KIND_DS4) {
        DWORD c;
        b[0] = 0x11; b[1] = 0xC0; b[2] = 0x20; b[3] = 0x01; b[6] = weak; b[7] = strong;
        c = crc32_bt(b, 74);
        memcpy(b + 74, &c, 4);
    } else {
        /* DualSense common block: flags (compatible vibration, haptics
         * select), flags, right (weak) motor, left (strong) motor. */
        BYTE *e;
        if (s->bt) {
            b[0] = 0x31;
            b[1] = (BYTE)(s->seq++ << 4);
            b[2] = 0x10;
            e = b + 3;
        } else {
            b[0] = 0x02;
            e = b + 1;
        }
        e[0] = 0x03; e[2] = weak; e[3] = strong;
        if (s->bt) {
            DWORD c = crc32_bt(b, 74);
            memcpy(b + 74, &c, 4);
        }
    }
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent) {
        if (WriteFile(s->h, b, len, &done, &ov) || GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 200) != WAIT_OBJECT_0) {
                CancelIoEx(s->h, &ov);
                fprintf(stderr, "  [sony] rumble write timed out");
                fputc(10, stderr);
            }
            GetOverlappedResult(s->h, &ov, &done, TRUE);
        } else {
            fprintf(stderr, "  [sony] rumble write failed, error %lu", GetLastError());
            fputc(10, stderr);
        }
        CloseHandle(ov.hEvent);
    }
    free(b);
    s->rumble_sent = want;
}

static DWORD WINAPI reader(LPVOID arg)
{
    SonyPad *s = (SonyPad *)arg;
    BYTE *buf = (BYTE *)calloc(1, s->in_len ? s->in_len : 64);
    OVERLAPPED ov;
    DWORD n;
    int pending = 0;
    DWORD reports = 0, beat = GetTickCount(), why = 0;
    WORD last_btn = 0xFFFF;
    BYTE last_lt = 0, last_rt = 0;

    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!buf || !ov.hEvent)
        goto out;

    /* Bluetooth: ask for the full report. */
    if (s->bt && s->feat_len) {
        BYTE *f = (BYTE *)calloc(1, s->feat_len);
        if (f) {
            f[0] = s->kind == KIND_DS ? 0x05 : 0x02;
            HidD_GetFeature(s->h, f, s->feat_len);
            free(f);
        }
    }

    for (;;) {
        DWORD w;
        LONG want;
        if (!pending) {
            ResetEvent(ov.hEvent);
            if (!ReadFile(s->h, buf, s->in_len, NULL, &ov) && GetLastError() != ERROR_IO_PENDING) {
                why = GetLastError();
                break;
            }
            pending = 1;
        }
        w = WaitForSingleObject(ov.hEvent, 50);
        if (w == WAIT_OBJECT_0) {
            XINPUT_GAMEPAD g;
            pending = 0;
            if (!GetOverlappedResult(s->h, &ov, &n, FALSE)) {
                why = GetLastError();
                break;
            }
            reports++;
            if (n && parse(s, buf, n, &g)) {
                /* Diagnostics (spyro.log with Log = 1): each change of the
                 * buttons, or a trigger crossing half way. */
                if (g.wButtons != last_btn || (g.bLeftTrigger > 127) != (last_lt > 127)
                        || (g.bRightTrigger > 127) != (last_rt > 127)) {
                    fprintf(stderr, "  [sony] id %d report %02X len %lu: buttons %04X LT %u RT %u",
                            (int)(s - s_pads), buf[0], n, g.wButtons, g.bLeftTrigger, g.bRightTrigger);
                    fputc(10, stderr);
                    last_btn = g.wButtons;
                    last_lt = g.bLeftTrigger;
                    last_rt = g.bRightTrigger;
                }
                AcquireSRWLockExclusive(&s->lock);
                if (!s->have || memcmp(&g, &s->pad, sizeof g))
                    s->packet++;
                s->pad = g;
                s->have = 1;
                ReleaseSRWLockExclusive(&s->lock);
            }
        } else if (w != WAIT_TIMEOUT) {
            why = GetLastError();
            break;
        }
        if (GetTickCount() - beat >= 5000) {
            fprintf(stderr, "  [sony] id %d: %lu reports in 5 s, rumble %04lX",
                    (int)(s - s_pads), reports, (unsigned long)s->rumble_sent);
            fputc(10, stderr);
            reports = 0;
            beat = GetTickCount();
        }
        want = s->rumble;
        if (want != s->rumble_sent)
            send_rumble(s, want);
    }
    if (pending) {
        CancelIoEx(s->h, &ov);
        GetOverlappedResult(s->h, &ov, &n, TRUE);
    }
out:
    fprintf(stderr, "  [sony] id %d: reader stopped, error %lu", (int)(s - s_pads), why);
    fputc(10, stderr);
    if (ov.hEvent)
        CloseHandle(ov.hEvent);
    free(buf);
    AcquireSRWLockExclusive(&s->lock);
    s->have = 0;
    ReleaseSRWLockExclusive(&s->lock);
    CloseHandle(s->h);
    s->h = NULL;
    s->path[0] = 0;
    InterlockedExchange(&s->active, 0);
    return 0;
}

static int already_open(const wchar_t *path)
{
    int i;
    for (i = 0; i < SONY_MAX_PADS; i++)
        if (s_pads[i].active && !_wcsicmp(s_pads[i].path, path))
            return 1;
    return 0;
}

static void try_open(const wchar_t *path)
{
    HIDD_ATTRIBUTES a;
    PHIDP_PREPARSED_DATA pp;
    HIDP_CAPS caps;
    HANDLE h;
    int kind, i;
    SonyPad *s = NULL;

    if (already_open(path))
        return;
    /* No access rights: enough to read the ids of any HID device, even the
     * keyboard and mouse that Windows keeps to itself. */
    h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    a.Size = sizeof a;
    kind = HidD_GetAttributes(h, &a) && a.VendorID == SONY_VID ? pad_kind(a.ProductID) : -1;
    CloseHandle(h);
    if (kind < 0)
        return;

    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    memset(&caps, 0, sizeof caps);
    if (!HidD_GetPreparsedData(h, &pp)) {
        CloseHandle(h);
        return;
    }
    HidP_GetCaps(pp, &caps);
    HidD_FreePreparsedData(pp);
    if (caps.UsagePage != 0x01 || (caps.Usage != 0x05 && caps.Usage != 0x04) || !caps.InputReportByteLength) {
        CloseHandle(h);
        return;
    }
    for (i = 0; i < SONY_MAX_PADS; i++)
        if (!s_pads[i].active) {
            s = &s_pads[i];
            break;
        }
    if (!s) {
        CloseHandle(h);
        return;
    }
    s->kind = kind;
    s->bt = wcsstr(path, L"00001124-0000-1000-8000-00805f9b34fb") != NULL
         || wcsstr(path, L"00001124-0000-1000-8000-00805F9B34FB") != NULL;
    s->h = h;
    lstrcpynW(s->path, path, 512);
    s->in_len = caps.InputReportByteLength;
    s->out_len = caps.OutputReportByteLength;
    s->feat_len = caps.FeatureReportByteLength;
    s->have = 0;
    s->rumble = 0;
    s->rumble_sent = 0;
    InterlockedExchange(&s->active, 1);
    {
        HANDLE t = CreateThread(NULL, 0, reader, s, 0, NULL);
        if (!t) {
            InterlockedExchange(&s->active, 0);
            CloseHandle(h);
            s->h = NULL;
            return;
        }
        CloseHandle(t);
    }
    fprintf(stderr, "  [input] %s over %s\n", kind == KIND_DS ? "DualSense" : "DualShock 4",
            s->bt ? "Bluetooth" : "USB");
    fflush(stderr);
}

static void scan(void)
{
    GUID guid;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i;

    HidD_GetHidGuid(&guid);
    set = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return;
    ifd.cbSize = sizeof ifd;
    for (i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &guid, i, &ifd); i++) {
        BYTE raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 512 * sizeof(wchar_t)];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)raw;
        d->cbSize = sizeof *d;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, d, sizeof raw, NULL, NULL))
            continue;
        /* Cheap filter before opening anything: the path names the ids. */
        if (!wcsstr(d->DevicePath, L"054c") && !wcsstr(d->DevicePath, L"054C"))
            continue;
        try_open(d->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
}

/* Looks for pads now and every two seconds, so one plugged in or paired
 * while the game runs is picked up. */
static DWORD WINAPI scanner(LPVOID arg)
{
    (void)arg;
    for (;;) {
        scan();
        Sleep(2000);
    }
}

static BOOL CALLBACK start(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    const char *e = getenv("RECOMP_SONY_HID");
    HANDLE t;
    int i;
    (void)once; (void)param; (void)ctx;
    s_off = e && *e == '0';
    for (i = 0; i < SONY_MAX_PADS; i++)
        InitializeSRWLock(&s_pads[i].lock);
    if (s_off)
        return TRUE;
    scan();
    t = CreateThread(NULL, 0, scanner, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    return TRUE;
}

void sony_init(void)
{
    InitOnceExecuteOnce(&s_once, start, NULL, NULL);
}

int sony_get(int n, XINPUT_STATE *st)
{
    int i;
    sony_init();
    for (i = 0; i < SONY_MAX_PADS; i++) {
        SonyPad *s = &s_pads[i];
        int ok = 0;
        if (!s->active)
            continue;
        AcquireSRWLockShared(&s->lock);
        if (s->have && n-- == 0) {
            st->dwPacketNumber = s->packet;
            st->Gamepad = s->pad;
            ok = 1;
        }
        ReleaseSRWLockShared(&s->lock);
        if (ok)
            return i;
    }
    return -1;
}

void sony_rumble(int id, WORD strong, WORD weak)
{
    if (id < 0 || id >= SONY_MAX_PADS || !s_pads[id].active)
        return;
    InterlockedExchange(&s_pads[id].rumble, (LONG)((strong >> 8) << 8 | (weak >> 8)));
}

#endif /* _WIN32 */
