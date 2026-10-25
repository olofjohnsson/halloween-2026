/* Pretends to be station 5 and sends MSG_PLAY_TRACK to music player 1.
 * Button 1: next track, Button 2: previous track (wraps within 1..LAST_TRACK). */
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tester, LOG_LEVEL_INF);

/* Protocol shared with the stations (see ../stations/src/main.c). */
#define HALLOWEEN_SERVICE_UUID 0xFFF0
#define PROTOCOL_VERSION 1
#define MAGIC_0 0x48
#define MAGIC_1 0x57
#define MSG_PLAY_TRACK 8
#define MUSIC_PLAYER_TARGET_BASE 10
#define SOURCE_STATION_ID 5

#define PLAYER_ID 1
#define LAST_TRACK 3
#define BEACON_MS 3000 /* the player plays each sequence number once */

#define SERVICE_DATA_LEN 13

static const struct gpio_dt_spec next_btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec prev_btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct gpio_callback next_cb;
static struct gpio_callback prev_cb;

K_SEM_DEFINE(press_sem, 0, 1);
static volatile int press_dir;

static uint8_t service_data[SERVICE_DATA_LEN];
static const uint8_t adv_flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
static const struct bt_data ad[] = {
    BT_DATA(BT_DATA_FLAGS, &adv_flags, sizeof(adv_flags)),
    BT_DATA(BT_DATA_SVC_DATA16, service_data, sizeof(service_data)),
};

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
                           uint32_t pins)
{
    press_dir = (cb == &next_cb) ? 1 : -1;
    k_sem_give(&press_sem);
}

static void fill_request(uint16_t track, uint8_t seq)
{
    service_data[0] = (uint8_t)(HALLOWEEN_SERVICE_UUID & 0xff);
    service_data[1] = (uint8_t)(HALLOWEEN_SERVICE_UUID >> 8);
    service_data[2] = MAGIC_0;
    service_data[3] = MAGIC_1;
    service_data[4] = PROTOCOL_VERSION;
    service_data[5] = MSG_PLAY_TRACK;
    service_data[6] = MUSIC_PLAYER_TARGET_BASE + PLAYER_ID;
    service_data[7] = SOURCE_STATION_ID;
    service_data[8] = 0;
    service_data[9] = 0;
    service_data[10] = (uint8_t)(track & 0xff);
    service_data[11] = (uint8_t)(track >> 8);
    service_data[12] = seq;
}

static int setup_button(const struct gpio_dt_spec *btn, struct gpio_callback *cb)
{
    if (!gpio_is_ready_dt(btn)) {
        return -ENODEV;
    }
    (void)gpio_pin_configure_dt(btn, GPIO_INPUT);
    gpio_init_callback(cb, button_pressed, BIT(btn->pin));
    (void)gpio_add_callback(btn->port, cb);
    return gpio_pin_interrupt_configure_dt(btn, GPIO_INT_EDGE_TO_ACTIVE);
}

int main(void)
{
    uint16_t track = 0;
    uint8_t seq = 0;
    int rc;

    if (!gpio_is_ready_dt(&led) || setup_button(&next_btn, &next_cb) < 0 ||
        setup_button(&prev_btn, &prev_cb) < 0) {
        LOG_ERR("Button/LED GPIO not ready");
        return 0;
    }
    (void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    rc = bt_enable(NULL);
    if (rc < 0) {
        LOG_ERR("Bluetooth init failed: %d", rc);
        return 0;
    }

    LOG_INF("Player tester ready: Button 1 = next track, Button 2 = previous (player %d)",
            PLAYER_ID);

    while (1) {
        k_sem_take(&press_sem, K_FOREVER);

        if (press_dir > 0) {
            track = (track >= LAST_TRACK || track == 0) ? 1 : track + 1;
        } else {
            track = (track <= 1) ? LAST_TRACK : track - 1;
        }
        seq = (seq == 255) ? 1 : seq + 1;
        fill_request(track, seq);

        rc = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
        if (rc < 0) {
            LOG_ERR("Advertising start failed: %d", rc);
            continue;
        }

        (void)gpio_pin_set_dt(&led, 1);
        LOG_INF("Sent play track %u to player %d (seq=%u)", track, PLAYER_ID, seq);
        k_msleep(BEACON_MS);
        (void)bt_le_adv_stop();
        (void)gpio_pin_set_dt(&led, 0);
        k_sem_reset(&press_sem);
    }
    return 0;
}
