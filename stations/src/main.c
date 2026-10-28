#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/devicetree.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

#include "fota.h"
#include "dfplayer.h"

LOG_MODULE_REGISTER(halloween, LOG_LEVEL_INF);

#if !defined(CONFIG_HALLOWEEN_STATION_ID)
#error "Set CONFIG_HALLOWEEN_STATION_ID in a station_N.conf file"
#endif

#if !defined(CONFIG_HALLOWEEN_CORRECT_BUTTON)
#error "Set CONFIG_HALLOWEEN_CORRECT_BUTTON in a station_N.conf file"
#endif

#define STATION_ID CONFIG_HALLOWEEN_STATION_ID
#define CORRECT_BUTTON CONFIG_HALLOWEEN_CORRECT_BUTTON
#define LAST_STATION_ID 5

#if CONFIG_HALLOWEEN_TEST_PINGPONG_KEYPAD && !CONFIG_HALLOWEEN_TEST_PINGPONG
#error "CONFIG_HALLOWEEN_TEST_PINGPONG_KEYPAD needs CONFIG_HALLOWEEN_TEST_PINGPONG"
#endif

#if STATION_ID == LAST_STATION_ID || CONFIG_HALLOWEEN_TEST_PINGPONG_KEYPAD
#define HAS_KEYPAD 1
#endif

#if CONFIG_HALLOWEEN_TEST_PINGPONG
#if STATION_ID > 2
#error "CONFIG_HALLOWEEN_TEST_PINGPONG only works for stations 1 and 2"
#endif
/* Test: stations 1 and 2 hand over to each other forever. */
#define NEXT_STATION_ID (STATION_ID == 1 ? 2 : 1)
#else
#define NEXT_STATION_ID (STATION_ID + 1)
#endif

/* Common PCF8574 I2C backpack defaults. Confirm address and bit mapping
 * against the exact LCD module; 0x27 is common, 0x3f is also used. */
#define LCD_I2C_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

/* Typical PCF8574 backpack mapping:
 * P0=RS, P1=RW, P2=EN, P3=backlight, P4..P7=D4..D7.
 * Some backpacks use a different mapping.
 */
#define LCD_RS 0x01
#define LCD_RW 0x02
#define LCD_EN 0x04
#define LCD_BACKLIGHT 0x08

/* Locally administered 16-bit service-data UUID for this project. */
#define HALLOWEEN_SERVICE_UUID 0xFFF0
#define PROTOCOL_VERSION 1
#define MAGIC_0 0x48 /* H */
#define MAGIC_1 0x57 /* W */

#define MSG_ACTIVATE 1
#define MSG_IDLE 3
/* Station 5 -> dispenser (target 0): the correct code was entered. */
#define MSG_RELEASE_CANDY 4
/* Station 5 -> all stations (target 0): start a new round with a new session. */
#define MSG_NEW_ROUND 5
/* Dispenser -> station 5 (source 0): candy was released for this session. */
#define MSG_CANDY_DONE 7
/* Any -> station/dispenser: start a track there. */
#define MSG_PLAY_TRACK 8
#define DISPENSER_ID 0
/* Music player N (ESP32-C3) is addressed as target MUSIC_PLAYER_TARGET_BASE + N. */
#define MUSIC_PLAYER_TARGET_BASE 10
#define PLAY_BEACON_MS 3000
/* Set in the track field of MSG_IDLE by a station that was just activated: "I got your activation". */
#define IDLE_ACTIVE_FLAG 0x8000
#define NEW_ROUND_BEACON_MS 60000
/* Station 1 announces a new round for this long after it (re)boots, then plays its start track. */
#define RESTART_BEACON_MS 15000
/* Station 1: holding answer buttons 1 and 2 together this long restarts the game. */
#define RESTART_HOLD_MS 5000
#define RESTART_GRACE_MS 150 /* wait for the second button before treating a press as an answer */
#define STATUS_SHOW_MS 2000
#define CODE_LEN 6

/* Payload following the 2-byte service UUID:
 * [0] magic H, [1] magic W, [2] version, [3] type,
 * [4] target station, [5] source station,
 * [6..7] session ID (little endian),
 * [8..10] value (24-bit little endian): bits 0..15 = DFPlayer track (0 = none),
 *         bits 16..23 = sequence number (MSG_PLAY_TRACK only, so a repeated
 *         beacon plays the track once).
 */
/* 0 = keep showing the code until the next round starts. */
#define CODE_DISPLAY_MS (CONFIG_HALLOWEEN_CODE_DISPLAY_SECONDS * 1000)
#define PAYLOAD_LEN 11
#define SERVICE_DATA_LEN (2 + PAYLOAD_LEN)

enum station_state {
    ST_WAITING = 0,
    ST_ACTIVE,
    ST_CELEBRATING,
    ST_COMPLETED,
    ST_ENTER_CODE, /* station 5: code shown, waiting for keypad entry */
    ST_RELEASING,  /* station 5: waiting for the dispenser to release candy */
};

struct button_msg {
    uint8_t type;
    uint8_t target;
    uint8_t source;
    uint16_t session;
    uint32_t value;
};

static const struct gpio_dt_spec buttons[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(button1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button3), gpios),
};

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(DT_NODELABEL(halloween_led), gpios);

#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(led0))
/* Board status LED (Pro Micro red LED on P0.15), used only for the boot indication. */
static const struct gpio_dt_spec boot_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Blinks 5 times at start-up so a fresh flash visibly shows the application runs. */
static void boot_blink(void)
{
    if (gpio_pin_configure_dt(&boot_led, GPIO_OUTPUT_INACTIVE) < 0) {
        return;
    }
    for (int i = 0; i < 5; i++) {
        (void)gpio_pin_set_dt(&boot_led, 1);
        k_msleep(100);
        (void)gpio_pin_set_dt(&boot_led, 0);
        k_msleep(100);
    }
}
#else
static void boot_blink(void) {}
#endif

#ifdef HAS_KEYPAD
/* Station 5 also has the 4x3 code keypad. Rows are driven low one at a time,
 * columns have pull-ups. Connector order assumed: R1..R4, C1..C3. */
