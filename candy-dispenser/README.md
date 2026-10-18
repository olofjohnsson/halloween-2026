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

## FOTA (over-the-air updates)
The dispenser is updated from the **nRF Connect Device Manager** app in the same way as
the stations: copy `build/dfu_application.zip` to the phone, connect to `HW-Candy`, and
upload it on the Image tab. Only the application core image is updated; the BLE
network-core image is not part of the update. The new image confirms itself after boot,
otherwise MCUboot reverts. The game beacon pauses while a phone is connected. See the
stations README for the security notes.

## BLE protocol
Same as the stations (16-bit service data UUID `0xFFF0`, magic `HW`, version 1).
The dispenser accepts `MSG_RELEASE_CANDY` (type 4, target 0, source 5) and advertises
`MSG_CANDY_DONE` (type 7, target 5, source 0) with the same session ID.
Messages are unauthenticated advertisements: fine for a supervised game, not for a
security system.

## Prototype limitations
- Not yet built against the exact board packages or tested with the physical hardware.
- Anyone with a BLE transmitter could send a release request.
