#include "led_manager.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "led";

#define LED_GPIO  ((gpio_num_t)CONFIG_APP_LED_GPIO)
#ifdef CONFIG_APP_LED_ACTIVE_LOW
#define LED_ON  0
#define LED_OFF 1
#else
#define LED_ON  1
#define LED_OFF 0
#endif

static esp_timer_handle_t s_off_timer;

static void led_off_cb(void *arg)
{
    gpio_set_level(LED_GPIO, LED_OFF);
}

esp_err_t led_manager_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_APP_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    gpio_set_level(LED_GPIO, LED_OFF);

    const esp_timer_create_args_t args = { .callback = led_off_cb, .name = "led_off" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_off_timer), TAG, "timer");
    ESP_LOGI(TAG, "LED sur GPIO %d (%d ms par reception)", CONFIG_APP_LED_GPIO, CONFIG_APP_LED_ON_MS);
    return ESP_OK;
}

void led_manager_pulse(void)
{
    gpio_set_level(LED_GPIO, LED_ON);
    esp_timer_stop(s_off_timer);   /* ESP_ERR_INVALID_STATE si deja arrete : sans importance */
    esp_timer_start_once(s_off_timer, (uint64_t)CONFIG_APP_LED_ON_MS * 1000);
}
