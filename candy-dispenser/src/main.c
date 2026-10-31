#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>

#include "fota.h"
#include "dfplayer.h"
#include "report.h"

LOG_MODULE_REGISTER(candy, LOG_LEVEL_INF);

/* Protocol shared with the stations (see ../stations/src/main.c). */
#define HALLOWEEN_SERVICE_UUID 0xFFF0
#define PROTOCOL_VERSION 1
#define MAGIC_0 0x48
#define MAGIC_1 0x57
#define MSG_IDLE 3
#define MSG_RELEASE_CANDY 4 /* station 5 -> dispenser */
#define MSG_CANDY_DONE 7    /* dispenser -> station 5 */
#define MSG_PLAY_TRACK 8    /* any station -> dispenser: play a track */
#define MSG_RUN_DISC 9      /* remote control: turn the disc while this keeps arriving */
#define RUN_DISC_TIMEOUT_MS 500
#define LAST_STATION_ID 5
#define DISPENSER_ID 0

#define PAYLOAD_LEN 11
#define SERVICE_DATA_LEN (2 + PAYLOAD_LEN)

#define ACTUATOR_PULSE_MS 800 /* fallback when there is no disc switch */
#define DISC_LEAVE_TIMEOUT_MS 5000
#define DISC_HOME_TIMEOUT_MS 10000

/* Set to 1 to run only the disc test (driver runs until the switch, repeat). */
#define DISC_TEST 0

static const struct gpio_dt_spec actuator =
    GPIO_DT_SPEC_GET(DT_NODELABEL(candy_actuator), gpios);

#if DT_NODE_EXISTS(DT_NODELABEL(disc_switch))
#define HAS_DISC_SWITCH 1
static const struct gpio_dt_spec disc_switch =
    GPIO_DT_SPEC_GET(DT_NODELABEL(disc_switch), gpios);
#else
#define HAS_DISC_SWITCH 0
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(flasher))
#define HAS_FLASHER 1
static const struct gpio_dt_spec flasher = GPIO_DT_SPEC_GET(DT_NODELABEL(flasher), gpios);
#else
#define HAS_FLASHER 0
#endif

/* Value field: bits 0..15 = DFPlayer track (0 = none), bits 16..23 = sequence (MSG_PLAY_TRACK). */
struct release_request {
    uint8_t type;
    uint8_t source;
    uint8_t seq;
    uint16_t session;
    uint16_t track;
};

#if CONFIG_HALLOWEEN_DFPLAYER
#if !DT_NODE_EXISTS(DT_NODELABEL(dfplayer_uart))
#error "CONFIG_HALLOWEEN_DFPLAYER needs a dfplayer_uart node in the board overlay"
#endif
static const struct device *const dfplayer_dev = DEVICE_DT_GET(DT_NODELABEL(dfplayer_uart));
#endif

static uint8_t last_play_seq[LAST_STATION_ID + 1];

/* BLE scan callback runs outside the main thread; queue requests for it. */
K_MSGQ_DEFINE(release_msgq, sizeof(struct release_request), 4, 4);

#define SESSION_HISTORY 8
/* Recent sessions: delayed copies of an old request (e.g. relayed by a station) must not release again. */
static uint16_t released_sessions[SESSION_HISTORY];
static size_t released_next;

static bool session_released(uint16_t session)
{
    for (size_t i = 0; i < SESSION_HISTORY; i++) {
        if (released_sessions[i] == session) {
            return true;
        }
    }
    return false;
}

static uint8_t service_data[SERVICE_DATA_LEN];
static const uint8_t adv_flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
static struct bt_data ad[] = {
    BT_DATA(BT_DATA_FLAGS, &adv_flags, sizeof(adv_flags)),
    BT_DATA(BT_DATA_SVC_DATA16, service_data, sizeof(service_data)),
};

static void fill_beacon(uint8_t type, uint8_t target, uint16_t session)
{
    service_data[0] = (uint8_t)(HALLOWEEN_SERVICE_UUID & 0xff);
    service_data[1] = (uint8_t)(HALLOWEEN_SERVICE_UUID >> 8);
    service_data[2] = MAGIC_0;
    service_data[3] = MAGIC_1;
    service_data[4] = PROTOCOL_VERSION;
    service_data[5] = type;
    service_data[6] = target;
    service_data[7] = DISPENSER_ID;
    service_data[8] = (uint8_t)(session & 0xff);
    service_data[9] = (uint8_t)(session >> 8);
    service_data[10] = 0;
    service_data[11] = 0;
    service_data[12] = 0;
}

