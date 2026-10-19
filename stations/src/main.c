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

#include "fota.h"

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
#define DISPENSER_ID 0
#define NEW_ROUND_BEACON_MS 60000
#define STATUS_SHOW_MS 2000
#define CODE_LEN 6

/* Payload following the 2-byte service UUID:
 * [0] magic H, [1] magic W, [2] version, [3] type,
 * [4] target station, [5] source station,
 * [6..7] session ID (little endian),
 * [8..10] value (24-bit little endian; currently unused).
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

#if STATION_ID == LAST_STATION_ID
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

static enum station_state state = ST_WAITING;
static uint16_t session_id;
static uint32_t final_code;
static bool final_code_valid;
static int64_t final_code_deadline_ms;
static bool code_hidden;
static bool activation_acked;
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


static int lcd_expander_write(uint8_t value)
{
    return i2c_write(lcd_i2c, &value, sizeof(value), LCD_I2C_ADDR);
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
    uint8_t value = (uint8_t)((nibble & 0x0f) << 4) | LCD_BACKLIGHT;
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
    (void)lcd_command(0x01);
    (void)lcd_print_at(0, 0, "HALLOWEEN PUZZLE");
    (void)lcd_print_at(0, 1, "Station 5 waiting");
}

static void lcd_show_final(void)
{
    char line1[17];
    char line2[17];

    if (!lcd_ready) return;

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
}

static void flash_led(unsigned int count, int on_ms, int off_ms)
{
    for (unsigned int i = 0; i < count; i++) {
        set_led(true);
        k_msleep(on_ms);
        set_led(false);
        k_msleep(off_ms);
    }
    if (state == ST_ACTIVE) {
        set_led(true);
    }
}

static void update_beacon(uint8_t type, uint8_t target, uint32_t value)
{
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
    if (rc < 0) {
        LOG_ERR("Updating BLE beacon failed: %d", rc);
    } else {
        LOG_INF("Beacon updated: type=%u target=%u session=%u",
                type, target, session_id);
    }
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
}

/* Station 5 only: the candy was released, so announce the next round. */
static void announce_new_round(void)
{
    uint16_t session;

    do {
        session = (uint16_t)sys_rand32_get();
    } while (session == 0 || session == session_id);

    begin_round(session);
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
    update_beacon(MSG_RELEASE_CANDY, DISPENSER_ID, 0);
    lcd_show_final();
    LOG_INF("TEST: asking dispenser to release candy (session=%u)", session_id);
    flash_led(1, 200, 0);
}
#endif

