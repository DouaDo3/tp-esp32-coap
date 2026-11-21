/*
 * DHT11 / DHT22 (AM2302) : temperature (C) + humidite (%).
 * Cablage : VCC->3V3, GND->GND, DATA->APP_SENSOR_DHT_GPIO
 * (resistance de tirage 10 kOhm DATA->3V3 si le module n'en a pas ; le pull-up
 * interne de l'ESP32 est aussi active).
 * Protocole 1 fil : impulsion de depart, puis 40 bits codes par la duree du niveau haut.
 */
#include "sdkconfig.h"
#if CONFIG_APP_SENSOR_DHT11 || CONFIG_APP_SENSOR_DHT22

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_driver.h"

static const char *TAG = "dht";
#define DHT_GPIO ((gpio_num_t)CONFIG_APP_SENSOR_DHT_GPIO)

#if CONFIG_APP_SENSOR_DHT11
const char *const sensor_drv_model = "DHT11";
#else
const char *const sensor_drv_model = "DHT22";
#endif

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Attend que la ligne quitte 'level' ; retourne la duree en us, -1 si timeout. */
static int wait_while(int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(DHT_GPIO) == level) {
        if (esp_timer_get_time() - start > timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - start);
}

esp_err_t sensor_drv_init(sensor_data_t *d)
{
    d->count = 2;
    d->v[0] = (sensor_value_t){ "temperature", "C", 0 };
    d->v[1] = (sensor_value_t){ "humidity", "%", 0 };
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_APP_SENSOR_DHT_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    gpio_set_level(DHT_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));    /* le capteur a besoin de ~1 s apres la mise sous tension */
    ESP_LOGI(TAG, "%s sur GPIO %d", sensor_drv_model, CONFIG_APP_SENSOR_DHT_GPIO);
    return ESP_OK;
}

static esp_err_t read_once(uint8_t b[5])
{
    /* Impulsion de depart : niveau bas >= 18 ms (DHT11) ou >= 1 ms (DHT22) */
    gpio_set_level(DHT_GPIO, 0);
#if CONFIG_APP_SENSOR_DHT11
    vTaskDelay(pdMS_TO_TICKS(20));
#else
    esp_rom_delay_us(1200);
#endif
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&s_mux);     /* section critique de ~5 ms : timing a la microseconde */
    gpio_set_level(DHT_GPIO, 1);
    esp_rom_delay_us(30);
    if (wait_while(1, 100) < 0 || wait_while(0, 100) < 0 || wait_while(1, 100) < 0) {
        err = ESP_ERR_TIMEOUT;      /* pas de reponse du capteur */
    }
    for (int i = 0; i < 40 && err == ESP_OK; i++) {
        if (wait_while(0, 80) < 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        int high = wait_while(1, 100);
        if (high < 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        b[i / 8] = (uint8_t)(b[i / 8] << 1 | (high > 40 ? 1 : 0));   /* ~27 us = 0, ~70 us = 1 */
    }
    portEXIT_CRITICAL(&s_mux);
    gpio_set_level(DHT_GPIO, 1);
    if (err == ESP_OK && (uint8_t)(b[0] + b[1] + b[2] + b[3]) != b[4]) {
        err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

esp_err_t sensor_drv_read(sensor_data_t *d)
{
    uint8_t b[5] = { 0 };
    esp_err_t err = read_once(b);
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2100));    /* une seconde tentative apres le delai minimal */
        b[0] = b[1] = b[2] = b[3] = b[4] = 0;
        err = read_once(b);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Lecture %s echouee : %s (cablage ? GPIO %d ?)", sensor_drv_model,
                 esp_err_to_name(err), CONFIG_APP_SENSOR_DHT_GPIO);
        return err;
    }
#if CONFIG_APP_SENSOR_DHT11
    float hum = b[0] + b[1] / 10.0f;
    float temp = (b[2] & 0x7F) + b[3] / 10.0f;
    if (b[3] & 0x80) {
        temp = -temp;
    }
#else
    float hum = ((b[0] << 8) | b[1]) / 10.0f;
    float temp = (((b[2] & 0x7F) << 8) | b[3]) / 10.0f;
    if (b[2] & 0x80) {
        temp = -temp;
    }
#endif
    d->v[0].value = temp;
    d->v[1].value = hum;
    return ESP_OK;
}

#endif