static void set_beacon(uint8_t type, uint8_t target, uint16_t session)
{
    fill_beacon(type, target, session);

    int rc = fota_adv_update(ad, ARRAY_SIZE(ad));
    if (rc < 0) {
        LOG_ERR("Updating BLE beacon failed: %d", rc);
    }
}

static bool parse_service_data(struct bt_data *data, void *user_data)
{
    struct release_request *out = user_data;

    if (data->type != BT_DATA_SVC_DATA16 || data->data_len < SERVICE_DATA_LEN) {
        return true;
    }

    if (data->data[0] != (uint8_t)(HALLOWEEN_SERVICE_UUID & 0xff) ||
        data->data[1] != (uint8_t)(HALLOWEEN_SERVICE_UUID >> 8)) {
        return true;
    }

    const uint8_t *p = data->data + 2;
    if (p[0] != MAGIC_0 || p[1] != MAGIC_1 || p[2] != PROTOCOL_VERSION ||
        p[4] != DISPENSER_ID) {
        return true;
    }
    if (!(p[3] == MSG_RELEASE_CANDY && p[5] == LAST_STATION_ID) &&
        !(p[3] == MSG_PLAY_TRACK && p[5] >= 1 && p[5] <= LAST_STATION_ID) &&
        !(p[3] == MSG_RUN_DISC && p[5] == LAST_STATION_ID)) {
        return true;
    }

    out->type = p[3];
    out->source = p[5];
    out->session = (uint16_t)p[6] | ((uint16_t)p[7] << 8);
    out->track = (uint16_t)p[8] | ((uint16_t)p[9] << 8);
    out->seq = p[10];
    return false;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi,
                         uint8_t adv_type, struct net_buf_simple *ad_buf)
{
    ARG_UNUSED(addr);
    ARG_UNUSED(rssi);
    ARG_UNUSED(adv_type);

    struct release_request req = {0};
    bt_data_parse(ad_buf, parse_service_data, &req);
    if (req.type != 0) {
        (void)k_msgq_put(&release_msgq, &req, K_NO_WAIT);
    }
}

#if HAS_DISC_SWITCH
static bool wait_disc_switch(bool active, int64_t timeout_ms)
{
    int64_t end = k_uptime_get() + timeout_ms;

    while (k_uptime_get() < end) {
        if (gpio_pin_get_dt(&disc_switch) == (active ? 1 : 0)) {
            return true;
        }
        k_msleep(2);
    }
    return false;
}
#endif

/* Turn the disc one position: run the driver until the disc has left home
 * and come back to the home switch. */
static bool release_candy(void)
{
    bool ok = true;

    (void)gpio_pin_set_dt(&actuator, 1);

#if HAS_DISC_SWITCH
    if (gpio_pin_get_dt(&disc_switch) == 1 &&
        !wait_disc_switch(false, DISC_LEAVE_TIMEOUT_MS)) {
        LOG_ERR("Disc did not leave home position");
        ok = false;
    } else if (!wait_disc_switch(true, DISC_HOME_TIMEOUT_MS)) {
        LOG_ERR("Timeout waiting for disc home switch");
        ok = false;
    }
#else
    k_msleep(ACTUATOR_PULSE_MS);
#endif

    (void)gpio_pin_set_dt(&actuator, 0);
    return ok;
}

#if HAS_FLASHER
#define FLASH_HALF_PERIOD_MS (CONFIG_HALLOWEEN_FLASH_PERIOD_MS / 2)

static struct k_work_delayable flash_work;
static int64_t flash_end_ms;
static bool flash_on;

static void flash_handler(struct k_work *work)
{
    if (k_uptime_get() >= flash_end_ms) {
        flash_on = false;
        (void)gpio_pin_set_dt(&flasher, 0);
        return;
    }
    flash_on = !flash_on;
    (void)gpio_pin_set_dt(&flasher, flash_on);
    k_work_reschedule(&flash_work, K_MSEC(FLASH_HALF_PERIOD_MS));
}

