#include "actuator_manager.h"

#include <string.h>
#include <strings.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "actuator";

#define ACT_GPIO ((gpio_num_t)CONFIG_APP_ACTUATOR_GPIO)
#ifdef CONFIG_APP_ACTUATOR_ACTIVE_LOW
#define ACT_LEVEL(on) ((on) ? 0 : 1)
#else
#define ACT_LEVEL(on) ((on) ? 1 : 0)
#endif

static volatile bool s_on;

static void apply(bool on)
{
    s_on = on;
    gpio_set_level(ACT_GPIO, ACT_LEVEL(on));
    ESP_LOGI(TAG, "[ACTIONNEUR] %s -> %s (GPIO %d)", CONFIG_APP_ACTUATOR_NAME, on ? "ON" : "OFF",
             CONFIG_APP_ACTUATOR_GPIO);
}

esp_err_t actuator_manager_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_APP_ACTUATOR_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    apply(false);
    return ESP_OK;
}

bool actuator_manager_command(const char *cmd)
{
    if (strcasecmp(cmd, "on") == 0 || strcmp(cmd, "1") == 0) {
        apply(true);
    } else if (strcasecmp(cmd, "off") == 0 || strcmp(cmd, "0") == 0) {
        apply(false);
    } else if (strcasecmp(cmd, "toggle") == 0) {
        apply(!s_on);
    } else {
        ESP_LOGW(TAG, "[ACTIONNEUR] commande inconnue : \"%s\" (attendu on / off / toggle)", cmd);
        return false;
    }
    return true;
}

bool actuator_manager_get(void)
{
    return s_on;
}

const char *actuator_manager_name(void)
{
    return CONFIG_APP_ACTUATOR_NAME;
}
