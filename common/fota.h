#ifndef HALLOWEEN_FOTA_H_
#define HALLOWEEN_FOTA_H_

#include <stddef.h>
#include <zephyr/bluetooth/bluetooth.h>

/*
 * Shared FOTA support: one connectable advertising set that carries the
 * game beacon and, in the scan response, the MCUmgr SMP service UUID and a
 * short device name so nRF Connect Device Manager can find the board.
 * Advertising is restarted automatically after a phone disconnects.
 */

/* ad must stay valid for the lifetime of the program; name is at most 11 characters. */
int fota_adv_start(const struct bt_data *ad, size_t ad_len, const char *name);

/* Update the beacon. While a phone is connected the new data is used when advertising resumes. */
int fota_adv_update(const struct bt_data *ad, size_t ad_len);

/* Mark the running image as good so MCUboot does not revert it. */
void fota_confirm_image(void);

#endif
