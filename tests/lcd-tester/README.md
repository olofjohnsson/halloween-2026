# LCD tester — nRF52840 DK

Tests the 1602 LCD with PCF8574 I2C backpack used on station 5. It scans the I2C bus (logged over UART),
finds the backpack on 0x27 or 0x3f, shows text, a full-character test pattern, then an uptime counter
while the backlight blinks. It uses the same driver code and bit mapping as `stations/src/main.c`.

Wiring (the DK's `i2c0`, Arduino header): SDA P0.26, SCL P0.27, plus 5 V and GND for the backpack.
If the backpack pulls SDA/SCL up to 5 V, use a level shifter or move the pull-ups to 3.3 V
(the nRF52840 GPIOs are not 5 V tolerant).

Build and flash (NCS 3.4.0 toolchain environment):

```sh
cd tests/lcd-tester
west build -b nrf52840dk/nrf52840 -p always .
west flash
```

Open the DK's UART console (115200 baud). If nothing shows on the LCD but the log lists the device,
adjust the contrast potentiometer on the backpack. If the log lists no device, check wiring and address.
