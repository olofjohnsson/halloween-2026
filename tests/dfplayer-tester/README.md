# DFPlayer Mini tester — nRF52840 DK

Plays the tracks on the SD card already in the DFPlayer Mini board.

- Selects the SD source, sets volume 15/30, then plays track 1..3 for 5 s each, looping.
- **Button 1**: skip to next track. **Button 2**: pause (the next track change resumes playback).
- Logs sent/received frames over UART0 (the DK's USB serial port).

## Wiring (3.3 V logic)

| DFPlayer | nRF52840 DK |
|----------|-------------|
| VCC | 5 V (VDD pin; module needs 3.3-5 V, 5 V is better for sound) |
| GND | GND |
| RX | P1.01 via 1 kΩ resistor |
| TX | P1.02 |
| SPK_1 / SPK_2 | small speaker (<3 W), or DAC_R/DAC_L + GND to an amplifier |

## Build and flash (NCS 3.4.0 toolchain environment)

```sh
cd tests/dfplayer-tester
west build -b nrf52840dk/nrf52840 -p always .
west flash
```

Adjust `TRACK_COUNT` in `src/main.c` to the number of files on the card
(`0001.mp3`, `0002.mp3`, ... in the `/MP3` folder). If you see no `RX` logs
(init reply `0x3F`, finished `0x3D`) check the TX/RX wiring.
