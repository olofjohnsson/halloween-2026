# Candy counter server

Small Flask app that counts candy dispensed by the nRF7002 DK.

- `POST /api/dispense` (header `X-API-Key`, JSON `{"boot":n,"seq":n,"session":n}`) increments the counter; retries are de-duplicated.
- `GET /api/count` returns `{"count":..,"last":..}`.
- `GET /` shows the live count page (`static/index.html`).

## Synology (no Docker needed)
1. In Package Center install **Python 3.x**.
2. Copy this folder to the NAS (e.g. `/volume1/candy-counter`).
3. In Control Panel → Task Scheduler create a **Triggered Task → User-defined script**, event **Boot-up**, user `root`, with the script:
   `API_KEY="your-secret" PORT=8080 /volume1/candy-counter/run.sh`
   (first run creates a virtualenv and installs Flask/Waitress, so the NAS needs internet once).
4. Run the task, then open `http://<nas-ip>:8080/`.
5. If the NAS firewall is on, allow TCP 8080 from the LAN.

The count is stored in `data/counts.json` next to `app.py`.

## Firmware
Copy `candy-dispenser/wifi_secrets.conf.example` to `wifi_secrets.conf`, set the Wi-Fi, `<nas-ip>`, port and the same API key, then rebuild.
Traffic is plain HTTP, so keep it on the LAN.
