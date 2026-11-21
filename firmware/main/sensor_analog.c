/*
 * Capteur analogique (MQ135, LDR, potentiometre...) sur l'ADC1.
 * Grandeurs : <APP_SENSOR_ANALOG_NAME> en % de la pleine echelle, et "raw" (0-4095).
 * Cablage : sortie analogique (AO) -> APP_SENSOR_ANALOG_GPIO (32..39 sur ESP32).
 * ATTENTION : la tension sur le GPIO ne doit pas depasser 3,3 V (un MQ135 alimente
 * en 5 V peut sortir jusqu'a 5 V : utiliser un pont diviseur).
 */
#include "sdkconfig.h"
#if CONFIG_APP_SENSOR_ANALOG

#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sensor_driver.h"

static const char *TAG = "analog";
const char *const sensor_drv_model = "ANALOG";

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_chan;

esp_err_t sensor_drv_init(sensor_data_t *d)
{
    d->count = 2;
    d->v[0] = (sensor_value_t){ CONFIG_APP_SENSOR_ANALOG_NAME, "%", 0 };
    d->v[1] = (sensor_value_t){ "raw", "", 0 };

    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(CONFIG_APP_SENSOR_ANALOG_GPIO, &unit, &s_chan),
                        TAG, "GPIO %d n'est pas une entree ADC", CONFIG_APP_SENSOR_ANALOG_GPIO);
    if (unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO %d est sur l'ADC2, inutilisable avec le Wi-Fi : choisir 32..39",
                 CONFIG_APP_SENSOR_ANALOG_GPIO);
        return ESP_ERR_INVALID_ARG;
    }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&ucfg, &s_adc), TAG, "adc unit");
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &ccfg), TAG, "adc chan");
    ESP_LOGI(TAG, "Entree analogique \"%s\" sur GPIO %d", CONFIG_APP_SENSOR_ANALOG_NAME,
             CONFIG_APP_SENSOR_ANALOG_GPIO);
    return ESP_OK;
}

esp_err_t sensor_drv_read(sensor_data_t *d)
{
    if (!s_adc) {
        return ESP_ERR_INVALID_STATE;
    }
    int sum = 0;
    for (int i = 0; i < 16; i++) {      /* moyenne pour lisser le bruit */
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, s_chan, &raw), TAG, "lecture");
        sum += raw;
    }
    float raw = sum / 16.0f;
    d->v[0].value = raw * 100.0f / 4095.0f;
    d->v[1].value = raw;
    return ESP_OK;
}

#endif
