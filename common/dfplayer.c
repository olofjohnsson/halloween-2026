#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

#include "dfplayer.h"

LOG_MODULE_REGISTER(dfplayer, LOG_LEVEL_INF);

#define CMD_PLAY_TRACK 0x03
#define CMD_VOLUME     0x06
#define CMD_SOURCE     0x09
#define CMD_RESET      0x0C
#define SRC_SD         0x02

enum init_step {
    STEP_RESET,
    STEP_SOURCE,
    STEP_VOLUME,
    STEP_READY,
};

static const struct device *df_uart;
static uint8_t df_volume;
static enum init_step step;
static bool ready;
static uint16_t pending_track;
static struct k_work_delayable init_work;
static K_MUTEX_DEFINE(tx_lock);
RING_BUF_DECLARE(rx_rb, 64);

static void uart_isr(const struct device *dev, void *user_data)
{
    uint8_t buf[16];

    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        int n = uart_fifo_read(dev, buf, sizeof(buf));

        if (n <= 0) {
            break;
        }
        ring_buf_put(&rx_rb, buf, n);
    }
}

/* Logs frames the module sent back (0x3F init, 0x3D track finished, 0x40 error). */
static void log_rx(void)
{
    uint8_t c;
    uint8_t f[10];
    int n = 0;

    while (ring_buf_get(&rx_rb, &c, 1) == 1) {
        if (c == 0x7E) {
            n = 0;
        }
        if (n < sizeof(f)) {
            f[n++] = c;
        }
        if (c == 0xEF && n == sizeof(f)) {
            LOG_INF("RX cmd 0x%02X param %u", f[3], (f[5] << 8) | f[6]);
            n = 0;
        }
    }
}

static void send(uint8_t cmd, uint16_t param)
{
    uint8_t f[10] = {0x7E, 0xFF, 0x06, cmd, 0x00, param >> 8, param & 0xFF, 0, 0, 0xEF};
    uint16_t sum = 0;

    for (int i = 1; i <= 6; i++) {
        sum += f[i];
    }
    sum = -sum;
    f[7] = sum >> 8;
    f[8] = sum & 0xFF;

    k_mutex_lock(&tx_lock, K_FOREVER);
    for (int i = 0; i < sizeof(f); i++) {
        uart_poll_out(df_uart, f[i]);
    }
    k_mutex_unlock(&tx_lock);
    LOG_INF("TX cmd 0x%02X param %u", cmd, param);
}

static void init_handler(struct k_work *work)
{
    log_rx();

    switch (step) {
    case STEP_RESET:
        send(CMD_RESET, 0);
        step = STEP_SOURCE;
        k_work_reschedule(&init_work, K_MSEC(2000));
        break;
    case STEP_SOURCE:
        send(CMD_SOURCE, SRC_SD);
        step = STEP_VOLUME;
        k_work_reschedule(&init_work, K_MSEC(500));
        break;
    case STEP_VOLUME:
        send(CMD_VOLUME, df_volume);
        step = STEP_READY;
        k_work_reschedule(&init_work, K_MSEC(200));
        break;
    case STEP_READY:
        ready = true;
        if (pending_track != 0) {
            send(CMD_PLAY_TRACK, pending_track);
            pending_track = 0;
        }
        break;
    }
}

int dfplayer_init(const struct device *uart, uint8_t volume)
{
    if (!device_is_ready(uart)) {
        LOG_ERR("DFPlayer UART not ready");
        return -ENODEV;
    }

    df_uart = uart;
    df_volume = volume > 30 ? 30 : volume;
    step = STEP_RESET;

    uart_irq_callback_user_data_set(uart, uart_isr, NULL);
    uart_irq_rx_enable(uart);

    k_work_init_delayable(&init_work, init_handler);
    k_work_schedule(&init_work, K_NO_WAIT);
    return 0;
}

void dfplayer_play(uint16_t track)
{
    if (track == 0 || df_uart == NULL) {
        return;
    }

    if (!ready) {
        pending_track = track;
        return;
    }
    log_rx();
    send(CMD_PLAY_TRACK, track);
}
