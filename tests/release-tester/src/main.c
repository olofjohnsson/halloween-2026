/* Pretends to be station 5: any button sends MSG_RELEASE_CANDY with a new session
 * and advertises it until the dispenser answers with MSG_CANDY_DONE. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "fota.h"

LOG_MODULE_REGISTER(tester, LOG_LEVEL_INF);

/* Protocol shared with the stations (see ../stations/src/main.c). */
#define HALLOWEEN_SERVICE_UUID 0xFFF0
#define PROTOCOL_VERSION 1
#define MAGIC_0 0x48
#define MAGIC_1 0x57
#define MSG_RELEASE_CANDY 4
#define MSG_CANDY_DONE 7
#define LAST_STATION_ID 5
#define DISPENSER_ID 0

#define SERVICE_DATA_LEN 13
#define CONFIRM_TIMEOUT_S 30

static const struct gpio_dt_spec buttons[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
#if DT_NODE_EXISTS(DT_ALIAS(sw1))
    GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw2))
    GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios),
#endif
};
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct gpio_callback button_cb[ARRAY_SIZE(buttons)];

K_SEM_DEFINE(press_sem, 0, 1);
K_SEM_DEFINE(done_sem, 0, 1);

static volatile uint16_t pending_session;
static uint8_t service_data[SERVICE_DATA_LEN];
static const uint8_t adv_flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
static const struct bt_data ad[] = {
    BT_DATA(BT_DATA_FLAGS, &adv_flags, sizeof(adv_flags)),
    BT_DATA(BT_DATA_SVC_DATA16, service_data, sizeof(service_data)),
};

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
                           uint32_t pins)
{
    k_sem_give(&press_sem);
}

static bool parse_service_data(struct bt_data *data, void *user_data)
{
    ARG_UNUSED(user_data);

    if (data->type != BT_DATA_SVC_DATA16 || data->data_len < SERVICE_DATA_LEN) {
        return true;
    }

    const uint8_t *d = data->data;

    if (d[0] != (uint8_t)(HALLOWEEN_SERVICE_UUID & 0xff) ||
        d[1] != (uint8_t)(HALLOWEEN_SERVICE_UUID >> 8) ||
        d[2] != MAGIC_0 || d[3] != MAGIC_1 || d[4] != PROTOCOL_VERSION ||
        d[5] != MSG_CANDY_DONE || d[6] != LAST_STATION_ID || d[7] != DISPENSER_ID) {
        return true;
    }

    uint16_t session = (uint16_t)d[8] | ((uint16_t)d[9] << 8);

    if (session == pending_session) {
        k_sem_give(&done_sem);
    }
    return false;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                         struct net_buf_simple *ad_buf)
{
    ARG_UNUSED(addr);
    ARG_UNUSED(rssi);
    ARG_UNUSED(adv_type);

    if (pending_session != 0) {
        bt_data_parse(ad_buf, parse_service_data, NULL);
    }
}

static void fill_request(uint16_t session)
{
    service_data[0] = (uint8_t)(HALLOWEEN_SERVICE_UUID & 0xff);
    service_data[1] = (uint8_t)(HALLOWEEN_SERVICE_UUID >> 8);
    service_data[2] = MAGIC_0;
    service_data[3] = MAGIC_1;
    service_data[4] = PROTOCOL_VERSION;
    service_data[5] = MSG_RELEASE_CANDY;
    service_data[6] = DISPENSER_ID;
    service_data[7] = LAST_STATION_ID;
    service_data[8] = (uint8_t)(session & 0xff);
    service_data[9] = (uint8_t)(session >> 8);
    service_data[10] = 0;
    service_data[11] = 0;
    service_data[12] = 0;
}

static uint16_t new_session(void)
{
    uint16_t session;

    do {
        session = (uint16_t)sys_rand32_get();
    } while (session == 0 || session == pending_session);
    return session;
}

int main(void)
{
    int rc;

    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("LED GPIO not ready");
        return 0;
    }
    (void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
        if (!gpio_is_ready_dt(&buttons[i])) {
            LOG_ERR("Button %u GPIO not ready", (unsigned int)i + 1);
            return 0;
        }
        (void)gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);
        gpio_init_callback(&button_cb[i], button_pressed, BIT(buttons[i].pin));
        (void)gpio_add_callback(buttons[i].port, &button_cb[i]);
        (void)gpio_pin_interrupt_configure_dt(&buttons[i], GPIO_INT_EDGE_TO_ACTIVE);
    }

    rc = bt_enable(NULL);
    if (rc < 0) {
        LOG_ERR("Bluetooth init failed: %d", rc);
        return 0;
    }

    /* Advertise an all-zero (ignored) beacon continuously so the board stays reachable for FOTA. */
    rc = fota_adv_start(ad, ARRAY_SIZE(ad), "RelTester");
    if (rc < 0) {
        LOG_ERR("BLE advertising start failed: %d", rc);
        return 0;
    }
    fota_confirm_image();

    rc = bt_le_scan_start(BT_LE_SCAN_PASSIVE, device_found);
    if (rc < 0) {
        LOG_ERR("BLE scan start failed: %d", rc);
        return 0;
    }

    LOG_INF("Release tester ready: press any button to request a candy");

    while (1) {
        k_sem_take(&press_sem, K_FOREVER);

        uint16_t session = new_session();

        fill_request(session);
        k_sem_reset(&done_sem);
        pending_session = session;

        rc = fota_adv_update(ad, ARRAY_SIZE(ad));
        if (rc < 0) {
            LOG_ERR("Advertising start failed: %d", rc);
            pending_session = 0;
            continue;
        }

        (void)gpio_pin_set_dt(&led, 1);
        LOG_INF("Requesting release (session=%u)", session);

        if (k_sem_take(&done_sem, K_SECONDS(CONFIRM_TIMEOUT_S)) == 0) {
            LOG_INF("Dispenser confirmed release (session=%u)", session);
        } else {
            LOG_WRN("No confirmation from dispenser within %d s", CONFIRM_TIMEOUT_S);
        }

        memset(service_data, 0, sizeof(service_data));
        (void)fota_adv_update(ad, ARRAY_SIZE(ad));
        (void)gpio_pin_set_dt(&led, 0);
        pending_session = 0;
        k_sem_reset(&press_sem);
    }
    return 0;
}
