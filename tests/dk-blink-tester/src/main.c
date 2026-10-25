#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#define BLINK_PIN 13 /* P0.13 = LED1 on the nRF52840 DK (the LED lights when the pin is low) */

int main(void)
{
    const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));

    if (!device_is_ready(gpio) || gpio_pin_configure(gpio, BLINK_PIN, GPIO_OUTPUT_HIGH) < 0) {
        return 0;
    }

    while (1) {
        (void)gpio_pin_toggle(gpio, BLINK_PIN);
        k_sleep(K_SECONDS(1));
    }

    return 0;
}
