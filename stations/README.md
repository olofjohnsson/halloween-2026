# Halloween BLE puzzle station (NCS 3.4.0)

Prototype firmware for five Pro Micro nRF52840 stations. Each station has:
- three buttons wired between GPIO and GND;
- one GPIO output controlling a MOSFET/transistor for ordinary LED lighting;
- BLE scanning and non-connectable BLE advertising;
- on station 5 only: a 1602 LCD with a PCF8574 I2C backpack and a 4x3 code keypad.

## Important hardware note

`boards/promicro_nrf52840_nrf52840.overlay` uses these GPIOs (as wired; all stations share the button pins):
- buttons: P0.08 (btn 1), P0.17 (btn 2), P0.20 (btn 3)
- LED MOSFET control: P1.06

Station 5 additionally uses the LCD, keypad and DFPlayer pins listed under "Station 5 wiring (Pro Micro)" below. Verify that all pins are exposed and unused on your exact Pro Micro board. The LED GPIO is a logic output only; do not power an LED strip directly from an nRF52840 GPIO. Use a suitable MOSFET/driver and a separately powered LED supply with common ground as appropriate.

## Build

From the NCS 3.4.0 toolchain environment, with this board target available:

```sh
west build -b promicro_nrf52840/nrf52840 -p always . -- -DEXTRA_CONF_FILE=station_1.conf
west flash
```

