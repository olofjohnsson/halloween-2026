# Release tester — nRF52840 DK

Acts as station 5 so the candy dispenser can be tested without the puzzle stations.

- Press **Button 1**: advertises `MSG_RELEASE_CANDY` (target 0, source 5) with a new random session.
- **LED1** is on while it waits for the dispenser's `MSG_CANDY_DONE` for the same session (30 s timeout).
- Logs over UART (and `Requesting release` / `Dispenser confirmed release`).

Build and flash (NCS 3.4.0 toolchain environment):

```sh
cd tests/release-tester
west build -b nrf52840dk/nrf52840 -p always .
west flash
```

Each press uses a new session, so the dispenser releases once per press.
