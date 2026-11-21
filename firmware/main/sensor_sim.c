/* Capteur simule : temperature + humidite realistes, differentes pour chaque carte. */
#include "sdkconfig.h"
#if CONFIG_APP_SENSOR_SIMULATED

#include <math.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sensor_driver.h"

const char *const sensor_drv_model = "SIMULE";
static float s_base_temp, s_base_hum;

esp_err_t sensor_drv_init(sensor_data_t *d)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_base_temp = 20.0f + (mac[5] % 8);
    s_base_hum = 45.0f + (mac[4] % 20);
    d->count = 2;
    d->v[0] = (sensor_value_t){ "temperature", "C", 0 };
    d->v[1] = (sensor_value_t){ "humidity", "%", 0 };
    ESP_LOGW("sensor_sim", "Capteur SIMULE (aucun materiel) : base %.0f C / %.0f %%", s_base_temp, s_base_hum);
    return ESP_OK;
}

esp_err_t sensor_drv_read(sensor_data_t *d)
{
    float t = esp_timer_get_time() / 1e6f;
    float noise = ((int)(esp_random() % 21) - 10) / 50.0f;   /* +/- 0.2 */
    d->v[0].value = roundf((s_base_temp + 1.5f * sinf(t / 60.0f) + noise) * 10) / 10;
    d->v[1].value = roundf((s_base_hum + 5.0f * cosf(t / 90.0f) + noise * 5) * 10) / 10;
    return ESP_OK;
}

#endif
