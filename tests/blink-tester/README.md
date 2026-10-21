# Blink tester — Pro Micro nRF52840

Blinks P0.15 (500 ms on / 500 ms off) to confirm that SWD flashing works. P0.15 is assumed to be the
board's LED and active high; edit `boards/promicro_nrf52840_nrf52840.overlay` if your board differs.

```sh
cd tests/blink-tester
west build -b promicro_nrf52840/nrf52840 -p always .
west flash
```

This is a plain app without MCUboot, linked at flash address 0 (`CONFIG_FLASH_LOAD_OFFSET=0x0`), so flashing it
over SWD replaces the Adafruit UF2 bootloader/MBR. Without that override Zephyr links the app at 0x1000 and nothing
starts it after an SWD flash. The real station firmware is flashed afterwards with `west flash` (see `stations/README.md`).
