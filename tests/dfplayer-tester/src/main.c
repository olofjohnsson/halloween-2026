#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dfplayer_tester, LOG_LEVEL_INF);

#define DF_UART DEVICE_DT_GET(DT_NODELABEL(dfplayer_uart))

#define CMD_NEXT        0x01
#define CMD_PLAY_TRACK  0x03
#define CMD_VOLUME      0x06
#define CMD_SOURCE      0x09
#define CMD_QUERY_VOL   0x43
#define CMD_RESET       0x0C
#define CMD_PAUSE       0x0E
#define SRC_SD          0x02

#define VOLUME          15   /* 0..30 */
#define TRACK_COUNT     3    /* files 0001.mp3 .. 000N.mp3 in /MP3 or the SD root */
#define TRACK_TIME_MS   5000

static const struct device *const uart = DF_UART;
static const struct gpio_dt_spec btn1 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec btn2 = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);

RING_BUF_DECLARE(rx_rb, 128);

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

static void df_send(uint8_t cmd, uint16_t param)
{
    uint8_t f[10] = {0x7E, 0xFF, 0x06, cmd, 0x00, param >> 8, param & 0xFF, 0, 0, 0xEF};
    uint16_t sum = 0;

    for (int i = 1; i <= 6; i++) {
        sum += f[i];
    }
    sum = -sum;
    f[7] = sum >> 8;
    f[8] = sum & 0xFF;

    for (int i = 0; i < sizeof(f); i++) {
        uart_poll_out(uart, f[i]);
    }
    LOG_INF("TX cmd 0x%02X param %u", cmd, param);
}

/* Drain and log anything the module sent back (e.g. 0x3F init, 0x3D track finished). */
static void df_log_rx(void)
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

static void wait_ms(int ms)
{
    for (int t = 0; t < ms; t += 20) {
        df_log_rx();
        k_msleep(20);
    }
}

int main(void)
{
    if (!device_is_ready(uart) || !gpio_is_ready_dt(&btn1) || !gpio_is_ready_dt(&btn2)) {
        LOG_ERR("Device not ready");
        return -1;
    }
    gpio_pin_configure_dt(&btn1, GPIO_INPUT);
    gpio_pin_configure_dt(&btn2, GPIO_INPUT);

    uart_irq_callback_user_data_set(uart, uart_isr, NULL);
    uart_irq_rx_enable(uart);

    LOG_INF("DFPlayer Mini tester: Button 1 = next track, Button 2 = pause");

    df_send(CMD_RESET, 0);
    wait_ms(2000);
    df_send(CMD_SOURCE, SRC_SD);
    wait_ms(500);
    df_send(CMD_VOLUME, VOLUME);
    wait_ms(200);
    df_send(CMD_QUERY_VOL, 0); /* module must answer 0x43 with the volume if RX works */
    wait_ms(200);

    int track = 1;
    int64_t started = 0;

    df_send(CMD_PLAY_TRACK, track);
    started = k_uptime_get();

    while (1) {
        bool next = gpio_pin_get_dt(&btn1) > 0;

        if (gpio_pin_get_dt(&btn2) > 0) {
            df_send(CMD_PAUSE, 0);
            while (gpio_pin_get_dt(&btn2) > 0) {
                k_msleep(20);
            }
        }
        if (next || k_uptime_get() - started >= TRACK_TIME_MS) {
            track = track % TRACK_COUNT + 1;
            LOG_INF("Playing track %d", track);
            df_send(CMD_PLAY_TRACK, track);
            started = k_uptime_get();
            while (gpio_pin_get_dt(&btn1) > 0) {
                k_msleep(20);
            }
        }
        df_log_rx();
        k_msleep(20);
    }
}
