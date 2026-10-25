# Player tester — nRF52840 DK

Acts as station 5 and sends `MSG_PLAY_TRACK` to music player 1 (target 11), so the player's BLE path and
DFPlayer can be tested without the puzzle stations.

- **Button 1**: play next track (1 → 2 → 3 → 1, `LAST_TRACK` in `src/main.c`).
- **Button 2**: play previous track.
- **LED1** is on while the request is advertised (3 s). Each press uses a new sequence number.
- Logs over UART: `Sent play track N to player 1`.

Build and flash (NCS 3.4.0 toolchain environment):

```sh
cd tests/player-tester
west build -b nrf52840dk/nrf52840 -p always .
west flash
```

Flash a music player first (`pio run -e player1 -t upload` in `music-player/`).
