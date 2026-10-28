/* Gate station: a PIR sensor at the entrance starts the game.
 *
 *   IDLE      motion -> a welcome track is played on a music player
 *   WELCOME   after the welcome track, station 1 is activated (MSG_ACTIVATE, repeated until it answers)
 *   RUNNING   motion -> a wait track (rate limited) and the motion is remembered
 *   end of round (MSG_NEW_ROUND from station 5 or 1, or a timeout) -> IDLE; if there was motion
 *   during the round the welcome track starts again, otherwise everything stays idle until new motion.
 */
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

#include "fota.h"

LOG_MODULE_REGISTER(halloween_gate, LOG_LEVEL_INF);

/* Same protocol as the stations (see stations/README.md). */
#define HALLOWEEN_SERVICE_UUID 0xFFF0
#define PROTOCOL_VERSION 1
#define MAGIC_0 0x48
#define MAGIC_1 0x57
#define MSG_ACTIVATE 1
#define MSG_IDLE 3
#define MSG_NEW_ROUND 5
#define MSG_PLAY_TRACK 8
#define IDLE_ACTIVE_FLAG 0x8000
#define MUSIC_PLAYER_TARGET_BASE 10
#define GATE_ID 6
#define FIRST_STATION_ID 1
#define LAST_STATION_ID 5
#define PLAY_BEACON_MS 3000
#define ACTIVATE_GIVE_UP_MS 60000
#define PAYLOAD_LEN 11
#define SERVICE_DATA_LEN (2 + PAYLOAD_LEN)

#define WELCOME_MS (CONFIG_HALLOWEEN_GATE_WELCOME_SECONDS * 1000)
#define WAIT_COOLDOWN_MS (CONFIG_HALLOWEEN_GATE_WAIT_COOLDOWN_SECONDS * 1000)
#define NEXT_WELCOME_MS (CONFIG_HALLOWEEN_GATE_NEXT_WELCOME_SECONDS * 1000)
#define PIR_WARMUP_MS (CONFIG_HALLOWEEN_GATE_PIR_WARMUP_SECONDS * 1000)
#define GAME_TIMEOUT_MS ((int64_t)CONFIG_HALLOWEEN_GATE_GAME_TIMEOUT_MINUTES * 60000)
#define PLAYER_TARGET (MUSIC_PLAYER_TARGET_BASE + CONFIG_HALLOWEEN_GATE_PLAYER)

BUILD_ASSERT(CONFIG_HALLOWEEN_GATE_WELCOME_LAST >= CONFIG_HALLOWEEN_GATE_WELCOME_FIRST,
             "welcome track range is empty");
BUILD_ASSERT(CONFIG_HALLOWEEN_GATE_WAIT_LAST >= CONFIG_HALLOWEEN_GATE_WAIT_FIRST,
             "wait track range is empty");

enum gate_state {
    GATE_IDLE,
    GATE_WELCOME,   /* welcome track playing */
    GATE_ACTIVATING, /* activation beacon for station 1 */
    GATE_RUNNING,   /* a game is in progress */
};

struct beacon_msg {
    uint8_t type;
    uint8_t target;
    uint8_t source;
    uint16_t session;
    uint32_t value;
};

static const struct gpio_dt_spec pir = GPIO_DT_SPEC_GET(DT_NODELABEL(halloween_pir), gpios);

K_MSGQ_DEFINE(beacon_msgq, sizeof(struct beacon_msg), 8, 4);

static const uint8_t adv_flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
static uint8_t service_data[SERVICE_DATA_LEN] = {
    (uint8_t)(HALLOWEEN_SERVICE_UUID & 0xff),
    (uint8_t)(HALLOWEEN_SERVICE_UUID >> 8),
    MAGIC_0, MAGIC_1, PROTOCOL_VERSION, MSG_IDLE,
    0, GATE_ID, 0, 0, 0, 0, 0
};
static struct bt_data ad[] = {
    BT_DATA(BT_DATA_FLAGS, &adv_flags, sizeof(adv_flags)),
    BT_DATA(BT_DATA_SVC_DATA16, service_data, sizeof(service_data)),
};

static enum gate_state state = GATE_IDLE;
static uint16_t session_id;
static uint8_t play_seq;
static int64_t state_since_ms;
static int64_t play_beacon_until_ms;
static int64_t last_wait_ms;
static int64_t next_welcome_ms; /* 0 = none scheduled */
static bool motion_during_round;
static uint16_t last_end_session_5;
static uint16_t last_end_session_1;

