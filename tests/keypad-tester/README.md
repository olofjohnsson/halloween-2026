# Keypad tester — nRF52840 DK

Logs every key press of the HALJIA 4x3 keypad over UART (`Key '5'`).

Wiring: keypad ribbon pin N to P1.0N (pin 1 to P1.01 ... pin 7 to P1.07). The overlay maps
R1=pin 2, R2=pin 7, R3=pin 6, R4=pin 4, C1=pin 3, C2=pin 1, C3=pin 5.

Build and flash (NCS 3.4.0 toolchain environment):

```sh
cd tests/keypad-tester
west build -b nrf52840dk/nrf52840 -p always .
west flash
```

Open the DK's UART console (115200 baud) and press the keys.
