# Halloween candy dispenser — nRF7002 DK / NCS 3.4.0

Firmware for the candy dispenser on the balcony. It has **no keypad and no display**:
station 5 (see `../stations`) has the LCD and the code keypad. When the correct code
is entered there, station 5 sends a BLE message and this dispenser releases one candy.

## How it works
1. Station 5 shows the random code, the kids type it on station 5's keypad, and station 5
   checks it.
2. On a correct code station 5 repeatedly advertises `MSG_RELEASE_CANDY` (target 0,
   source 5, with the round's session ID).
3. The dispenser turns the disc one position: it runs the driver until the disc has left
   the home position and come back to the home switch (timeouts are logged).
4. It then advertises `MSG_CANDY_DONE` (target 5, source 0, same session).
5. Station 5 hears the confirmation and starts a new round (see the stations README).

The dispenser releases only once per session, so repeated requests while it is busy or
before station 5 hears the confirmation do not give extra candy.
If the board has no `disc_switch` node (Pro Micro overlay) the driver is pulsed for 800 ms.

The actuator output is a logic signal only; use a suitable transistor/MOSFET or
motor/solenoid driver and a separate supply. Never connect a solenoid or motor
directly to an nRF GPIO.

## nRF7002 DK
Build for the app core (sysbuild automatically adds the BLE `hci_ipc` net-core image):

```sh
west build -b nrf7002dk/nrf5340/cpuapp -p always .
west flash
```
Pins (see `boards/nrf7002dk_nrf5340_cpuapp.overlay`):

| Signal | Header pin | nRF5340 pin |
|--------|-----------|-------------|
| Candy driver (active-low) | D4 (LED1) | P1.06 |
| Disc position switch (to GND, active-low) | - | P1.03 |

### 1.8 V to 5 V level-shifter board
A small circuit board converts the DK's 1.8 V control signal to 5 V for the candy
driver. Its 5-pin header, counting from pin 1:

| Header pin | Signal |
|-----------|--------|
| 1 | 5 V in |
| 2 | 5 V out |
| 3 | 1.8 V control in (from the DK GPIO) |
| 4 | GND |
| 5 | GND |

Connect the 1.8 V control input to the candy driver GPIO (P1.06 / D4) and share GND
with the DK. GPIO levels stay at the DK's 1.8 V. The candy driver pin goes through an
inverting transistor stage, so it is configured active-low (pin low = driver on,
idle high).

### Disc test
Set `DISC_TEST` to 1 in `src/main.c` to run only the disc test: the driver runs until
the disc has left home and returned to the switch, waits 2 s, and repeats. Keep it 0
for normal operation.

## Pro Micro nRF52840 (alternative)
```sh
west build -b promicro_nrf52840/nrf52840 -p always .
```
Example pin (verify against your board): candy driver P0.10. The first flash goes over
SWD and replaces the UF2 bootloader with MCUboot (see the stations README).

## No FOTA on the dispenser
The Wi-Fi firmware is too large for an MCUboot slot on the nRF5340, so the dispenser is
flashed over USB/J-Link (`west flash`) and has no BLE FOTA. The stations still use FOTA.

## Sound (DFPlayer Mini)

Set `CONFIG_HALLOWEEN_DFPLAYER=y` (e.g. in `wifi_secrets.conf` or `-DEXTRA_CONF_FILE`) to play tracks requested by
the stations: the track in the candy release request from station 5 (`CONFIG_HALLOWEEN_DISPENSER_TRACK` there)
and `MSG_PLAY_TRACK` from any station. It uses the Arduino serial port UART1 at 9600 baud: P1.01 (D1, TX) -> 1 kΩ
-> DFPlayer RX, P1.00 (D0, RX) <- DFPlayer TX. The Arduino header is 1.8 V logic, so use a level shifter for the
DFPlayer's 3.3 V TX and RX. Volume: `CONFIG_HALLOWEEN_DFPLAYER_VOLUME`. Not available on the Pro Micro variant yet.

## BLE protocol
Same as the stations (16-bit service data UUID `0xFFF0`, magic `HW`, version 1).
The dispenser accepts `MSG_RELEASE_CANDY` (type 4, target 0, source 5) and advertises
`MSG_CANDY_DONE` (type 7, target 5, source 0) with the same session ID.
Messages are unauthenticated advertisements: fine for a supervised game, not for a
security system.

## Prototype limitations
- Not yet built against the exact board packages or tested with the physical hardware.
- Anyone with a BLE transmitter could send a release request.

## Reporting dispense counts

The nRF7002 DK posts every released candy over Wi-Fi to the web server in `../server`.
Copy `wifi_secrets.conf.example` to `wifi_secrets.conf` (git-ignored) and fill in Wi-Fi and server settings; without it the build works but does not report.

### Report test mode
Add `CONFIG_HALLOWEEN_REPORT_TEST=y` to `wifi_secrets.conf` and rebuild: the dispenser then posts a fake dispense to the server every second (session 0), without releasing candy. Remove it for normal use.