static const struct gpio_dt_spec rows[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(krow1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(krow2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(krow3), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(krow4), gpios),
};
static const struct gpio_dt_spec cols[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(kcol1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(kcol2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(kcol3), gpios),
};
static const char keymap[4][3] = {
    {'1','2','3'},
    {'4','5','6'},
    {'7','8','9'},
    {'*','0','#'}
};
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(i2c0))
static const struct device *lcd_i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
#else
#error "The board devicetree must provide an i2c0 node for the LCD"
#endif

static bool lcd_ready;
static uint8_t lcd_backlight = LCD_BACKLIGHT;

#if CONFIG_HALLOWEEN_DFPLAYER
#if !DT_NODE_EXISTS(DT_NODELABEL(dfplayer_uart))
#error "CONFIG_HALLOWEEN_DFPLAYER needs a dfplayer_uart node in the board overlay"
#endif
static const struct device *const dfplayer_dev = DEVICE_DT_GET(DT_NODELABEL(dfplayer_uart));
#endif

#define VALUE_TRACK(value) ((uint16_t)((value) & 0xffff))
#define VALUE_SEQ(value) ((uint8_t)(((value) >> 16) & 0xff))

static uint8_t play_seq;
static uint8_t last_play_seq[LAST_STATION_ID + 1];
static int64_t play_beacon_until_ms;
static bool release_pending; /* station 5: send the release request once the play beacon ends */

static enum station_state state = ST_WAITING;
static uint16_t session_id;
static uint32_t final_code;
static bool final_code_valid;
static int64_t final_code_deadline_ms;
static bool code_hidden;
static bool activation_acked;
static int64_t delayed_track_at_ms; /* 0 = nothing scheduled */
static char entry[CODE_LEN + 1];
static size_t entry_len;
static const char *status_text;
static int64_t status_until_ms;
static int64_t new_round_until_ms;

/* Non-blocking correct-answer celebration. The main loop continues to process
 * BLE messages while the LED runs this pattern: three quick flashes, a pause,
 * then three quick flashes. */
#define WIN_FLASH_MS 100
#define WIN_PAUSE_MS 250
#define WIN_PHASE_COUNT 12
static uint8_t win_phase;
static int64_t win_phase_deadline_ms;

/* BLE scan callback runs outside the main thread. Queue decoded messages
 * so all game-state and advertising changes happen in the main thread. */
K_MSGQ_DEFINE(beacon_msgq, sizeof(struct button_msg), 8, 4);

static uint8_t service_data[SERVICE_DATA_LEN] = {
    (uint8_t)(HALLOWEEN_SERVICE_UUID & 0xff),
    (uint8_t)(HALLOWEEN_SERVICE_UUID >> 8),
    MAGIC_0, MAGIC_1, PROTOCOL_VERSION, MSG_IDLE,
    0, STATION_ID, 0, 0, 0, 0, 0
};

static const uint8_t adv_flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
static struct bt_data ad[] = {
    BT_DATA(BT_DATA_FLAGS, &adv_flags, sizeof(adv_flags)),
    BT_DATA(BT_DATA_SVC_DATA16, service_data, sizeof(service_data)),
};


/* Relay: stations are spread out, so long-range messages are re-advertised by every station that hears
 * them. The relay payload alternates with the station's own beacon so a pending activation keeps going. */
#define RELAY_WINDOW_MS 3000
#define RELAY_SWAP_MS 400
#define RELAY_DEDUP_MS 5000
#define RELAY_CACHE_SIZE 6

struct relay_seen {
    struct button_msg msg;
    int64_t at_ms;
};

static uint8_t relay_data[SERVICE_DATA_LEN];
static struct bt_data relay_ad[] = {
    BT_DATA(BT_DATA_FLAGS, &adv_flags, sizeof(adv_flags)),
    BT_DATA(BT_DATA_SVC_DATA16, relay_data, sizeof(relay_data)),
};
static struct relay_seen relay_cache[RELAY_CACHE_SIZE];
static uint8_t relay_cache_next;
static int64_t relay_until_ms;
static int64_t relay_next_swap_ms;
static bool relay_showing;

static void configure_gpio_voltage_level(void)
{
    if ((NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) !=
        (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos)) {
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
        while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {}
        NRF_UICR->REGOUT0 = (NRF_UICR->REGOUT0 & ~UICR_REGOUT0_VOUT_Msk) |
                            (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
        while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {}
        NVIC_SystemReset();
    }
}

static int lcd_expander_write(uint8_t value)
{
    int rc = 0;

    /* A failed write would drop a nibble and desync the LCD, so retry a few times. */
    for (int attempt = 0; attempt < 3; attempt++) {
        rc = i2c_write(lcd_i2c, &value, sizeof(value), LCD_I2C_ADDR);
        if (rc == 0) break;
    }
    return rc;
}

static int lcd_pulse_enable(uint8_t value)
{
    int rc = lcd_expander_write(value | LCD_EN);
    if (rc < 0) return rc;
    k_busy_wait(1);
    rc = lcd_expander_write(value & (uint8_t)~LCD_EN);
    k_busy_wait(50);
    return rc;
}

static int lcd_write_nibble(uint8_t nibble, bool rs)
{
    uint8_t value = (uint8_t)((nibble & 0x0f) << 4) | lcd_backlight;
    if (rs) value |= LCD_RS;
    /* RW stays low (write). */
    return lcd_pulse_enable(value);
}

static int lcd_send(uint8_t value, bool rs)
{
    int rc = lcd_write_nibble(value >> 4, rs);
    if (rc < 0) return rc;
    return lcd_write_nibble(value & 0x0f, rs);
}

static int lcd_command(uint8_t command)
{
    int rc = lcd_send(command, false);
    if (command == 0x01 || command == 0x02) {
        k_msleep(2);
    }
    return rc;
}

static int lcd_data(uint8_t value)
{
    return lcd_send(value, true);
}

static int lcd_init(void)
{
    if (!device_is_ready(lcd_i2c)) {
        LOG_ERR("I2C controller for LCD is not ready");
        return -ENODEV;
    }

    k_msleep(50);
    int rc = lcd_write_nibble(0x03, false);
    if (rc < 0) return rc;
    k_msleep(5);
    rc = lcd_write_nibble(0x03, false);
    if (rc < 0) return rc;
    k_busy_wait(150);
    rc = lcd_write_nibble(0x03, false);
    if (rc < 0) return rc;
    rc = lcd_write_nibble(0x02, false); /* 4-bit mode */
    if (rc < 0) return rc;

    if ((rc = lcd_command(0x28)) < 0) return rc; /* 4-bit, 2 lines, 5x8 */
    if ((rc = lcd_command(0x08)) < 0) return rc; /* display off */
    if ((rc = lcd_command(0x01)) < 0) return rc; /* clear */
    if ((rc = lcd_command(0x06)) < 0) return rc; /* entry mode */
    if ((rc = lcd_command(0x0c)) < 0) return rc; /* display on */
    return 0;
}

static int lcd_set_cursor(uint8_t column, uint8_t row)
{
    static const uint8_t row_offsets[] = {0x00, 0x40};
    if (row >= LCD_ROWS || column >= LCD_COLS) return -EINVAL;
    return lcd_command(0x80 | (row_offsets[row] + column));
}

static int lcd_print_at(uint8_t column, uint8_t row, const char *text)
{
    int rc = lcd_set_cursor(column, row);
    if (rc < 0) return rc;
    for (size_t i = 0; text[i] != '\0' && column < LCD_COLS; i++, column++) {
        rc = lcd_data((uint8_t)text[i]);
        if (rc < 0) return rc;
    }
    return 0;
}

static void lcd_show_waiting(void)
{
    if (!lcd_ready) return;
    /* Waiting: clear, display off and backlight off (the expander write latches the backlight). */
    (void)lcd_command(0x01);
    (void)lcd_command(0x08);
    lcd_backlight = 0;
    (void)lcd_expander_write(0);
}

static void lcd_wake(void)
{
    if (lcd_backlight) return;
    lcd_backlight = LCD_BACKLIGHT;
    (void)lcd_command(0x0c);
}

static void lcd_show_final(void)
{
    char line1[17];
    char line2[17];

    if (state == ST_RELEASING) {
        snprintk(line1, sizeof(line1), "CORRECT!");
        snprintk(line2, sizeof(line2), "TAKE YOUR CANDY");
    } else {
        if (code_hidden) {
            snprintk(line1, sizeof(line1), "ENTER THE CODE");
        } else {
            snprintk(line1, sizeof(line1), "CODE: %06u", (unsigned int)final_code);
        }

        if (status_text != NULL) {
            snprintk(line2, sizeof(line2), "%s", status_text);
        } else if (entry_len == 0) {
            snprintk(line2, sizeof(line2), "Type code, #=OK");
        } else {
            snprintk(line2, sizeof(line2), "> %s", entry);
        }
    }

#if CONFIG_HALLOWEEN_TEST_SKIP_PUZZLE
    /* Mirror the LCD on the UART so station 5 can be tested without an LCD. */
    LOG_INF("LCD: [%s] [%s]", line1, line2);
#endif

    if (!lcd_ready) return;

    lcd_wake();
    (void)lcd_command(0x01);
    (void)lcd_print_at(0, 0, line1);
    (void)lcd_print_at(0, 1, line2);
}

static void set_led(bool on)
{
    int rc = gpio_pin_set_dt(&led, on ? 1 : 0);
    if (rc < 0) {
        LOG_ERR("LED GPIO write failed: %d", rc);
    }
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(led0))
    /* Mirror on the board LED (Pro Micro P0.15) so the active state is visible. */
    (void)gpio_pin_set_dt(&boot_led, on ? 1 : 0);
#endif
}

/* Non-blocking lightning on the station LED while ACTIVE: a stuttering intro when the station
 * activates that settles to steady on, then sporadic bursts at random intervals. Every burst has an
 * even number of toggles, so it always ends with the LED on. */
#define LIGHTNING_INTRO_TOGGLES 14
#define LIGHTNING_GAP_MIN_MS 3000
#define LIGHTNING_GAP_MAX_MS 12000

static uint8_t lightning_toggles_left;
static bool lightning_level;
static int64_t lightning_next_ms;
static int64_t lightning_burst_at_ms; /* 0 = no burst scheduled */

static uint32_t lightning_rnd(uint32_t min, uint32_t max)
{
    return min + sys_rand32_get() % (max - min + 1U);
}

static void lightning_schedule_burst(void)
{
    lightning_burst_at_ms = k_uptime_get() + lightning_rnd(LIGHTNING_GAP_MIN_MS, LIGHTNING_GAP_MAX_MS);
}

static void lightning_cancel(void)
{
    lightning_toggles_left = 0;
    lightning_burst_at_ms = 0;
}

static void lightning_begin(uint8_t toggles)
{
    lightning_toggles_left = toggles;
    lightning_level = true;
    lightning_next_ms = 0; /* toggle at once */
    lightning_burst_at_ms = 0;
    LOG_INF("Lightning: %u toggles", toggles);
}

static void lightning_start_intro(void)
{
    if (IS_ENABLED(CONFIG_HALLOWEEN_LIGHTNING)) {
        lightning_begin(LIGHTNING_INTRO_TOGGLES);
    }
}

static void lightning_update(void)
{
    int64_t now = k_uptime_get();

    if (!IS_ENABLED(CONFIG_HALLOWEEN_LIGHTNING)) {
        return;
    }
    if (state != ST_ACTIVE) {
        lightning_cancel();
        return;
    }

    if (lightning_toggles_left > 0) {
        if (now < lightning_next_ms) {
            return;
        }
        lightning_level = !lightning_level;
        set_led(lightning_level);
        lightning_toggles_left--;
        if (lightning_toggles_left == 0) {
            lightning_schedule_burst();
        } else if (lightning_level) {
            lightning_next_ms = now + lightning_rnd(20, 90);
        } else {
            /* Mostly short gaps, now and then a longer blackout. */
            lightning_next_ms = now + (lightning_rnd(0, 5) == 0 ? lightning_rnd(300, 700)
                                                                : lightning_rnd(30, 200));
        }
    } else if (lightning_burst_at_ms != 0 && now >= lightning_burst_at_ms) {
        lightning_begin((uint8_t)(2U * lightning_rnd(1, 4)));
    }
}

static void flash_led(unsigned int count, int on_ms, int off_ms)
{
    lightning_cancel();
    for (unsigned int i = 0; i < count; i++) {
        set_led(true);
        k_msleep(on_ms);
        set_led(false);
        k_msleep(off_ms);
    }
    if (state == ST_ACTIVE) {
        set_led(true);
        if (IS_ENABLED(CONFIG_HALLOWEEN_LIGHTNING)) {
            lightning_schedule_burst();
        }
    }
}

static void update_beacon(uint8_t type, uint8_t target, uint32_t value)
{
    if (type != MSG_PLAY_TRACK) {
        play_beacon_until_ms = 0;
    }
    service_data[2] = MAGIC_0;
    service_data[3] = MAGIC_1;
    service_data[4] = PROTOCOL_VERSION;
    service_data[5] = type;
    service_data[6] = target;
    service_data[7] = STATION_ID;
    service_data[8] = (uint8_t)(session_id & 0xff);
    service_data[9] = (uint8_t)(session_id >> 8);
    service_data[10] = (uint8_t)(value & 0xff);
    service_data[11] = (uint8_t)((value >> 8) & 0xff);
    service_data[12] = (uint8_t)((value >> 16) & 0xff);

    int rc = fota_adv_update(ad, ARRAY_SIZE(ad));
    relay_showing = false;
    if (rc < 0) {
        LOG_ERR("Updating BLE beacon failed: %d", rc);
    } else {
        LOG_INF("Beacon updated: type=%u target=%u session=%u",
                type, target, session_id);
    }
}

/* Asks another station (1..5), the dispenser (0) or a music player (10 + N) to play a track. The beacon
 * is repeated for a few seconds and then falls back to idle (see main loop). */
static void send_play_track(uint8_t target, uint16_t track)
{
    if (track == 0) {
        return;
    }
    play_seq = (uint8_t)(play_seq + 1U);
    if (play_seq == 0) {
        play_seq = 1;
    }
    update_beacon(MSG_PLAY_TRACK, target, (uint32_t)track | ((uint32_t)play_seq << 16));
    play_beacon_until_ms = k_uptime_get() + PLAY_BEACON_MS;
}

static void send_remote_track(uint16_t track)
{
    send_play_track(CONFIG_HALLOWEEN_REMOTE_PLAYER > 0
                        ? MUSIC_PLAYER_TARGET_BASE + CONFIG_HALLOWEEN_REMOTE_PLAYER
                        : CONFIG_HALLOWEEN_REMOTE_TARGET,
                    track);
}

/* Called whenever this station becomes the active one. */
static void on_station_active(void)
{
    lightning_start_intro();
    dfplayer_play(CONFIG_HALLOWEEN_ACTIVE_TRACK);
    send_remote_track(CONFIG_HALLOWEEN_REMOTE_TRACK);
}

struct parse_context {
    struct button_msg *msg;
    bool found;
};

static bool parse_ad(struct bt_data *data, void *user_data)
{
    struct parse_context *ctx = user_data;

    if (data->type != BT_DATA_SVC_DATA16 ||
        data->data_len < SERVICE_DATA_LEN) {
        return true;
    }

    if (data->data[0] != (uint8_t)(HALLOWEEN_SERVICE_UUID & 0xff) ||
        data->data[1] != (uint8_t)(HALLOWEEN_SERVICE_UUID >> 8)) {
        return true;
    }

    const uint8_t *p = data->data + 2;
    if (p[0] != MAGIC_0 || p[1] != MAGIC_1 ||
        p[2] != PROTOCOL_VERSION) {
        return true;
    }

    ctx->msg->type = p[3];
    ctx->msg->target = p[4];
    ctx->msg->source = p[5];
    ctx->msg->session = (uint16_t)p[6] | ((uint16_t)p[7] << 8);
    ctx->msg->value = (uint32_t)p[8] |
                      ((uint32_t)p[9] << 8) |
                      ((uint32_t)p[10] << 16);
    ctx->found = true;
    return false;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi,
                         uint8_t adv_type, struct net_buf_simple *ad_buf)
{
    ARG_UNUSED(addr);
    ARG_UNUSED(rssi);
    ARG_UNUSED(adv_type);

    struct button_msg msg = {0};
    struct parse_context ctx = {
        .msg = &msg,
        .found = false,
    };

    bt_data_parse(ad_buf, parse_ad, &ctx);
    if (ctx.found) {
        (void)k_msgq_put(&beacon_msgq, &msg, K_NO_WAIT);
    }
}

static void clear_entry(void)
{
    entry_len = 0;
    memset(entry, 0, sizeof(entry));
}

/* Reset this station for a new round with the given session. */
static void begin_round(uint16_t session)
{
    session_id = session;
    final_code_valid = false;
    final_code = 0;
    final_code_deadline_ms = 0;
    code_hidden = false;
    activation_acked = false;
    delayed_track_at_ms = 0;
    release_pending = false;
    status_text = NULL;
    clear_entry();

    if (STATION_ID == 1) {
        state = ST_ACTIVE;
        set_led(true);
        LOG_INF("New round; station 1 ACTIVE (session=%u)", session_id);
    } else {
        state = ST_WAITING;
        set_led(false);
        LOG_INF("New round; station %d WAITING (session=%u)", STATION_ID, session_id);
    }

    update_beacon(MSG_IDLE, 0, 0);
    if (STATION_ID == LAST_STATION_ID) {
        lcd_show_waiting();
    }
    if (state == ST_ACTIVE) {
        on_station_active();
    }
}

static void finish_correct_answer(void);

/* Station 5 only: the candy was released, so announce the next round. */
/* Station 1 only: starts a new game with a fresh session and tells every station to reset to it. */
static void start_game(void)
{
    uint16_t session;

    do {
        session = (uint16_t)sys_rand32_get();
    } while (session == 0 || session == session_id);

    LOG_INF("Station 1 starts the game (session=%u)", session);
    begin_round(session);
    /* The start track is sent when the announcement ends. */
    update_beacon(MSG_NEW_ROUND, 0, 0);
    new_round_until_ms = k_uptime_get() + RESTART_BEACON_MS;
}

static void announce_new_round(void)
{
    uint16_t session;

    do {
        session = (uint16_t)sys_rand32_get();
    } while (session == 0 || session == session_id);

    begin_round(session);
#if CONFIG_HALLOWEEN_TEST_SKIP_PUZZLE
    /* Test: stay in WAITING until an answer button is pressed (see main loop). */
    LOG_INF("TEST: candy released; press any answer button for a new code");
    return;
#endif
    update_beacon(MSG_NEW_ROUND, 0, 0);
    new_round_until_ms = k_uptime_get() + NEW_ROUND_BEACON_MS;
}

#if CONFIG_HALLOWEEN_TEST_RELEASE_INTERVAL_S > 0
/* Test only: ask the dispenser for a candy with a fresh session, without the puzzle. */
static void test_release(void)
{
    uint16_t session;

    do {
        session = (uint16_t)sys_rand32_get();
    } while (session == 0 || session == session_id);

    session_id = session;
    final_code_valid = false;
    clear_entry();
    state = ST_RELEASING;
    update_beacon(MSG_RELEASE_CANDY, DISPENSER_ID, CONFIG_HALLOWEEN_DISPENSER_TRACK);
    lcd_show_final();
    LOG_INF("TEST: asking dispenser to release candy (session=%u)", session_id);
    flash_led(1, 200, 0);
}
#endif

static void maybe_relay(const struct button_msg *msg)
{
    if (msg->type != MSG_NEW_ROUND && msg->type != MSG_RELEASE_CANDY &&
        msg->type != MSG_CANDY_DONE && msg->type != MSG_PLAY_TRACK) {
        return;
    }
    /* Own messages and messages addressed to this station need no relay (target 0 = everyone/dispenser). */
    if (msg->source == STATION_ID ||
        (msg->target == STATION_ID && msg->type != MSG_NEW_ROUND)) {
        return;
    }
    /* One relay at a time; the sender repeats its beacon, so a skipped message is caught on the next one. */
    if (relay_until_ms != 0) {
        return;
    }

    int64_t now = k_uptime_get();
    for (size_t i = 0; i < RELAY_CACHE_SIZE; i++) {
        const struct relay_seen *seen = &relay_cache[i];

        if (seen->at_ms != 0 && now - seen->at_ms < RELAY_DEDUP_MS &&
            seen->msg.type == msg->type && seen->msg.target == msg->target &&
            seen->msg.source == msg->source && seen->msg.session == msg->session &&
            seen->msg.value == msg->value) {
            return;
        }
    }
    relay_cache[relay_cache_next].msg = *msg;
    relay_cache[relay_cache_next].at_ms = now;
    relay_cache_next = (relay_cache_next + 1U) % RELAY_CACHE_SIZE;

    relay_data[0] = (uint8_t)(HALLOWEEN_SERVICE_UUID & 0xff);
    relay_data[1] = (uint8_t)(HALLOWEEN_SERVICE_UUID >> 8);
    relay_data[2] = MAGIC_0;
    relay_data[3] = MAGIC_1;
    relay_data[4] = PROTOCOL_VERSION;
    relay_data[5] = msg->type;
    relay_data[6] = msg->target;
    relay_data[7] = msg->source;
    relay_data[8] = (uint8_t)(msg->session & 0xff);
    relay_data[9] = (uint8_t)(msg->session >> 8);
    relay_data[10] = (uint8_t)(msg->value & 0xff);
    relay_data[11] = (uint8_t)((msg->value >> 8) & 0xff);
    relay_data[12] = (uint8_t)((msg->value >> 16) & 0xff);

    relay_showing = false;
    relay_next_swap_ms = now;
    relay_until_ms = now + RELAY_WINDOW_MS;
    LOG_INF("Relaying type=%u source=%u target=%u", msg->type, msg->source, msg->target);
}

static void relay_tick(void)
{
    if (relay_until_ms == 0) {
        return;
    }

    int64_t now = k_uptime_get();

    if (now >= relay_until_ms) {
        relay_until_ms = 0;
        relay_showing = false;
        (void)fota_adv_update(ad, ARRAY_SIZE(ad));
    } else if (now >= relay_next_swap_ms) {
        relay_next_swap_ms = now + RELAY_SWAP_MS;
        relay_showing = !relay_showing;
        (void)fota_adv_update(relay_showing ? relay_ad : ad,
                              relay_showing ? ARRAY_SIZE(relay_ad) : ARRAY_SIZE(ad));
    }
}

static void process_beacon(const struct button_msg *msg)
{
    maybe_relay(msg);

    /* Station 5 starts a new round after the candy was released, and a (re)booted station 1 restarts
     * the whole game. */
    if (msg->type == MSG_NEW_ROUND &&
        msg->target == 0 &&
        ((msg->source == LAST_STATION_ID && STATION_ID != LAST_STATION_ID) ||
         (msg->source == 1 && STATION_ID != 1)) &&
        msg->session != 0 &&
        msg->session != session_id) {
        begin_round(msg->session);
        return;
    }

    /* The dispenser reports that the candy was released. */
    if (STATION_ID == LAST_STATION_ID &&
        state == ST_RELEASING &&
        msg->type == MSG_CANDY_DONE &&
        msg->source == DISPENSER_ID &&
        msg->target == LAST_STATION_ID &&
        msg->session == session_id) {
        LOG_INF("Dispenser confirmed candy release");
        announce_new_round();
        return;
    }

    /* Another station asks us to play a track; beacons repeat, so play each sequence once. */
    if (msg->type == MSG_PLAY_TRACK &&
        msg->target == STATION_ID &&
        msg->source <= LAST_STATION_ID &&
        msg->source != STATION_ID &&
        VALUE_SEQ(msg->value) != last_play_seq[msg->source]) {
        last_play_seq[msg->source] = VALUE_SEQ(msg->value);
        LOG_INF("Station %d asks for track %u", msg->source, VALUE_TRACK(msg->value));
        dfplayer_play(VALUE_TRACK(msg->value));
        return;
    }

    /* The next station answering with our session proves it got the activation,
     * so stop advertising it (a stale activation could re-trigger a reboot). */
    if (state == ST_COMPLETED && STATION_ID < LAST_STATION_ID &&
        !activation_acked &&
        ((msg->type == MSG_IDLE && (msg->value & IDLE_ACTIVE_FLAG)) ||
         msg->type == MSG_PLAY_TRACK) &&
        msg->source >= NEXT_STATION_ID &&
        msg->session == session_id) {
        activation_acked = true;
        update_beacon(MSG_IDLE, 0, 0);
        LOG_INF("Station %d acknowledged; activation beacon stopped", msg->source);
        return;
    }

    /* Accept activation only from our own round (or if we have not synced yet). */
    if (msg->type == MSG_ACTIVATE &&
        msg->target == STATION_ID &&
        (STATION_ID != 1 || IS_ENABLED(CONFIG_HALLOWEEN_TEST_PINGPONG)) &&
        (state == ST_WAITING ||
         (IS_ENABLED(CONFIG_HALLOWEEN_TEST_PINGPONG) && state == ST_COMPLETED)) &&
        (session_id == 0 || msg->session == session_id)) {
        session_id = msg->session;
        if (session_id == 0) {
            session_id = 1;
        }
        state = ST_ACTIVE;
        activation_acked = false;
        set_led(true);
        LOG_INF("Station %d ACTIVE (source=%d session=%u)",
                STATION_ID, msg->source, session_id);
        update_beacon(MSG_IDLE, 0, IDLE_ACTIVE_FLAG);
        dfplayer_play(VALUE_TRACK(msg->value));
        on_station_active();
    }
}

static bool button_is_pressed(const struct gpio_dt_spec *button)
{
    int value = gpio_pin_get_dt(button);
    return value > 0;
}

/* Six digits without 4, 5, 6 (those keys are unusable on the keypad); no leading 0. */
static uint32_t generate_code(void)
{
    static const char digits[] = {0, 1, 2, 3, 7, 8, 9};
    uint32_t code = 0;

    for (int i = 0; i < CODE_LEN; i++) {
        uint32_t d;

        do {
            d = (uint32_t)digits[sys_rand32_get() % ARRAY_SIZE(digits)];
        } while (i == 0 && d == 0);
        code = code * 10 + d;
    }
    return code;
}

static void finish_correct_answer(void)
{
    state = ST_COMPLETED;
    set_led(false);

    if (STATION_ID < LAST_STATION_ID) {
        /* Keep this message in the advertising payload so it is repeated until
         * the next station receives it. The next station only accepts it once. */
        update_beacon(MSG_ACTIVATE, NEXT_STATION_ID, CONFIG_HALLOWEEN_NEXT_STATION_TRACK);
        LOG_INF("Station %d completed; advertising activation for station %d",
                STATION_ID, NEXT_STATION_ID);
    } else {
        final_code = generate_code();
        final_code_valid = true;
        code_hidden = false;
        final_code_deadline_ms = (CODE_DISPLAY_MS > 0) ?
            k_uptime_get() + CODE_DISPLAY_MS : 0;
        clear_entry();
        status_text = NULL;
        state = ST_ENTER_CODE;
        update_beacon(MSG_IDLE, 0, 0);
        lcd_show_final();
        LOG_INF("Final code generated; waiting for keypad entry");
    }
}

static void start_winning_pattern(void)
{
    state = ST_CELEBRATING;
    win_phase = 0;
    set_led(true);
    dfplayer_play(CONFIG_HALLOWEEN_CORRECT_TRACK);
    send_remote_track(CONFIG_HALLOWEEN_REMOTE_CORRECT_TRACK);
    if (CONFIG_HALLOWEEN_REMOTE_DELAYED_TRACK > 0) {
        delayed_track_at_ms = k_uptime_get() + CONFIG_HALLOWEEN_REMOTE_DELAYED_SECONDS * 1000;
    }
    win_phase_deadline_ms = k_uptime_get() + WIN_FLASH_MS;
    LOG_INF("Correct answer at station %d; playing winning LED pattern", STATION_ID);
}

static void update_winning_pattern(void)
{
    if (state != ST_CELEBRATING || k_uptime_get() < win_phase_deadline_ms) {
        return;
    }

    win_phase++;
    if (win_phase >= WIN_PHASE_COUNT) {
        finish_correct_answer();
        return;
    }

    /* Even phases are ON, odd phases OFF. Phase 5 is the longer pause between
     * the first and second groups of three flashes. */
    set_led((win_phase % 2U) == 0U);
    int duration_ms = (win_phase == 5U) ? WIN_PAUSE_MS : WIN_FLASH_MS;
    win_phase_deadline_ms = k_uptime_get() + duration_ms;
}

static bool restart_buttons_held(void)
{
    return STATION_ID == 1 && button_is_pressed(&buttons[0]) && button_is_pressed(&buttons[1]);
}

static bool restart_fired;
static int64_t restart_hold_start_ms;

/* Station 1: restarts the game once buttons 1 and 2 have been held together for RESTART_HOLD_MS. */
static void restart_combo_tick(void)
{
    if (STATION_ID != 1) {
        return;
    }
    if (!button_is_pressed(&buttons[0]) && !button_is_pressed(&buttons[1])) {
        restart_fired = false;
    }
    if (!restart_buttons_held()) {
        restart_hold_start_ms = 0;
        return;
    }

    int64_t now = k_uptime_get();

    if (restart_hold_start_ms == 0) {
        restart_hold_start_ms = now;
    } else if (!restart_fired && now - restart_hold_start_ms >= RESTART_HOLD_MS) {
        restart_fired = true;
        LOG_INF("Buttons 1+2 held: restarting the game");
        start_game();
    }
}

static void handle_answer(unsigned int button_number)
{
    LOG_INF("Button %u pressed", button_number);

    if (state != ST_ACTIVE) {
        return;
    }

    if (button_number != CORRECT_BUTTON) {
        LOG_INF("Wrong answer at station %d", STATION_ID);
        dfplayer_play(CONFIG_HALLOWEEN_WRONG_TRACK);
        send_remote_track(CONFIG_HALLOWEEN_REMOTE_WRONG_TRACK);
        flash_led(3, 100, 100);
        return;
    }

    start_winning_pattern();
}

#ifdef HAS_KEYPAD
#define KEYPAD_SETTLE_US 50
#define KEYPAD_RELEASE_MS 15
#define KEYPAD_POLL_MS 2

/* Returns the key currently pressed (scan only, never blocks). */
static char keypad_scan_pressed(void)
{
    for (size_t r = 0; r < ARRAY_SIZE(rows); r++) {
        for (size_t i = 0; i < ARRAY_SIZE(rows); i++) {
            (void)gpio_pin_set_dt(&rows[i], 1); /* inactive/high */
        }
        (void)gpio_pin_set_dt(&rows[r], 0); /* selected row low */
        k_busy_wait(KEYPAD_SETTLE_US);

        for (size_t c = 0; c < ARRAY_SIZE(cols); c++) {
            if (gpio_pin_get_dt(&cols[c]) == 0) {
                return keymap[r][c];
            }
        }
    }
    return '\0';
}

/* Reports a key the moment it is pressed (lowest latency) and ignores
 * contact bounce by requiring the key to be released for KEYPAD_RELEASE_MS. */
static char keypad_poll(void)
{
    static char held;
    static int64_t last_seen_ms;
    char key = keypad_scan_pressed();
    int64_t now = k_uptime_get();

    if (key) {
        last_seen_ms = now;
        if (key != held) {
            held = key;
            return key;
        }
    } else if (held && now - last_seen_ms >= KEYPAD_RELEASE_MS) {
        held = '\0';
    }
    return '\0';
}

#if STATION_ID == LAST_STATION_ID
static void handle_key(char key)
{
    if (!key || state != ST_ENTER_CODE) return;

    status_text = NULL;

    if (key == '*') {
        clear_entry();
    } else if (key == '#') {
        if (entry_len != CODE_LEN) {
            status_text = "NEED 6 DIGITS";
            status_until_ms = k_uptime_get() + STATUS_SHOW_MS;
        } else {
            uint32_t entered = 0;
            for (size_t i = 0; i < CODE_LEN; i++) {
                entered = entered * 10 + (uint32_t)(entry[i] - '0');
            }

            if (final_code_valid && entered == final_code) {
                /* Invalidate the code before telling the dispenser to release. */
                final_code_valid = false;
                final_code = 0;
                clear_entry();
                state = ST_RELEASING;
                /* The correct-code track shares the beacon slot, so the release request follows it. */
                dfplayer_play(CONFIG_HALLOWEEN_CORRECT_TRACK);
                if (CONFIG_HALLOWEEN_REMOTE_CORRECT_TRACK > 0) {
                    send_remote_track(CONFIG_HALLOWEEN_REMOTE_CORRECT_TRACK);
                    release_pending = true;
                } else {
                    update_beacon(MSG_RELEASE_CANDY, DISPENSER_ID, CONFIG_HALLOWEEN_DISPENSER_TRACK);
                }
                LOG_INF("Correct code; asking dispenser to release candy");
            } else {
                status_text = "WRONG CODE!";
                status_until_ms = k_uptime_get() + STATUS_SHOW_MS;
                clear_entry();
                dfplayer_play(CONFIG_HALLOWEEN_WRONG_TRACK);
                send_remote_track(CONFIG_HALLOWEEN_REMOTE_WRONG_TRACK);
                /* Show the correct code again for another display period. */
                code_hidden = false;
                final_code_deadline_ms = (CODE_DISPLAY_MS > 0) ?
                    k_uptime_get() + CODE_DISPLAY_MS : 0;
            }
        }
    } else if (key >= '0' && key <= '9' && entry_len < CODE_LEN) {
        entry[entry_len++] = key;
    }

    lcd_show_final();
}
#endif /* STATION_ID == LAST_STATION_ID */

#if CONFIG_HALLOWEEN_TEST_PINGPONG_KEYPAD
#define PP_CODE CONFIG_HALLOWEEN_TEST_PINGPONG_CODE
static char pp_entry[sizeof(PP_CODE)];
static size_t pp_entry_len;

/* Test: the fixed code ('#' submits, '*' clears) is a second way to pass the activation on. */
static void handle_pingpong_key(char key)
{
    if (!key || state != ST_ACTIVE) {
        return;
    }

    LOG_INF("Key '%c'", key);

    if (key == '*') {
        pp_entry_len = 0;
    } else if (key == '#') {
        bool ok = pp_entry_len == strlen(PP_CODE) &&
                  memcmp(pp_entry, PP_CODE, pp_entry_len) == 0;

        LOG_INF("Entered %.*s, expected %s", (int)pp_entry_len, pp_entry, PP_CODE);
        pp_entry_len = 0;
        if (ok) {
            LOG_INF("Correct code at station %d", STATION_ID);
            start_winning_pattern();
        } else {
            LOG_INF("Wrong code at station %d", STATION_ID);
            flash_led(3, 100, 100);
        }
    } else if (key >= '0' && key <= '9' && pp_entry_len < strlen(PP_CODE)) {
        pp_entry[pp_entry_len++] = key;
    }
}
#endif
#endif

#if CONFIG_HALLOWEEN_TEST_IO
/* Resyncs the LCD's 4-bit interface without clearing or blanking the display (no flicker). */
static void lcd_resync(void)
{
    (void)lcd_write_nibble(0x03, false);
    k_msleep(5);
    (void)lcd_write_nibble(0x03, false);
    k_busy_wait(150);
    (void)lcd_write_nibble(0x03, false);
    (void)lcd_write_nibble(0x02, false);
    (void)lcd_command(0x28);
    (void)lcd_command(0x0c);
}

#if STATION_ID != LAST_STATION_ID
#error "CONFIG_HALLOWEEN_TEST_IO only works for station 5"
#endif
#define IO_CODE CONFIG_HALLOWEEN_TEST_IO_CODE

/* Test: shows the last button and key on the LCD; typing IO_CODE starts the DFPlayer. */
static void io_test_loop(void)
{
    char entry_buf[sizeof(IO_CODE)] = "";
    size_t len = 0;
    char last_key = '-';
    unsigned int last_button = 0;
    const char *result = "";
    bool pressed[ARRAY_SIZE(buttons)] = {false};

    LOG_INF("TEST IO: press buttons and keys; type %s to play track %d",
            IO_CODE, CONFIG_HALLOWEEN_TEST_IO_TRACK);
    lcd_wake();

    bool changed = true; /* draw once at start */
    int64_t last_draw_ms = 0;

    while (1) {
        while (!changed) {
            /* Redraw every second so any garbage heals itself. */
            if (k_uptime_get() - last_draw_ms >= 1000) {
                changed = true;
                break;
            }

            for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
                bool now = button_is_pressed(&buttons[i]);

                /* Debounce: a press only counts if it is still there 30 ms later. */
                if (now && !pressed[i]) {
                    k_msleep(30);
                    now = button_is_pressed(&buttons[i]);
                }

                if (now && !pressed[i]) {
                    last_button = (unsigned int)i + 1;
                    result = "";
                    changed = true;
                    LOG_INF("Button %u pressed", last_button);
                    /* Do not touch the LCD while the button is held or bouncing (noise on I2C). */
                    while (button_is_pressed(&buttons[i])) {
                        k_msleep(10);
                    }
                    k_msleep(50);
                    now = false;
                }
                pressed[i] = now;
            }

            char key = IS_ENABLED(CONFIG_HALLOWEEN_TEST_IO_NO_KEYPAD) ? '\0' : keypad_poll();

            if (key) {
                last_key = key;
                result = "";
                changed = true;
                LOG_INF("Key '%c' pressed", key);

                if (key == '*') {
                    len = 0;
                } else if (key >= '0' && key <= '9') {
                    if (len >= strlen(IO_CODE)) len = 0;
                    entry_buf[len++] = key;
                    entry_buf[len] = '\0';

                    if (len == strlen(IO_CODE)) {
                        if (memcmp(entry_buf, IO_CODE, len) == 0) {
                            result = "CORRECT! PLAYING";
                            LOG_INF("Correct keys, playing track %d", CONFIG_HALLOWEEN_TEST_IO_TRACK);
#if CONFIG_HALLOWEEN_DFPLAYER
                            dfplayer_play(CONFIG_HALLOWEEN_TEST_IO_TRACK);
#else
                            LOG_WRN("DFPlayer not enabled in this build");
#endif
                        } else {
                            result = "WRONG CODE";
                        }
                        len = 0;
                    }
                }
                entry_buf[len] = '\0';
            }
            k_msleep(KEYPAD_POLL_MS);
        }

        char line1[17];
        char line2[17];
        char text[24];

        if (last_button) {
            snprintk(text, sizeof(text), "BTN:%u  KEY:%c", last_button, last_key);
        } else {
            snprintk(text, sizeof(text), "BTN:-  KEY:%c", last_key);
        }
        /* Pad to the full width instead of clearing, so the LCD does not flicker. */
        snprintk(line1, sizeof(line1), "%-16.16s", text);
        if (result[0] != '\0') {
            snprintk(text, sizeof(text), "%s", result);
        } else {
            snprintk(text, sizeof(text), "> %s", entry_buf);
        }
        snprintk(line2, sizeof(line2), "%-16.16s", text);
        if (lcd_ready) {
            /* A glitch on the I2C lines can desync the LCD's 4-bit interface and show garbage;
             * resyncing before every redraw heals it. The text is overwritten, never cleared. */
            lcd_resync();
            (void)lcd_print_at(0, 0, line1);
            (void)lcd_print_at(0, 1, line2);
        }
        changed = false;
        last_draw_ms = k_uptime_get();
    }
}
#endif /* CONFIG_HALLOWEEN_TEST_IO */