static void update_beacon(uint8_t type, uint8_t target, uint32_t value)
{
    if (type != MSG_PLAY_TRACK) {
        play_beacon_until_ms = 0;
    }
    service_data[5] = type;
    service_data[6] = target;
    service_data[8] = (uint8_t)(session_id & 0xff);
    service_data[9] = (uint8_t)(session_id >> 8);
    service_data[10] = (uint8_t)(value & 0xff);
    service_data[11] = (uint8_t)((value >> 8) & 0xff);
    service_data[12] = (uint8_t)((value >> 16) & 0xff);

    int rc = fota_adv_update(ad, ARRAY_SIZE(ad));

    if (rc < 0) {
        LOG_ERR("Updating BLE beacon failed: %d", rc);
    }
}

static void play_on_player(uint16_t track)
{
    play_seq = (uint8_t)(play_seq + 1U);
    if (play_seq == 0) {
        play_seq = 1;
    }
    update_beacon(MSG_PLAY_TRACK, PLAYER_TARGET, (uint32_t)track | ((uint32_t)play_seq << 16));
    play_beacon_until_ms = k_uptime_get() + PLAY_BEACON_MS;
    LOG_INF("Playing track %u on music player %d", track, CONFIG_HALLOWEEN_GATE_PLAYER);
}

static uint16_t random_track(uint16_t first, uint16_t last)
{
    return first + (uint16_t)(sys_rand32_get() % (uint32_t)(last - first + 1));
}

static void set_state(enum gate_state next)
{
    state = next;
    state_since_ms = k_uptime_get();
}

static void start_welcome(void)
{
    motion_during_round = false;
    next_welcome_ms = 0;
    set_state(GATE_WELCOME);
    play_on_player(random_track(CONFIG_HALLOWEEN_GATE_WELCOME_FIRST, CONFIG_HALLOWEEN_GATE_WELCOME_LAST));
    LOG_INF("Welcome; activating station 1 in %d s", CONFIG_HALLOWEEN_GATE_WELCOME_SECONDS);
}

static void start_activation(void)
{
    do {
        session_id = (uint16_t)sys_rand32_get();
    } while (session_id == 0);

    set_state(GATE_ACTIVATING);
    update_beacon(MSG_ACTIVATE, FIRST_STATION_ID, CONFIG_HALLOWEEN_GATE_STATION1_TRACK);
    LOG_INF("Activating station 1");
}

static void game_running(void)
{
    set_state(GATE_RUNNING);
    last_wait_ms = k_uptime_get();
    update_beacon(MSG_IDLE, 0, 0);
}

static void game_over(const char *why)
{
    LOG_INF("Game over (%s); %s", why, motion_during_round ? "motion seen, welcoming again" : "idle");
    set_state(GATE_IDLE);
    update_beacon(MSG_IDLE, 0, 0);
    if (motion_during_round) {
        next_welcome_ms = k_uptime_get() + NEXT_WELCOME_MS;
        if (next_welcome_ms == 0) {
            next_welcome_ms = 1;
        }
    }
}

static void process_beacon(const struct beacon_msg *msg)
{
    if (msg->source < FIRST_STATION_ID || msg->source > LAST_STATION_ID) {
        return;
    }

    /* Station 1 answering (it is active, or plays its start track) proves it got the activation. */
    if (state == GATE_ACTIVATING && msg->source == FIRST_STATION_ID &&
        ((msg->type == MSG_IDLE && (msg->value & IDLE_ACTIVE_FLAG)) || msg->type == MSG_PLAY_TRACK)) {
        LOG_INF("Station 1 is active; game running");
        game_running();
        return;
    }

    /* Station 5 announces a new round after the candy; a restart of station 1 does so too. The
     * announcements repeat for a while, so each session ends the game only once. */
    if (msg->type == MSG_NEW_ROUND && msg->session != 0 && state == GATE_RUNNING) {
        if (msg->source == LAST_STATION_ID && msg->session != last_end_session_5) {
            last_end_session_5 = msg->session;
            game_over("candy released");
        } else if (msg->source == FIRST_STATION_ID && msg->session != last_end_session_1) {
            last_end_session_1 = msg->session;
            game_over("station 1 restarted");
        }
    }
}

