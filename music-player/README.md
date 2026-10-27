# Music player — ESP32-C3 Super Mini + DFPlayer Mini

A pure sound player (music and scary noises). It listens for `MSG_PLAY_TRACK` beacons from the stations
(same BLE advertising protocol as in [`stations/README.md`](../stations/README.md)) and starts the requested
track at once. Each player has its own ID, so station 1 can play a track on player 1 while station 3 plays
one on player 2.

## IDs

A player with `PLAYER_ID = N` (1-9) answers to protocol target `10 + N`. It accepts requests from stations 1-5
and ignores everything else. Beacons repeat for a few seconds; each station's sequence number is played once.

To make a station start a track on a player, set in `stations/station_N.conf`:

```
CONFIG_HALLOWEEN_REMOTE_PLAYER=2     # music player 2
CONFIG_HALLOWEEN_REMOTE_TRACK=7      # file 0007.mp3
```

The track is sent when the station becomes active.

## Wiring (ESP32-C3 Super Mini)

| Super Mini | DFPlayer Mini |
|------------|---------------|
| GPIO21 (TX) | RX, via a 1 kΩ resistor |
| GPIO20 (RX) | TX |
| 5V | VCC |
| GND | GND |

Speaker on the DFPlayer SPK pins (or the DAC outputs to an amplifier). Files on the SD card are `0001.mp3`,
`0002.mp3`, ... in a folder named `MP3` (`/MP3/0001.mp3`); the track number is the file name number, independent of copy order. Pins and volume are build flags in
`platformio.ini` (`DF_RX_PIN`, `DF_TX_PIN`, `DF_VOLUME`).

## DFPlayer self-test

```sh
pio run -e selftest -t upload && pio device monitor
```

On boot this plays `0001.mp3`, then `0002.mp3` five seconds later (`DF_SELF_TEST_MS`), then runs as player 1.
Use it to check wiring, SD card and speaker.

## Build and flash (PlatformIO)

```sh
cd music-player
pio run -e player1 -t upload     # player2, player3 for the others
pio device monitor              # shows "Play track N" on every request
```

To add more players, copy an `[env:playerN]` block in `platformio.ini` with a new `PLAYER_ID` (1-9).
The onboard LED blinks briefly on every request.
