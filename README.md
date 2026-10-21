# halloween-2026

A Halloween game for kids: solve a chain of puzzles to earn candy. Five puzzle stations
talk to each other over Bluetooth LE. When the final code is entered, a candy dispenser
on the balcony releases a handful of candy for the kids to share.

## How it works

1. **Stations 1-4** each ask a question with three answer buttons. A correct answer
   makes the station celebrate with its LED and activates the next station.
2. **Station 5** shows a random six-digit code on an LCD. The kids type it on a keypad.
3. On a correct code, station 5 asks the **candy dispenser** to release a handful of candy.
4. The dispenser confirms, station 5 starts a new round, and all stations reset.

All communication is BLE advertising (no pairing, no wiring between stations).
It is meant for a supervised game, not for security.

## Repository layout

| Folder | Description |
|--------|-------------|
| [`stations/`](stations/README.md) | Firmware for the five puzzle stations (Pro Micro nRF52840), including the BLE protocol and per-station config (`station_N.conf`). |
| [`candy-dispenser/`](candy-dispenser/README.md) | Firmware for the candy dispenser (nRF7002 DK, with a Pro Micro alternative). |
| [`tests/release-tester/`](tests/release-tester/README.md) | nRF52840 DK app that fakes station 5 (Button 1 requests a candy release) for testing the dispenser. |

## Getting started

Both projects are Zephyr applications built with the nRF Connect SDK (NCS) 3.4.0 and `west`.
From the NCS toolchain environment:

```sh
# A station (repeat with station_1.conf ... station_5.conf)
cd stations
west build -b promicro_nrf52840/nrf52840 -p always . -- -DEXTRA_CONF_FILE=station_1.conf
west flash

# The candy dispenser
cd candy-dispenser
west build -b nrf7002dk/nrf5340/cpuapp -p always .
west flash
```

See the folder READMEs for pin assignments, wiring notes, and troubleshooting.

## Firmware updates (FOTA)

All boards run MCUboot and can be updated over BLE from a phone with the nRF Connect Device
Manager app. The first flash of each board is over SWD; after that, upload the
`build/dfu_application.zip` from the build. Details are in the folder READMEs.

## Status

Prototype. GPIO pins are examples and must be verified against your exact boards, and the
firmware has not yet been fully tested on the physical hardware.