static bool parse_ad(struct bt_data *data, void *user_data)
{
    struct beacon_msg *msg = user_data;

    if (data->type != BT_DATA_SVC_DATA16 || data->data_len < SERVICE_DATA_LEN ||
        data->data[0] != (uint8_t)(HALLOWEEN_SERVICE_UUID & 0xff) ||
        data->data[1] != (uint8_t)(HALLOWEEN_SERVICE_UUID >> 8)) {
        return true;
    }

    const uint8_t *p = data->data + 2;

    if (p[0] != MAGIC_0 || p[1] != MAGIC_1 || p[2] != PROTOCOL_VERSION) {
        return true;
    }
    msg->type = p[3];
    msg->target = p[4];
    msg->source = p[5];
    msg->session = (uint16_t)p[6] | ((uint16_t)p[7] << 8);
    msg->value = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16);
    return false;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                         struct net_buf_simple *ad_buf)
{
    ARG_UNUSED(addr);
    ARG_UNUSED(rssi);
    ARG_UNUSED(adv_type);

    struct beacon_msg msg = {0};

    /* parse_ad returns false once it has found a game beacon */
    msg.type = 0xff;
    bt_data_parse(ad_buf, parse_ad, &msg);
    if (msg.type != 0xff) {
        (void)k_msgq_put(&beacon_msgq, &msg, K_NO_WAIT);
    }
}

static void motion_detected(int64_t now)
{
    switch (state) {
    case GATE_IDLE:
        if (next_welcome_ms == 0) {
            LOG_INF("Motion at the gate");
            start_welcome();
        }
        break;
    case GATE_RUNNING:
        motion_during_round = true;
        if (CONFIG_HALLOWEEN_GATE_WAIT_FIRST > 0 && now - last_wait_ms >= WAIT_COOLDOWN_MS) {
            last_wait_ms = now;
            LOG_INF("Motion during the game; asking for patience");
            play_on_player(random_track(CONFIG_HALLOWEEN_GATE_WAIT_FIRST, CONFIG_HALLOWEEN_GATE_WAIT_LAST));
        }
        break;
    default:
        /* The beacon is busy with the welcome or the activation. */
        break;
    }
}

int main(void)
{
    int rc;

    LOG_INF("Halloween gate station v%s", APP_VERSION_STRING);

    if (!gpio_is_ready_dt(&pir) || gpio_pin_configure_dt(&pir, GPIO_INPUT) < 0) {
        LOG_ERR("PIR pin setup failed");
        return 0;
    }

    rc = bt_enable(NULL);
    if (rc < 0) {
        LOG_ERR("Bluetooth init failed: %d", rc);
        return 0;
    }

    /* "GT-v<version>", e.g. GT-v1.0.0; at most 11 characters (see fota.h) */
    static const char fota_name[] = "GT-v" APP_VERSION_STRING;
    BUILD_ASSERT(sizeof(fota_name) - 1 <= 11, "version too long for the advertised name");

    rc = fota_adv_start(ad, ARRAY_SIZE(ad), fota_name);
    if (rc < 0) {
        LOG_ERR("BLE advertising start failed: %d", rc);
        return 0;
    }
    fota_confirm_image();

    rc = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found);
    if (rc < 0) {
        LOG_ERR("BLE scanning start failed: %d", rc);
        return 0;
    }

    LOG_INF("Gate ready; ignoring the PIR for %d s", CONFIG_HALLOWEEN_GATE_PIR_WARMUP_SECONDS);

    while (1) {
        int64_t now = k_uptime_get();
        struct beacon_msg msg;

        while (k_msgq_get(&beacon_msgq, &msg, K_NO_WAIT) == 0) {
            process_beacon(&msg);
        }

        if (play_beacon_until_ms != 0 && now >= play_beacon_until_ms) {
            play_beacon_until_ms = 0;
            /* Keep the activation beacon if it was sent in the meantime (it is not a play beacon). */
            if (state != GATE_ACTIVATING) {
                update_beacon(MSG_IDLE, 0, 0);
            }
        }

        if (state == GATE_WELCOME && now - state_since_ms >= WELCOME_MS) {
            start_activation();
        } else if (state == GATE_ACTIVATING && now - state_since_ms >= ACTIVATE_GIVE_UP_MS) {
            LOG_WRN("Station 1 did not answer; giving up");
            game_over("no answer from station 1");
        } else if (state == GATE_RUNNING && now - state_since_ms >= GAME_TIMEOUT_MS) {
            game_over("timeout");
        }

        if (state == GATE_IDLE && next_welcome_ms != 0 && now >= next_welcome_ms) {
            start_welcome();
        }

        if (now >= PIR_WARMUP_MS && gpio_pin_get_dt(&pir) > 0) {
            motion_detected(now);
        }

        k_msleep(50);
    }
    return 0;
}
