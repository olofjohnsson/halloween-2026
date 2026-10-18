# Halloween BLE puzzle station (NCS 3.4.0)

Prototype firmware for five Pro Micro nRF52840 stations. Each station has:
- three buttons wired between GPIO and GND;
- one GPIO output controlling a MOSFET/transistor for ordinary LED lighting;
- BLE scanning and non-connectable BLE advertising;
- on station 5 only: a 1602 LCD with a PCF8574 I2C backpack and a 4x3 code keypad.

## Important hardware note

`boards/promicro_nrf52840_nrf52840.overlay` uses example GPIO numbers:
- buttons: P0.02, P0.03, P0.04
- LED MOSFET control: P0.05

Verify that these pins are exposed and unused on your exact Pro Micro board. The LED GPIO is a logic output only; do not power an LED strip directly from an nRF52840 GPIO. Use a suitable MOSFET/driver and a separately powered LED supply with common ground as appropriate.

## Build

From the NCS 3.4.0 toolchain environment, with this board target available:

```sh
west build -b promicro_nrf52840/nrf52840 -p always . -- -DEXTRA_CONF_FILE=station_1.conf
west flash
```

The first flash of a board must be done over SWD (`west flash`), because it installs MCUboot. This replaces the Adafruit UF2 bootloader that Pro Micro boards ship with. Build with `promicro_nrf52840/nrf52840` (not the `/uf2` variant). After that, update over the air, see [FOTA](#fota-over-the-air-updates).

For stations 2 through 5, use the corresponding `station_N.conf` file. Build each image separately.

If your board target is named differently in your board package, use the exact target shown by `west boards`.

## FOTA (over-the-air updates)

Every station can be updated from a phone with the **nRF Connect Device Manager** app (MCUboot + MCUmgr SMP over BLE).

1. Build the station as above. The signed update package is `build/dfu_application.zip`. Copy it to the phone.
2. In nRF Connect Device Manager, find `HW-StationN` (N is the station ID), tap it, and open the **Image** tab.
3. Select the zip and tap **Start**. Any upload mode works.
4. The station reboots into the new image and confirms it automatically once Bluetooth is up. If the new image fails to start, MCUboot reverts to the previous one.

Notes:
- The device is always connectable, so no button press is needed. While a phone is connected the station pauses its game beacon, so update stations outside a running game.
- The SMP service is unauthenticated. MCUboot only accepts images signed with the project key, but the default key in the SDK is public. For anything beyond a family game, set your own key with `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` in `sysbuild.conf` and keep it out of git.
- The station firmware is in `src/main.c`; the shared FOTA code is in `../common/`.

## Protocol

The firmware uses legacy BLE advertising with 16-bit Service Data UUID `0xFFF0` (locally assigned for this project). Every station scans continuously.

Message payload:
- magic `HW`, version;
- message type, target and source station IDs (the dispenser is source `0`);
- 16-bit game session ID;
- 24-bit value.

| Type | Name | Sent by | Meaning |
|------|------|---------|---------|
| 1 | `MSG_ACTIVATE` | stations 1-4 | activate the next station |
| 3 | `MSG_IDLE` | all | nothing to say |
| 4 | `MSG_RELEASE_CANDY` | station 5 | correct code entered; dispenser (target 0) should release candy |
| 5 | `MSG_NEW_ROUND` | station 5 | start a new round with a fresh session |
| 7 | `MSG_CANDY_DONE` | dispenser | candy released for this session |

### Game flow
- Station 1 starts active with a random session. After a correct answer, stations 1-4 repeatedly advertise `MSG_ACTIVATE` for the next station.
- A waiting station accepts an activation only if it has no session yet or the session matches its own, so stale activations from an earlier round are ignored.
- When the next station has activated and answers with the same session, the previous station stops advertising the activation (acknowledge), so a rebooted station cannot be re-activated by an old beacon.
- Station 5 (LCD + keypad) generates a random six-digit code and shows it for `CONFIG_HALLOWEEN_CODE_DISPLAY_SECONDS` (default 15 s, 0 = always), then hides it. The kids type it on the keypad: digits are shown on the LCD, `*` clears, `#` submits (`WRONG CODE!` / `NEED 6 DIGITS` on errors). The code never leaves station 5.
- On a correct code station 5 advertises `MSG_RELEASE_CANDY`; the dispenser (balcony, no cables to station 5) releases one candy and answers with `MSG_CANDY_DONE`.
- Station 5 then broadcasts `MSG_NEW_ROUND` with a new session for 60 s. All stations reset (station 1 active, others waiting).

Station 5 pins (example, verify): keypad rows P0.06, P0.07, P0.12, P0.13, columns P0.15, P0.17, P0.20; buttons P0.02-P0.04; LED P0.05; LCD I2C SDA P0.08 / SCL P0.11. Keypad connector order assumed R1-R4, C1-C3.

The simple 16-bit session ID is for avoiding accidental cross-game activation, not security. This protocol is suitable for a supervised Halloween game, not access control.

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
- Use RTT/UART logs to check station state transitions.


## Station 5 LCD

The prototype includes a small PCF8574/HD44780 4-bit LCD driver directly in
`src/main.c` (no external library required). It assumes:
- I2C controller `i2c0`;
- 7-bit address `0x27`;
- common backpack bit mapping: P0=RS, P1=RW, P2=E, P3=backlight, P4..P7=D4..D7;
- example SDA P0.08 and SCL P0.11.

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
