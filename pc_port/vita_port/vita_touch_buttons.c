/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vita_touch_buttons.c - PSX buttons the Vita does not have, on the front
 * touchscreen: L1/R1 (sidestep), since the shoulder buttons are L2/R2 (look,
 * aim; see the SH_VITA pad defaults in pc_config.c). Called from
 * PsyX_Pad_InternalPadUpdates for player 1. */
#if defined(__vita__)
#include <psp2/touch.h>
#include "sh_log.h"

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
    { 0,                 0, PANEL_W / 4, PANEL_H / 3, 0x0400 }, /* L1 */
    { PANEL_W * 3 / 4,   0, PANEL_W,     PANEL_H / 3, 0x0800 }, /* R1 */
};

static int s_started = 0;

/* Rear panel tap: step through draw-batching settings so their effect on fps
 * can be compared in place (see the table in the tap handler). */
extern int  g_vitaPalCache;
extern int  g_vitaPalMinArea;
extern int  g_vitaReorder;
static int  s_backDown = 0;
static int  s_palMode = 1;

static void VitaTouch_PollBack(void)
{
    SceTouchData back;
    int          down;

    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &back, 1) < 1)
        return;
    down = back.reportNum > 0;
    if (down && !s_backDown)
    {
        /* mode: palette cache, min area, reorder */
        static const int modes[4][3] = {
            { 0, 0,    1 },
            { 1, 0,    1 },
            { 1, 1024, 1 },
            { 1, 1024, 0 },
        };
        s_palMode = (s_palMode + 1) & 3;
        g_vitaPalCache   = modes[s_palMode][0];
        g_vitaPalMinArea = modes[s_palMode][1];
        g_vitaReorder    = modes[s_palMode][2];
        SH_DBG("[PALCACHE] rear touch: mode %d (palette %s, min area %d, reorder %s)", s_palMode,
               g_vitaPalCache ? "on" : "off", g_vitaPalMinArea, g_vitaReorder ? "on" : "off");
    }
    s_backDown = down;
}

/* Returns the pad-word bits to clear (pressed = 0, as libpad reports them). */
unsigned short VitaTouch_PressedBits(void)
{
    SceTouchData   touch;
    unsigned short pressed = 0;
    unsigned       i, j;

    if (!s_started)
    {
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
        s_started = 1;
    }

    VitaTouch_PollBack();

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