int main(void)
{
    int rc;

    configure_gpio_voltage_level();

    boot_blink();

    LOG_INF("Halloween station firmware v%s: station %d, correct button %d",
            APP_VERSION_STRING, STATION_ID, CORRECT_BUTTON);

    rc = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (rc < 0) {
        LOG_ERR("Cannot configure LED output: %d", rc);
        return 0;
    }

    for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
        rc = gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);
        if (rc < 0) {
            LOG_ERR("Cannot configure button %u: %d", (unsigned int)(i + 1), rc);
            return 0;
        }
    }

#ifdef HAS_KEYPAD
    for (size_t i = 0; i < ARRAY_SIZE(rows); i++) {
        rc = gpio_pin_configure_dt(&rows[i], GPIO_OUTPUT_ACTIVE);
        if (rc < 0) {
            LOG_ERR("Keypad row setup failed: %d", rc);
            return 0;
        }
        (void)gpio_pin_set_dt(&rows[i], 1);
    }
    for (size_t i = 0; i < ARRAY_SIZE(cols); i++) {
        rc = gpio_pin_configure_dt(&cols[i], GPIO_INPUT | GPIO_PULL_UP);
        if (rc < 0) {
            LOG_ERR("Keypad column setup failed: %d", rc);
            return 0;
        }
    }
