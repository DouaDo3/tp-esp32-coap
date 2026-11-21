/*
 * PIR (HC-SR501...) ou tout capteur numerique 0/1 : grandeur "motion".
 * Cablage : VCC->5V (HC-SR501) ou 3V3, GND->GND, OUT->APP_SENSOR_PIR_GPIO.
 * Chaque changement d'etat declenche une mesure immediate (interruption),
 * donc une notification Observe sans attendre la periode.
 */
#include "sdkconfig.h"
#if CONFIG_APP_SENSOR_PIR

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sensor_driver.h"

static const char *TAG = "pir";
const char *const sensor_drv_model = "PIR";
#define PIR_GPIO ((gpio_num_t)CONFIG_APP_SENSOR_PIR_GPIO)

static void IRAM_ATTR pir_isr(void *arg)
{
    sensor_manager_wake_from_isr();
}

esp_err_t sensor_drv_init(sensor_data_t *d)
{
    d->count = 1;
    d->v[0] = (sensor_value_t){ "motion", "", 0 };
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_APP_SENSOR_PIR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {    /* deja installe : OK */
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIR_GPIO, pir_isr, NULL), TAG, "isr");
    ESP_LOGI(TAG, "PIR sur GPIO %d (publication immediate a chaque changement)", CONFIG_APP_SENSOR_PIR_GPIO);
    return ESP_OK;
}

esp_err_t sensor_drv_read(sensor_data_t *d)
{
    d->v[0].value = gpio_get_level(PIR_GPIO) ? 1.0f : 0.0f;
    return ESP_OK;
}

#endif
