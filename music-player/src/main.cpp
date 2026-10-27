// Halloween music player: listens for MSG_PLAY_TRACK beacons from the stations (BLE advertising)
// and plays the requested track on a DFPlayer Mini. Each player has its own PLAYER_ID (1..9).
#include <Arduino.h>
#include <NimBLEDevice.h>

#ifndef PLAYER_ID
#error "Define PLAYER_ID (1..9), e.g. build the player1 environment"
#endif
#ifndef DF_RX_PIN
#define DF_RX_PIN 20 // ESP32-C3 RX <- DFPlayer TX
#endif
#ifndef DF_TX_PIN
#define DF_TX_PIN 21 // ESP32-C3 TX -> 1k -> DFPlayer RX
#endif
#ifndef DF_SELF_TEST_MS
#define DF_SELF_TEST_MS 5000 // delay between track 1 and track 2 in self-test mode
#endif
#ifndef DF_VOLUME
#define DF_VOLUME 25 // 0..30
#endif

// Same protocol as the stations (see stations/README.md).
static const uint16_t SERVICE_UUID = 0xFFF0;
static const uint8_t PROTOCOL_VERSION = 1;
static const uint8_t MSG_PLAY_TRACK = 8;
static const uint8_t MUSIC_PLAYER_TARGET_BASE = 10;
static const uint8_t LAST_STATION_ID = 5;
static const uint8_t MY_TARGET = MUSIC_PLAYER_TARGET_BASE + PLAYER_ID;

// A BLE callback must not block, so it only queues the request for loop().
static volatile uint16_t pending_track = 0;
static uint8_t last_seq[LAST_STATION_ID + 1];

static void df_send(uint8_t cmd, uint16_t param)
{
    uint8_t f[10] = {0x7E, 0xFF, 0x06, cmd, 0x00, (uint8_t)(param >> 8), (uint8_t)param, 0, 0, 0xEF};
    uint16_t sum = 0;

    for (int i = 1; i <= 6; i++) sum += f[i];
    sum = (uint16_t)(0 - sum);
    f[7] = sum >> 8;
    f[8] = sum & 0xFF;
    Serial1.write(f, sizeof(f));
}

static void df_init()
{
    Serial1.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
    delay(500);
    df_send(0x0C, 0); // reset
    delay(1500);
    df_send(0x09, 2); // source: SD card
    delay(300);
    df_send(0x06, DF_VOLUME);
    delay(100);
}

// Parses the service data of one advertisement:
// [0..1] UUID, then 'H' 'W', version, type, target, source, session(2), value(3).
static void handle_payload(const uint8_t *p, size_t len)
{
    size_t i = 0;

    while (i + 1 < len) {
        uint8_t l = p[i];

        if (l == 0 || i + 1 + l > len) return;
        if (p[i + 1] == 0x16 && l >= 14) { // service data, 16-bit UUID
            const uint8_t *d = p + i + 2;

            if ((uint16_t)(d[0] | (d[1] << 8)) != SERVICE_UUID) return;
            d += 2;
            if (d[0] != 'H' || d[1] != 'W' || d[2] != PROTOCOL_VERSION) return;

            uint8_t type = d[3], target = d[4], source = d[5];
            uint16_t track = d[8] | (d[9] << 8);
            uint8_t seq = d[10];

            Serial.printf("RX type=%u target=%u source=%u track=%u seq=%u\n", type, target, source, track, seq);

            // Beacons repeat for a few seconds, so play each sequence number once per station.
            if (type == MSG_PLAY_TRACK && target == MY_TARGET && source >= 1 &&
                source <= LAST_STATION_ID && seq != last_seq[source]) {
                last_seq[source] = seq;
                pending_track = track;
            }
            return;
        }
        i += 1 + l;
    }
}

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override
    {
        const std::vector<uint8_t> &payload = dev->getPayload();

        handle_payload(payload.data(), payload.size());
    }
};

static ScanCallbacks scan_callbacks;

void setup()
{
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH); // the Super Mini LED is active low
    df_init();

#ifdef DF_SELF_TEST
    // Test mode: play track 1, then track 2 a few seconds later, to verify the DFPlayer.
    Serial.println("Self-test: playing track 1");
    df_send(0x12, 1);
    delay(DF_SELF_TEST_MS);
    Serial.println("Self-test: playing track 2");
    df_send(0x12, 2);
#endif

    NimBLEDevice::init("");
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scan_callbacks, true); // keep duplicates: beacons repeat
    scan->setActiveScan(false);
    scan->setMaxResults(0); // do not store devices
    scan->setInterval(100); // ms
    scan->setWindow(99);    // ms
    scan->start(0, false);  // scan forever

    Serial.printf("Music player %d ready (listening for target %d)\n", PLAYER_ID, MY_TARGET);
}

void loop()
{
    uint16_t track = pending_track;

    if (track != 0) {
        pending_track = 0;
        Serial.printf("Play track %u\n", track);
        df_send(0x12, track);
        digitalWrite(LED_BUILTIN, LOW);
        delay(100);
        digitalWrite(LED_BUILTIN, HIGH);
    }
    delay(10);
}