#endif

    if (STATION_ID == LAST_STATION_ID) {
        rc = lcd_init();
        if (rc < 0) {
            LOG_ERR("LCD init failed: %d; station will continue without LCD", rc);
        } else {
            lcd_ready = true;
            lcd_show_waiting();
        }
    }

    rc = bt_enable(NULL);
    if (rc < 0) {
        LOG_ERR("Bluetooth init failed: %d", rc);
        return 0;
    }

#if CONFIG_HALLOWEEN_TEST_PINGPONG
    static const char fota_name[] = "pingpong" STRINGIFY(STATION_ID);
#elif CONFIG_HALLOWEEN_TEST_IO
    static const char fota_name[] = "iotest" STRINGIFY(STATION_ID);
#else
    /* "HW<station>-v<version>", e.g. HW3-v1.0.0; at most 11 characters (see fota.h) */
    static const char fota_name[] = "HW" STRINGIFY(STATION_ID) "-v" APP_VERSION_STRING;
    BUILD_ASSERT(sizeof(fota_name) - 1 <= 11, "version too long for the advertised name");
#endif

    rc = fota_adv_start(ad, ARRAY_SIZE(ad), fota_name);
    if (rc < 0) {
        LOG_ERR("BLE advertising start failed: %d", rc);
        return 0;
    }
    fota_confirm_image();

