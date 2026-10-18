#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>

#include "fota.h"

LOG_MODULE_REGISTER(fota, LOG_LEVEL_INF);

/* Scan response: 18 byte UUID + 2 byte header + name must fit in 31 bytes. */
#define FOTA_NAME_MAX 11

static const uint8_t smp_uuid[] = { SMP_BT_SVC_UUID_VAL };

static const struct bt_data *beacon_ad;
static size_t beacon_ad_len;
static struct bt_data scan_rsp[2];
static bool connected;

static int adv_restart(void)
{
    int rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, beacon_ad, beacon_ad_len,
                             scan_rsp, ARRAY_SIZE(scan_rsp));

    if (rc == -EALREADY) {
        return 0;
    }
    if (rc < 0) {
        LOG_ERR("BLE advertising start failed: %d", rc);
    }
    return rc;
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    ARG_UNUSED(conn);

    if (err) {
        LOG_ERR("Connection failed: 0x%02x", err);
        return;
    }
    connected = true;
    LOG_INF("Phone connected for FOTA; game beacon paused");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);

    connected = false;
    LOG_INF("Phone disconnected (reason 0x%02x)", reason);
}

/* The connection object is only free again in recycled(), so restart advertising there. */
static void on_recycled(void)
{
    adv_restart();
}

BT_CONN_CB_DEFINE(fota_conn_cb) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .recycled = on_recycled,
};

int fota_adv_start(const struct bt_data *ad, size_t ad_len, const char *name)
{
    size_t name_len = strlen(name);

    if (name_len > FOTA_NAME_MAX) {
        return -EINVAL;
    }

    beacon_ad = ad;
    beacon_ad_len = ad_len;

    scan_rsp[0].type = BT_DATA_UUID128_ALL;
    scan_rsp[0].data_len = sizeof(smp_uuid);
    scan_rsp[0].data = smp_uuid;
    scan_rsp[1].type = BT_DATA_NAME_COMPLETE;
    scan_rsp[1].data_len = name_len;
    scan_rsp[1].data = (const uint8_t *)name;

    return adv_restart();
}

int fota_adv_update(const struct bt_data *ad, size_t ad_len)
{
    if (connected) {
        return 0;
    }
    return bt_le_adv_update_data(ad, ad_len, scan_rsp, ARRAY_SIZE(scan_rsp));
}

void fota_confirm_image(void)
{
    if (boot_is_img_confirmed()) {
        return;
    }

    int rc = boot_write_img_confirmed();

    if (rc < 0) {
        LOG_ERR("Confirming new image failed: %d", rc);
    } else {
        LOG_INF("New firmware image confirmed");
    }
}
