/* Sony controllers (DualSense, DualShock 4) read straight from HID on
 * Windows, where XInput can't see them. See hid_sony.c. */
#ifndef HID_SONY_H
#define HID_SONY_H

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>

#define SONY_MAX_PADS 4

/* Starts looking for pads (again every two seconds). Safe to call often. */
void sony_init(void);

/* The n-th connected Sony pad's state as an XInput one. Returns its id
 * (for sony_rumble), or -1 when there are not that many. */
int sony_get(int n, XINPUT_STATE *st);

/* Motor speeds as XInput's: strong = left (low frequency), weak = right. */
void sony_rumble(int id, WORD strong, WORD weak);
#endif

#endif
