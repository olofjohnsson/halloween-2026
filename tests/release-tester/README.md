# Release tester — nRF52840 DK

Acts as station 5 so the candy dispenser can be tested without the puzzle stations.

- Press **any button** (all three do the same): advertises `MSG_RELEASE_CANDY` (target 0, source 5) with a new random session.
- **LED1** is on while it waits for the dispenser's `MSG_CANDY_DONE` for the same session (30 s timeout).
- Logs over UART (and `Requesting release` / `Dispenser confirmed release`).

Build and flash (NCS 3.4.0 toolchain environment):

```sh
cd tests/release-tester
west build -b nrf52840dk/nrf52840 -p always .
west flash
```

## Pro Micro nRF52840

Three buttons between P0.08 / P0.17 / P0.20 and GND (any of them sends the release), **LED1** is P0.15 (assumed active high). Logs go over RTT.

## FOTA

Builds with MCUboot (sysbuild) like the stations, and stays connectable as `RelTester` for the
nRF Connect Device Manager app. The first flash must be over SWD (replaces the UF2 bootloader):

```sh
west build -b promicro_nrf52840/nrf52840 -p always .
west flash
```

Later builds can be updated over the air with the FOTA package (`build/dfu_application.zip`):

```sh
./build_fota.sh [output dir]   # writes release-tester-dfu.zip
```

Each press uses a new session, so the dispenser releases once per press.