/* Flash for the length of the final celebration song, starting now. */
static void flash_start(void)
{
    flash_end_ms = k_uptime_get() + CONFIG_HALLOWEEN_FLASH_SECONDS * 1000;
    flash_on = false;
    k_work_reschedule(&flash_work, K_NO_WAIT);
}
#else
static inline void flash_start(void)
{
}
#endif

#if DISC_TEST && HAS_DISC_SWITCH
static void disc_test_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while (1) {
        release_candy();
        k_sleep(K_SECONDS(2));
    }
}

K_THREAD_DEFINE(disc_test, 1024, disc_test_thread, NULL, NULL, NULL, 7, 0, 1000);
#endif

int main(void)
{
    int rc;

    rc = gpio_pin_configure_dt(&actuator, GPIO_OUTPUT_INACTIVE);
    if (rc < 0) {
        LOG_ERR("Actuator GPIO setup failed: %d", rc);
        return 0;
    }

#if HAS_FLASHER
    rc = gpio_pin_configure_dt(&flasher, GPIO_OUTPUT_INACTIVE);
    if (rc < 0) {
        LOG_ERR("Flasher GPIO setup failed: %d", rc);
        return 0;
    }
    k_work_init_delayable(&flash_work, flash_handler);
#endif

#if HAS_DISC_SWITCH
    rc = gpio_pin_configure_dt(&disc_switch, GPIO_INPUT);
    if (rc < 0) {
        LOG_ERR("Disc switch setup failed: %d", rc);
        return 0;
    }
#endif

    rc = bt_enable(NULL);
    if (rc < 0) {
        LOG_ERR("Bluetooth init failed: %d", rc);
        return 0;
    }

    /* Advertising is not running yet, so only fill the payload (updating would fail with -11) */
    fill_beacon(MSG_IDLE, 0, 0);
    rc = fota_adv_start(ad, ARRAY_SIZE(ad), "HW-Candy");
    if (rc < 0) {
        LOG_ERR("BLE advertising start failed: %d", rc);
        return 0;
    }
    fota_confirm_image();

    rc = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found);
    if (rc < 0) {
        LOG_ERR("BLE scan start failed: %d", rc);
        return 0;
    }

#if CONFIG_HALLOWEEN_DFPLAYER
    (void)dfplayer_init(dfplayer_dev, CONFIG_HALLOWEEN_DFPLAYER_VOLUME);
#endif

    report_init();
    LOG_INF("Candy dispenser ready");

    while (1) {
        struct release_request req;

        if (k_msgq_get(&release_msgq, &req, K_FOREVER) != 0) {
            continue;
        }

        /* Manual mode: the disc turns as long as the remote's beacons keep coming. */
        if (req.type == MSG_RUN_DISC) {
            LOG_INF("Remote: turning the disc");
            (void)gpio_pin_set_dt(&actuator, 1);
            while (k_msgq_get(&release_msgq, &req, K_MSEC(RUN_DISC_TIMEOUT_MS)) == 0) {
                /* Each message restarts the timeout; other messages are dropped meanwhile. */
            }
            (void)gpio_pin_set_dt(&actuator, 0);
            LOG_INF("Remote: disc stopped");
            continue;
        }

        /* Beacons repeat, so play each requested sequence once. */
        if (req.type == MSG_PLAY_TRACK) {
            if (req.seq != last_play_seq[req.source]) {
                last_play_seq[req.source] = req.seq;
                LOG_INF("Station %u asks for track %u", req.source, req.track);
                dfplayer_play(req.track);
            }
            continue;
        }

        /* Station 5 repeats its request until it hears the confirmation, so
         * release only once per session and keep confirming it. */
        if (!session_released(req.session)) {
            LOG_INF("Release requested (session=%u)", req.session);
            /* Remember the session first: requests queued during the release must not repeat it. */
            released_sessions[released_next] = req.session;
            released_next = (released_next + 1) % SESSION_HISTORY;
            dfplayer_play(req.track);
            flash_start();
            bool ok = release_candy();

            if (ok) {
                report_dispense(req.session);
            } else {
                LOG_WRN("Release not confirmed by disc switch; not counted");
            }
            set_beacon(MSG_CANDY_DONE, LAST_STATION_ID, req.session);
            LOG_INF("Candy released; confirmation advertised");
        }
    }
    return 0;
}