#if CONFIG_HALLOWEEN_DFPLAYER
    (void)dfplayer_init(dfplayer_dev, CONFIG_HALLOWEEN_DFPLAYER_VOLUME);
#endif

#if CONFIG_HALLOWEEN_TEST_IO
    io_test_loop();
#endif

    rc = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found);
    if (rc < 0) {
        LOG_ERR("BLE scanning start failed: %d", rc);
        return 0;
    }

    if (STATION_ID == 1) {
        start_game();
    } else {
        state = ST_WAITING;
        set_led(false);
        LOG_INF("Waiting for activation beacon for station %d", STATION_ID);
    }

#if CONFIG_HALLOWEEN_TEST_SKIP_PUZZLE
    /* Test: pretend the puzzle was solved and show the code right away. */
    session_id = (uint16_t)sys_rand32_get() | 1U;
    finish_correct_answer();
    LOG_INF("TEST: puzzle skipped, enter the code printed above on the keypad");
#endif

    while (1) {
        if (STATION_ID == LAST_STATION_ID) {
            int64_t now = k_uptime_get();

#if CONFIG_HALLOWEEN_TEST_RELEASE_INTERVAL_S > 0
            static int64_t next_test_release_ms;

            if (now >= next_test_release_ms) {
                next_test_release_ms = now + CONFIG_HALLOWEEN_TEST_RELEASE_INTERVAL_S * 1000;
                test_release();
            }
#endif

            if (state == ST_ENTER_CODE && !code_hidden &&
                final_code_deadline_ms != 0 && now >= final_code_deadline_ms) {
                code_hidden = true;
                lcd_show_final();
                LOG_INF("Final code hidden on LCD");
            }
            if (state == ST_ENTER_CODE && status_text != NULL &&
                now >= status_until_ms) {
                status_text = NULL;
                lcd_show_final();
            }
            /* Stop announcing the new round after a while. */
            if (new_round_until_ms != 0 && now >= new_round_until_ms) {
                new_round_until_ms = 0;
                if (state == ST_WAITING || state == ST_ACTIVE) {
                    update_beacon(MSG_IDLE, 0, 0);
                }
            }
        }

        if (STATION_ID == 1 && new_round_until_ms != 0 && k_uptime_get() >= new_round_until_ms) {
            new_round_until_ms = 0;
            if (state == ST_ACTIVE) {
                update_beacon(MSG_IDLE, 0, IDLE_ACTIVE_FLAG);
                send_remote_track(CONFIG_HALLOWEEN_REMOTE_TRACK);
            }
        }

        if (play_beacon_until_ms != 0 && k_uptime_get() >= play_beacon_until_ms) {
            if (release_pending) {
                release_pending = false;
                update_beacon(MSG_RELEASE_CANDY, DISPENSER_ID, CONFIG_HALLOWEEN_DISPENSER_TRACK);
            } else {
                update_beacon(MSG_IDLE, 0, state == ST_ACTIVE ? IDLE_ACTIVE_FLAG : 0);
            }
        }

        /* The beacon slot is shared with the activation message, so the delayed track waits until the
         * next station has acknowledged the activation. */
        if (delayed_track_at_ms != 0 && k_uptime_get() >= delayed_track_at_ms &&
            state != ST_CELEBRATING && (STATION_ID == LAST_STATION_ID || activation_acked)) {
            delayed_track_at_ms = 0;
            send_remote_track(CONFIG_HALLOWEEN_REMOTE_DELAYED_TRACK);
        }

        struct button_msg msg;
        while (k_msgq_get(&beacon_msgq, &msg, K_NO_WAIT) == 0) {
            process_beacon(&msg);
        }
        relay_tick();

        update_winning_pattern();
        lightning_update();
        restart_combo_tick();

#if CONFIG_HALLOWEEN_TEST_SKIP_PUZZLE
        if (state == ST_WAITING) {
            for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
                if (button_is_pressed(&buttons[i])) {
                    k_msleep(30);
                    if (button_is_pressed(&buttons[i])) {
                        while (button_is_pressed(&buttons[i])) {
                            k_msleep(10);
                        }
                        LOG_INF("Button %u pressed", (unsigned int)i + 1);
                        finish_correct_answer();
                        break;
                    }
                }
            }
        }
