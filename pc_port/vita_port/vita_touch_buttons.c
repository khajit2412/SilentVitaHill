/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vita_touch_buttons.c - PSX buttons the Vita does not have, on the front
 * touchscreen. Called from PsyX_Pad_InternalPadUpdates for player 1. */
#if defined(__vita__)
#include <psp2/touch.h>

/* Front panel coordinates are 1920x1088 (twice the screen). */
#define PANEL_W 1920
#define PANEL_H 1088

typedef struct
{
    short x0, y0, x1, y1;
    unsigned short padBit; /* PSX pad word bit, active low */
} VitaTouchButton;

/* The two top corners, a quarter of the width by a third of the height each. */
static const VitaTouchButton s_buttons[] = {
    { 0,                 0, PANEL_W / 4, PANEL_H / 3, 0x0100 }, /* L2 */
    { PANEL_W * 3 / 4,   0, PANEL_W,     PANEL_H / 3, 0x0200 }, /* R2 */
};

static int s_started = 0;

/* Returns the pad-word bits to clear (pressed = 0, as libpad reports them). */
unsigned short VitaTouch_PressedBits(void)
{
    SceTouchData   touch;
    unsigned short pressed = 0;
    unsigned       i, j;

    if (!s_started)
    {
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        s_started = 1;
    }

    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 1)
        return 0;

    for (i = 0; i < touch.reportNum && i < SCE_TOUCH_MAX_REPORT; i++)
    {
        for (j = 0; j < sizeof(s_buttons) / sizeof(s_buttons[0]); j++)
        {
            const VitaTouchButton* b = &s_buttons[j];
            if (touch.report[i].x >= b->x0 && touch.report[i].x < b->x1 &&
                touch.report[i].y >= b->y0 && touch.report[i].y < b->y1)
                pressed |= b->padBit;
        }
    }

    return pressed;
}
#endif