The first flash of a board must be done over SWD (`west flash`), because it installs MCUboot. This replaces the Adafruit UF2 bootloader that Pro Micro boards ship with. Build with `promicro_nrf52840/nrf52840` (not the `/uf2` variant). After that, update over the air, see [FOTA](#fota-over-the-air-updates).

For stations 2 through 5, use the corresponding `station_N.conf` file. The gate station uses `station_gate.conf` (`./build_fota.sh gate`). Build each image separately.

If your board target is named differently in your board package, use the exact target shown by `west boards`.

## Initial flash with J-Link (pogo-pin fixture)

Only SWD is needed: **SWDIO, SWCLK, GND** and **VTref** (target voltage sense), plus optionally nRESET.
The J-Link 20-pin to 10-pin adapter is a standard Cortex-M adapter, so the 10-pin connector follows the
ARM Cortex Debug 10-pin (1.27 mm) pinout. Pin 1 is marked on the connector/ribbon (red wire on the cable).

| 10-pin (Cortex) | Signal | J-Link 20-pin | Needed? |
|-----------------|--------|---------------|---------|
| 1 | VTref | 1 | yes (target 3.3 V, so J-Link sees the board) |
| 2 | SWDIO | 7 | yes |
| 3 | GND | 4 (also 6, 8, ...) | yes |
| 4 | SWCLK | 9 | yes |
| 5 | GND | 6 | optional |
| 6 | SWO | 13 | no |
| 7 | KEY (no pin) | none | no |
| 8 | NC/TDI | 5 | no |
| 9 | GND detect | 14 | no |
| 10 | nRESET | 15 | optional |

Without the adapter, connect directly to the J-Link 20-pin connector (2x10, 2.54 mm, pin 1 marked/red wire,
odd pins on one row and even pins on the other). Only these pins are needed:

| J-Link 20-pin | Signal | Connect to |
|---------------|--------|------------|
| 1 | VTref | board 3.3 V |
| 7 | SWDIO (TMS) | Pro Micro SWDIO |
| 9 | SWCLK (TCK) | Pro Micro SWCLK |
| 4 (or any even pin: 6, 8, 10 ...) | GND | board GND |
| 15 | nRESET | board reset (optional) |

Pin 19 is a 5 V supply output from the J-Link: leave it unconnected, it must never go to a 3.3 V pad.

My wire colors:

| Color | Signal | J-Link 20-pin |
|-------|--------|---------------|
| Yellow | VDD (3.3 V, to VTref) | 1 |
| Red | SWDIO | 7 |
| Orange | SWCLK | 9 |
| Brown | GND | 4 |

Fixture mapping (fill in by continuity-testing the fixture from the 10-pin connector to each pogo pin and
to the board pad it touches; then it never has to be rediscovered):

| 10-pin connector pin | Signal | Pogo pin / color | Pro Micro pad |
|----------------------|--------|------------------|---------------|
| 1 | VTref | | |
| 2 | SWDIO | | |
| 3 | GND | | |
| 4 | SWCLK | | |
| 10 | nRESET | | |

Check: with the board powered over USB, J-Link should report the target voltage ~3.3 V and
`west flash` (or `nrfutil device list`) should find an nRF52840. Swapped SWDIO/SWCLK or a missing GND are the usual faults.
Locate the Pro Micro's SWDIO/SWCLK pads from your exact board's schematic; they are not the same on every clone.

## FOTA (over-the-air updates)

Every station can be updated from a phone with the **nRF Connect Device Manager** app (MCUboot + MCUmgr SMP over BLE).

1. Build the station as above. The signed update package is `build/dfu_application.zip`. Copy it to the phone.
2. In nRF Connect Device Manager, find `HWN-vX.Y.Z` (N is the station ID, X.Y.Z the running firmware version, e.g. `HW3-v1.0.0`), tap it, and open the **Image** tab.
   After an update the name shows the new version, which confirms the update took effect.
   To release a new version, edit `stations/VERSION` (`VERSION_MAJOR`, `VERSION_MINOR`, `PATCHLEVEL`) before building;
   it is the single source for the advertised name, the boot log and the MCUboot image version (bump it
   for every release). The name has room for 11 characters, e.g. `HW3-v10.2.3`.
3. Select the zip and tap **Start**. Any upload mode works.
4. The station reboots into the new image and confirms it automatically once Bluetooth is up. If the new image fails to start, MCUboot reverts to the previous one.

Notes:
- The device is always connectable, so no button press is needed. While a phone is connected the station pauses its game beacon, so update stations outside a running game.
- The SMP service is unauthenticated. MCUboot only accepts images signed with the project key, but the default key in the SDK is public. For anything beyond a family game, set your own key with `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` in `sysbuild.conf` and keep it out of git.
- The station firmware is in `src/main.c`; the shared FOTA code is in `../common/`.

## Gate station

The gate station (Pro Micro nRF52840 + PIR sensor, `src/gate.c`, built with `station_gate.conf`, advertised as
`GT-v<version>`) is the first step of the game. It only listens and sends beacons; it has no DFPlayer, the sound comes
from a music player (`CONFIG_HALLOWEEN_GATE_PLAYER`, default 1; the player must be in range of the gate or of a station
that relays).

1. **Idle:** motion starts a random welcome track (`CONFIG_HALLOWEEN_GATE_WELCOME_FIRST..LAST`, default 10-14) on the music player.
2. **Welcome:** `CONFIG_HALLOWEEN_GATE_WELCOME_SECONDS` (default 10, set it to the length of the welcome tracks) later the gate
   sends `MSG_ACTIVATE` (source 6, target 1) until station 1 answers that it is active (60 s at most). Station 1 accepts it
   regardless of session and keeps the session it got from the last `MSG_NEW_ROUND`.
3. **Running:** motion plays a random wait track (`CONFIG_HALLOWEEN_GATE_WAIT_FIRST..LAST`, default 15-18, at most one per
   `CONFIG_HALLOWEEN_GATE_WAIT_COOLDOWN_SECONDS`, default 30) and is remembered.
4. **End of round:** when the gate hears `MSG_NEW_ROUND` from station 5 (candy released, relayed by the stations) or from
   station 1 (restart), or after `CONFIG_HALLOWEEN_GATE_GAME_TIMEOUT_MINUTES` (default 20), all stations are waiting. If motion was
   seen during the round, the welcome starts again after `CONFIG_HALLOWEEN_GATE_NEXT_WELCOME_SECONDS` (default 10); otherwise
   everything stays idle until the next motion.

While a welcome or a game is in progress the gate's MOSFET LED (P1.06, as on the stations) shows the lightning effect
(stuttering intro, then steady with sporadic bursts, see [LED lightning](#led-lightning)); it is off while the gate
is idle. `CONFIG_HALLOWEEN_LIGHTNING=n` makes it steady on instead.

The gate must be within range of station 1 (to hear its answer) and, for the end of round, of a station that relays
station 5's announcement. The PIR is ignored for the first 30 s after power-up. Wiring (Pro Micro): PIR OUT -> P0.22
(example pin, `halloween_pir` in the overlay; verify it is free), VCC 5 V, GND. Motion that continues keeps counting: the PIR
output is level-triggered, so someone standing at the gate during the round gets another welcome afterwards.
Without a gate station, flash station 1 with `CONFIG_HALLOWEEN_GATE=n` and it starts by itself as before.

## Restarting the game

Restart the whole game by power-cycling (or resetting) **station 1**, or by holding its answer buttons 1 and 2
pressed together for 5 s (works in any state; press both within 150 ms so no answer is registered, and release
after the lightning intro starts). Station 1 picks a new session ID and announces a
new round for 15 s; stations 2-5 reset to waiting as soon as they hear it (also via the relay). Without a gate
station, station 1 becomes active again with the lightning effect and its start track. With the gate station
(`CONFIG_HALLOWEEN_GATE=y`, set in `station_1.conf`) every station is waiting after a restart, and the game
starts again when the gate sees movement. The other stations never need a restart. A
station that missed the announcement ignores the old round's activations, so just restart station 1 again.

## Protocol

The firmware uses legacy BLE advertising with 16-bit Service Data UUID `0xFFF0` (locally assigned for this project). Every station scans continuously.

Message payload:
- magic `HW`, version;
- message type, target and source station IDs (the dispenser is source `0`);
- 16-bit game session ID;
- 24-bit value.

| Type | Name | Sent by | Meaning |
|------|------|---------|---------|
| 1 | `MSG_ACTIVATE` | stations 1-4, gate (source 6, target 1) | activate the next station |
| 3 | `MSG_IDLE` | all | nothing to say |
| 4 | `MSG_RELEASE_CANDY` | station 5 | correct code entered; dispenser (target 0) should release candy |
| 5 | `MSG_NEW_ROUND` | station 5, station 1 | start a new round with a fresh session (station 1 sends it for 15 s after it boots) |
| 7 | `MSG_CANDY_DONE` | dispenser | candy released for this session |
| 8 | `MSG_PLAY_TRACK` | stations 1-5, gate (source 6) | start a DFPlayer track on the target station (1-5), the dispenser (0) or music player N (target `10 + N`, see [`music-player/`](../music-player/README.md)) |

The 24-bit value carries the DFPlayer track in bits 0-15 (0 = none). `MSG_PLAY_TRACK` also uses bits 16-23 as a
sequence number so a repeated beacon plays the track only once. `MSG_ACTIVATE` (value = track the next station plays)
and `MSG_RELEASE_CANDY` (value = track the dispenser plays) use the track field too.

### Relay (range)

Stations are spread out in the garden, so not every device hears every other one. Each station re-advertises
`MSG_NEW_ROUND`, `MSG_RELEASE_CANDY`, `MSG_CANDY_DONE` and `MSG_PLAY_TRACK` it hears (unchanged payload) for 3 s,
alternating with its own beacon every 400 ms. A message already relayed in the last 5 s is not relayed again, which
stops loops; senders repeat their beacons, so a lost relay is retried. `MSG_ACTIVATE` and the activation
acknowledge are not relayed (they only travel between neighbouring stations). The candy dispenser does not relay,
so at least one station must be within its range. The protocol version is unchanged, so only the stations need
reflashing.

### Game flow
- With the gate station (default, see [Gate station](#gate-station)) station 1 waits after boot and after every round until the gate activates it. Without it (`CONFIG_HALLOWEEN_GATE=n`) station 1 starts active with a random session. After a correct answer, stations 1-4 repeatedly advertise `MSG_ACTIVATE` for the next station.
- A waiting station accepts an activation only if it has no session yet or the session matches its own, so stale activations from an earlier round are ignored.
- When the next station has activated and answers with the same session, the previous station stops advertising the activation (acknowledge), so a rebooted station cannot be re-activated by an old beacon.
- Station 5 (LCD + keypad) generates a random six-digit code (never containing 4, 5 or 6, since those keypad keys are broken) and shows it for `CONFIG_HALLOWEEN_CODE_DISPLAY_SECONDS` (default 15 s, 0 = always), then hides it. The kids type it on the keypad: digits are shown on the LCD, `*` clears, `#` submits (`WRONG CODE!` / `NEED 6 DIGITS` on errors). The code never leaves station 5.
- On a correct code station 5 advertises `MSG_RELEASE_CANDY`; the dispenser (balcony, no cables to station 5) releases one candy and answers with `MSG_CANDY_DONE`.
- Station 5 then broadcasts `MSG_NEW_ROUND` with a new session for 60 s. All stations reset (station 1 active, others waiting).

### Station 5 wiring (Pro Micro)

| Function | Pins |
|----------|------|
| Buttons 1 / 2 / 3 | P0.08 / P0.17 / P0.20 (to GND) |
| LCD I2C | SDA P0.24, SCL P1.00 |
| DFPlayer Mini | RX <- P1.01 (via 1 kΩ), TX -> P1.02 |
| Keypad | see "Station 5 keypad" below |

Summary (Pro Micro, as wired): keypad ribbon pins 1..7 on P0.31, P0.29, P0.02, P1.15, P1.13, P1.11, P0.06; buttons P0.08, P0.17, P0.20; LED P1.06; LCD I2C SDA P0.24 / SCL P1.00; DFPlayer RX <- P1.01 (via 1 kΩ), TX -> P1.02. Keypad connector order assumed R1-R4, C1-C3.

The simple 16-bit session ID is for avoiding accidental cross-game activation, not security. This protocol is suitable for a supervised Halloween game, not access control.

## Testing stations 1 and 2 (ping-pong)

Build station 1 and 2 with `-DEXTRA_CONF_FILE=station_1_pingpong.conf` / `station_2_pingpong.conf`. Station 1 is
active from boot; its correct button deactivates it and activates station 2, whose correct button activates
station 1 again, and so on. If the keypad is wired to the station, add `CONFIG_HALLOWEEN_TEST_PINGPONG_KEYPAD=y`
(see `station_1_pingpong.conf`): typing the fixed code (`CONFIG_HALLOWEEN_TEST_PINGPONG_CODE`, default 1237) followed by `#`
also passes the activation on (`*` clears). Power both at the same time so they share a session. Test only, not for the real game.

## Sound (DFPlayer Mini)

Every station and the candy dispenser can have a DFPlayer Mini (optional, `CONFIG_HALLOWEEN_DFPLAYER=y` in
`station_N.conf`). Wiring on the Pro Micro: P1.01 (TX) -> 1 kΩ -> DFPlayer RX, P1.02 (RX) <- DFPlayer TX,
5 V and GND from the supply (the module needs 5 V for good sound; 3.3 V logic is fine). The pins are set in
`boards/promicro_nrf52840_nrf52840.overlay` (UART1, 9600 baud). On the nRF52840 DK (station 5) the DFPlayer
uses P1.08 (TX -> 1 kΩ -> DFPlayer RX) and P1.09 (RX <- DFPlayer TX) because P1.01..P1.07 are the keypad;
the DK's own 5 V pin powers the module. Files on the SD card are `0001.mp3`, `0002.mp3`, ...
in a folder named `MP3` (`/MP3/0001.mp3`); the track number is the file name number, independent of copy order.

### Track list (file name -> spoken text)

| File | Text |
|------|------|
| `0001.mp3` | du ska få dansa på en myrstack |
| `0002.mp3` | dumheter och inget bus |
| `0003.mp3` | jag ska ta ditt godis och ge det till din mamma |
| `0004.mp3` | din kudde kommer vara hård som sten inatt |
| `0005.mp3` | myrorna biter dig på tårna |
| `0006.mp3` | du kommer skratta så du kissar på dig |
| `0007.mp3` | jag ska limma fast din telefon under skon |
| `0008.mp3` | en gräshoppa kommer gnaga på din macka |
| `0009.mp3` | allt är så läskigt, så läskigt roligt |
| `0010.mp3` | välkommen1 |
| `0011.mp3` | välkommen2 |
| `0012.mp3` | välkommen3 |
| `0013.mp3` | välkommen4 (bra) |
| `0014.mp3` | välkommen5 (bäst) |
| `0015.mp3` | vänta på er tur1 |
| `0016.mp3` | vänta på er tur2 |
| `0017.mp3` | vänta på er tur3 |
| `0018.mp3` | vänta på er tur4 |

## LED lightning

While a station is active its LED flickers like lightning (on/off only): a stuttering burst when it activates that
settles to steady on, then a short random burst every 3-12 s (sometimes with a brief blackout). A wrong answer cancels
any running burst. Turn it off with
`CONFIG_HALLOWEEN_LIGHTNING=n` in `station_N.conf`. The timing constants (`LIGHTNING_*`) are in `src/main.c`.

Options in `station_N.conf`:

| Option | Meaning |
|--------|---------|
| `CONFIG_HALLOWEEN_DFPLAYER_VOLUME` | volume 0-30 (default 20) |
| `CONFIG_HALLOWEEN_ACTIVE_TRACK` | played on this station when it becomes active |
| `CONFIG_HALLOWEEN_CORRECT_TRACK` | played on this station on a correct answer |
| `CONFIG_HALLOWEEN_NEXT_STATION_TRACK` | sent with the activation: the next station plays it when activated |
| `CONFIG_HALLOWEEN_REMOTE_TRACK` + `CONFIG_HALLOWEEN_REMOTE_TARGET` | when this station becomes active, start that track on another station (1-5) or the dispenser (0) via `MSG_PLAY_TRACK` |
| `CONFIG_HALLOWEEN_REMOTE_PLAYER` | if 1-9, send `CONFIG_HALLOWEEN_REMOTE_TRACK` to music player N instead of `CONFIG_HALLOWEEN_REMOTE_TARGET` |
| `CONFIG_HALLOWEEN_REMOTE_WRONG_TRACK` | sent to the same player/target when a wrong button is pressed |
| `CONFIG_HALLOWEEN_REMOTE_CORRECT_TRACK` | sent to the same player/target when the correct button is pressed |
| `CONFIG_HALLOWEEN_REMOTE_DELAYED_TRACK` + `CONFIG_HALLOWEEN_REMOTE_DELAYED_SECONDS` | sent that many seconds after the correct button (default 10 s); waits until the next station has acknowledged the activation, cancelled on a new round |
| `CONFIG_HALLOWEEN_DISPENSER_TRACK` | station 5: track the dispenser plays on a candy release |

Example: `CONFIG_HALLOWEEN_NEXT_STATION_TRACK=3` in `station_2.conf` makes station 3 play track 3 when station 2
hands over. Stations without a DFPlayer ignore track requests.

## Questions and answers

The correct button is set per station in `station_N.conf`. Fill in the questions
printed on each station:

| Station | Correct button | Question |
|---------|---------------|----------|
| 1 | 2 | |
| 2 | 3 | |
| 3 | 1 | |
| 4 | 2 | |
| 5 | 3 | |

## Limitations / next steps

1. Confirm and adjust GPIO pin assignments for your exact Pro Micro board.
2. If a station reboots mid-game it loses its state (it waits again). If station 1 reboots it picks a new session, so power-cycle all stations to restart cleanly.
3. A station that misses the 60 s `MSG_NEW_ROUND` announcement will not rejoin until the next round or a power cycle.

## Troubleshooting

- Use `west build -p always` after changing the overlay.
- Check the generated `build/zephyr/zephyr.dts` to verify the GPIO mapping.
- Use RTT/UART logs to check station state transitions. Pro Micro builds log over RTT
  (`boards/promicro_nrf52840_nrf52840.conf`); view it with the J-Link attached, e.g. `JLinkRTTViewer`
  or the RTT viewer in nRF Connect for VS Code / `nrfutil rtt`. Use `west build -p always` after adding it.


## Station 5 keypad (HALJIA 4x3, 7 pins)

**Code test with the candy dispenser:** build station 5 on a DK with
`west build -b nrf52840dk/nrf52840 -p always . -- -DEXTRA_CONF_FILE=station_5_code_test.conf`.
It skips the puzzle and mirrors the LCD on the UART (`LCD: [CODE: 123789] [> 12]`), so read the code
from the log and type it on the keypad (`#` submits, `*` clears). A correct code makes station 5 request a
release from the dispenser; once the dispenser confirms, press any answer button to get a new code.

The 12-key membrane keypad is a matrix: 4 row lines + 3 column lines = 7 pins.
Measured pinout of this keypad (ribbon pin numbers, each key shorts two pins):
R1 = pin 2, R2 = pin 7, R3 = pin 6, R4 = pin 4, C1 = pin 3, C2 = pin 1, C3 = pin 5.
Wire as follows (Pro Micro nRF52840, 3.3 V logic, no resistors needed, columns use internal pull-ups):

| Keypad ribbon pin | Line | Keys | Pro Micro GPIO |
|-------------------|------|------|----------------|
| 2 | Row 1 | 1 2 3 | P0.29 |
| 7 | Row 2 | 4 5 6 | P0.06 |
| 6 | Row 3 | 7 8 9 | P1.11 |
| 4 | Row 4 | * 0 # | P1.15 |
| 3 | Column 1 | 1 4 7 * | P0.02 |
| 1 | Column 2 | 2 5 8 0 | P0.31 |
| 5 | Column 3 | 3 6 9 # | P1.13 |

If you prefer other pins, change `krow1..krow4` / `kcol1..kcol3` in
`boards/promicro_nrf52840_nrf52840.overlay` (and the pin list above) and rebuild with `-p always`.
On the nRF52840 DK, connect ribbon pin N to P1.0N (pin 1 to P1.01 ... pin 7 to P1.07) and build with
`-b nrf52840dk/nrf52840 -- -DEXTRA_CONF_FILE=station_5.conf`; the DK overlay already maps them.
To test: build with `station_5.conf`, reach the code entry screen and check that each key shows on the LCD.

## Station 5 I/O test (buttons, keypad, LCD, DFPlayer)

`station_5_io_test.conf` checks the station 5 hardware without the puzzle or the dispenser:
the LCD shows the last button and key pressed (`BTN:2  KEY:7`) and the digits typed so far (`> 12`).
Typing the test code (`CONFIG_HALLOWEEN_TEST_IO_CODE`, default `1237`; no `#` needed, `*` clears) shows
`CORRECT! PLAYING` and starts DFPlayer track `CONFIG_HALLOWEEN_TEST_IO_TRACK` (default 1); a wrong code shows `WRONG CODE`.
The LCD is the only output (logging is off in this build). The BLE name is `iotest5` so this build is easy to tell apart. Build on the Pro Micro with:

```sh
west build -b promicro_nrf52840/nrf52840 -p always . -- -DEXTRA_CONF_FILE=station_5_io_test.conf
```

## Station 5 LCD

The prototype includes a small PCF8574/HD44780 4-bit LCD driver directly in
`src/main.c` (no external library required). It assumes:
- I2C controller `i2c0`;
- 7-bit address `0x27`;
- common backpack bit mapping: P0=RS, P1=RW, P2=E, P3=backlight, P4..P7=D4..D7;
- Pro Micro: SDA P0.24 and SCL P1.00 (DK: SDA P0.26 / SCL P0.27).

These assumptions vary by seller and backpack. If the LCD is not detected,
verify its address (often 0x27 or 0x3f), bit mapping, and pinctrl settings.
The sample overlay pin assignments are not guaranteed for every Pro Micro
variant and must be checked against your exact board schematic.

For a common 5 V LCD backpack, do not connect SDA/SCL directly if the backpack
pulls them up to 5 V. Use a bidirectional I2C level shifter or verify that
the pull-ups are tied to 3.3 V. The nRF52840 GPIOs are not 5 V tolerant.





## Correct-answer winning pattern

When the correct button is pressed, the station immediately stops accepting
answers and runs a non-blocking LED celebration: three quick flashes, a short
pause, then three quick flashes. The LED then turns fully off and the station
activates the next station (or shows the final code on station 5).
The pattern is advanced by the main loop, so BLE scanning continues during the
celebration. Only after the pattern finishes is the next station activated.