#endif

        if (state == ST_ACTIVE) {
            for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
                if (button_is_pressed(&buttons[i])) {
                    /* Simple debounce; wait for release to avoid repeat events. */
                    k_msleep(30);
                    if (button_is_pressed(&buttons[i])) {
                        bool combo = false;

                        if (STATION_ID == 1 && i < 2) {
                            k_msleep(RESTART_GRACE_MS);
                            combo = restart_buttons_held();
                        }
                        if (!combo && !restart_fired) {
                            handle_answer((unsigned int)i + 1);
                        }
                        while (button_is_pressed(&buttons[i])) {
                            restart_combo_tick();
                            lightning_update();
                            k_msleep(10);
                        }
                        break;
                    }
                }
            }
        }

#if STATION_ID == LAST_STATION_ID
        if (state == ST_ENTER_CODE) {
            handle_key(keypad_poll());
        }
#endif
#if CONFIG_HALLOWEEN_TEST_PINGPONG_KEYPAD
        if (state == ST_ACTIVE) {
            handle_pingpong_key(keypad_poll());
        }
#endif

#ifdef HAS_KEYPAD
        k_msleep((state == ST_ENTER_CODE || state == ST_ACTIVE) ? KEYPAD_POLL_MS : 10);
#else
        k_msleep(10);
#endif
    }

    return 0;
}