static void process_beacon(const struct button_msg *msg)
{
    /* Station 5 starts a new round after the candy was released. */
    if (msg->type == MSG_NEW_ROUND &&
        msg->target == 0 &&
        msg->source == LAST_STATION_ID &&
        STATION_ID != LAST_STATION_ID &&
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

    /* The next station answering with our session proves it got the activation,
     * so stop advertising it (a stale activation could re-trigger a reboot). */
    if (state == ST_COMPLETED && STATION_ID < LAST_STATION_ID &&
        !activation_acked &&
        msg->source > STATION_ID &&
        msg->session == session_id) {
        activation_acked = true;
        update_beacon(MSG_IDLE, 0, 0);
        LOG_INF("Station %d acknowledged; activation beacon stopped", msg->source);
        return;
    }

    /* Accept activation only from our own round (or if we have not synced yet). */
    if (msg->type == MSG_ACTIVATE &&
        msg->target == STATION_ID &&
        STATION_ID != 1 &&
        state == ST_WAITING &&
        (session_id == 0 || msg->session == session_id)) {
        session_id = msg->session;
        if (session_id == 0) {
            session_id = 1;
        }
        state = ST_ACTIVE;
        set_led(true);
        LOG_INF("Station %d ACTIVE (source=%d session=%u)",
                STATION_ID, msg->source, session_id);
        update_beacon(MSG_IDLE, 0, 0);
    }
}

static bool button_is_pressed(const struct gpio_dt_spec *button)
{
    int value = gpio_pin_get_dt(button);
    return value > 0;
}

static void finish_correct_answer(void)
{
    state = ST_COMPLETED;
    set_led(false);

    if (STATION_ID < LAST_STATION_ID) {
        /* Keep this message in the advertising payload so it is repeated until
         * the next station receives it. The next station only accepts it once. */
        update_beacon(MSG_ACTIVATE, STATION_ID + 1, 0);
        LOG_INF("Station %d completed; advertising activation for station %d",
                STATION_ID, STATION_ID + 1);
    } else {
        /* 100000..999999, fits in the protocol's 24-bit value field. */
        final_code = 100000U + (sys_rand32_get() % 900000U);
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

static void handle_answer(unsigned int button_number)
{
    if (state != ST_ACTIVE) {
        return;
    }

    if (button_number != CORRECT_BUTTON) {
        LOG_INF("Wrong answer at station %d", STATION_ID);
        flash_led(3, 100, 100);
        return;
    }

    start_winning_pattern();
}

#if STATION_ID == LAST_STATION_ID
static char keypad_scan_once(void)
{
    for (size_t r = 0; r < ARRAY_SIZE(rows); r++) {
        for (size_t i = 0; i < ARRAY_SIZE(rows); i++) {
            (void)gpio_pin_set_dt(&rows[i], 1); /* inactive/high */
        }
        (void)gpio_pin_set_dt(&rows[r], 0); /* selected row low */
        k_busy_wait(100);

        for (size_t c = 0; c < ARRAY_SIZE(cols); c++) {
            if (gpio_pin_get_dt(&cols[c]) == 0) {
                char key = keymap[r][c];
                /* Wait for release to avoid repeating a held key. */
                while (gpio_pin_get_dt(&cols[c]) == 0) {
                    k_msleep(10);
                }
                k_msleep(25);
                return key;
            }
        }
    }
    return '\0';
}

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
                update_beacon(MSG_RELEASE_CANDY, DISPENSER_ID, 0);
                LOG_INF("Correct code; asking dispenser to release candy");
            } else {
                status_text = "WRONG CODE!";
                status_until_ms = k_uptime_get() + STATUS_SHOW_MS;
                clear_entry();
            }
        }
    } else if (key >= '0' && key <= '9' && entry_len < CODE_LEN) {
        entry[entry_len++] = key;
    }

    lcd_show_final();
}
#endif

int main(void)
{
    int rc;

    LOG_INF("Halloween station firmware: station %d, correct button %d",
            STATION_ID, CORRECT_BUTTON);

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

#if STATION_ID == LAST_STATION_ID
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

    static const char fota_name[] = "HW-Station" STRINGIFY(STATION_ID);

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

    if (STATION_ID == 1) {
        session_id = (uint16_t)sys_rand32_get();
        if (session_id == 0) {
            session_id = 1;
        }
        state = ST_ACTIVE;
        set_led(true);
        LOG_INF("Station 1 starts the game (session=%u)", session_id);
    } else {
        state = ST_WAITING;
        set_led(false);
        LOG_INF("Waiting for activation beacon for station %d", STATION_ID);
    }

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

        struct button_msg msg;
        while (k_msgq_get(&beacon_msgq, &msg, K_NO_WAIT) == 0) {
            process_beacon(&msg);
        }

        update_winning_pattern();

        if (state == ST_ACTIVE) {
            for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
                if (button_is_pressed(&buttons[i])) {
                    /* Simple debounce; wait for release to avoid repeat events. */
                    k_msleep(30);
                    if (button_is_pressed(&buttons[i])) {
                        handle_answer((unsigned int)i + 1);
                        while (button_is_pressed(&buttons[i])) {
                            k_msleep(10);
                        }
                        break;
                    }
                }
            }
        }

#if STATION_ID == LAST_STATION_ID
        if (state == ST_ENTER_CODE) {
            handle_key(keypad_scan_once());
        }
#endif

        k_msleep(10);
    }

    return 0;
}
