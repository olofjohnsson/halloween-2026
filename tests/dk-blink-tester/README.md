# DK blink tester — nRF52840 DK

Sanity check: toggles P0.13 (LED1) once per second, so the LED is on for 1 s and off for 1 s.
It also works as a signal source on P0.13 for measuring with a scope or multimeter.

Build and flash (NCS 3.4.0 toolchain environment):

```sh
cd tests/dk-blink-tester
west build -b nrf52840dk/nrf52840 -p always .
west flash
```
