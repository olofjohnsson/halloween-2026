#ifndef HALLOWEEN_DFPLAYER_H
#define HALLOWEEN_DFPLAYER_H

#include <stdint.h>
#include <zephyr/device.h>

/*
 * DFPlayer Mini over UART (9600 8N1). The module is initialised in the
 * background (reset, select SD card, set volume), so none of these calls block.
 * A track requested before the module is ready is played as soon as it is.
 */
#if defined(CONFIG_HALLOWEEN_DFPLAYER)
int dfplayer_init(const struct device *uart, uint8_t volume);
/* Plays /MP3/0001.mp3 .. by file name for track 1..; 0 is ignored. */
void dfplayer_play(uint16_t track);
#else
static inline int dfplayer_init(const struct device *uart, uint8_t volume)
{
    (void)uart;
    (void)volume;
    return 0;
}
static inline void dfplayer_play(uint16_t track)
{
    (void)track;
}
#endif

#endif
