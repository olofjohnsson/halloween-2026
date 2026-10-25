# Light effect tester — nRF52840 DK

Plays light patterns for the station spot light through a MOSFET, in a loop:

| Group | Patterns |
|-------|----------|
| Scary | heartbeat, lightning, slow breathing, dying bulb (random flicker and blackouts) |
| Celebration | strobe, build-up (faster and faster pulses, bright finish), fireworks (random bursts) |
| Wrong answer | buzzer (three hard flashes, slow fade), fizzle (weakening stutter) |

Button 1 skips to the next pattern; the pattern name is printed on the UART console (115200 baud).

Wiring: P0.13 -> MOSFET gate (via a small resistor and a pull-down to GND), MOSFET switches the spot light's
own supply, common GND with the DK. The output is PWM at 5 kHz with normal polarity (higher duty = brighter);
LED1 on the DK shows the inverse. Do not connect the spot light directly to the pin.

Patterns are tables of steps (ramp to an intensity, then hold) in `src/main.c`, so they can be moved to the
stations later. Intensity uses a squared curve so fades look even to the eye.

```sh
cd tests/light-tester
west build -b nrf52840dk/nrf52840 -p always .
west flash
```
