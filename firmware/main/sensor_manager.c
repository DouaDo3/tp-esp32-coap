#include "sensor_manager.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_driver.h"

static const char *TAG = "sensor";

static sensor_data_t s_last;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static sensor_cb_t s_cb;
static TaskHandle_t s_task;

static void do_measure(void)
{
    sensor_data_t d;
    sensor_manager_get(&d);             /* repart des dernieres valeurs valides */
    d.ok = (sensor_drv_read(&d) == ESP_OK);
    for (int i = 0; i < d.count; i++) {
        d.v[i].value = roundf(d.v[i].value * 100) / 100;
    }
    d.seq++;

    portENTER_CRITICAL(&s_mux);
    s_last = d;
    portEXIT_CRITICAL(&s_mux);

    char line[128];
    int n = snprintf(line, sizeof(line), "Mesure #%" PRIu32 " (%s)%s :", d.seq, d.model,
                     d.ok ? "" : " LECTURE ECHOUEE, dernieres valeurs");
    for (int i = 0; i < d.count && n < (int)sizeof(line); i++) {
        n += snprintf(line + n, sizeof(line) - n, " %s=%g%s", d.v[i].name, d.v[i].value, d.v[i].unit);
    }
    if (d.ok) {
        ESP_LOGI(TAG, "%s", line);
    } else {
        ESP_LOGW(TAG, "%s", line);
    }
    if (s_cb) {
        s_cb(&d);
    }
}

static void sensor_task(void *arg)
{
    for (;;) {
        do_measure();
        /* Reveil a la periode, ou plus tot si un capteur evenementiel (PIR) le demande */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_APP_SENSOR_PERIOD_S * 1000));
    }
}

void sensor_manager_wake_from_isr(void)
{
    if (s_task) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &hp);
        portYIELD_FROM_ISR(hp);
    }
}

esp_err_t sensor_manager_init(void)
{
    sensor_data_t d = { 0 };
    d.model = sensor_drv_model;
    esp_err_t err = sensor_drv_init(&d);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initialisation du capteur %s echouee (%s) : verifier le cablage et les GPIO",
                 sensor_drv_model, esp_err_to_name(err));
        /* On continue quand meme : la carte reste utilisable pour recevoir des donnees */
    }
    d.ok = (err == ESP_OK) && (sensor_drv_read(&d) == ESP_OK);
    portENTER_CRITICAL(&s_mux);
    s_last = d;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Capteur %s : %d grandeur(s) publiee(s)", d.model, d.count);
    return ESP_OK;
}

esp_err_t sensor_manager_start(sensor_cb_t cb)
{
    s_cb = cb;
    return xTaskCreate(sensor_task, "sensor", 4096, NULL, 3, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void sensor_manager_get(sensor_data_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_last;
    portEXIT_CRITICAL(&s_mux);
}

bool sensor_data_find(const sensor_data_t *d, const char *name, float *value, const char **unit)
{
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->v[i].name, name) == 0) {
            if (value) {
                *value = d->v[i].value;
            }
            if (unit) {
                *unit = d->v[i].unit;
            }
            return true;
        }
    }
    return false;
}

int sensor_data_to_json(const sensor_data_t *d, const char *device_id, char *buf, size_t len)
{
    int n = snprintf(buf, len, "{\"device_id\":\"%s\",\"sensor\":\"%s\",\"seq\":%" PRIu32 ",\"ok\":%s",
                     device_id, d->model, d->seq, d->ok ? "true" : "false");
    for (int i = 0; i < d->count && n < (int)len; i++) {
        if (isfinite(d->v[i].value)) {
            n += snprintf(buf + n, len - n, ",\"%s\":%g", d->v[i].name, d->v[i].value);
        } else {
            n += snprintf(buf + n, len - n, ",\"%s\":null", d->v[i].name);
        }
    }
    if (n < (int)len) {
        n += snprintf(buf + n, len - n, ",\"units\":{");
    }
    for (int i = 0; i < d->count && n < (int)len; i++) {
        n += snprintf(buf + n, len - n, "%s\"%s\":\"%s\"", i ? "," : "", d->v[i].name, d->v[i].unit);
    }
    if (n < (int)len) {
        n += snprintf(buf + n, len - n, "}}");
    }
    return n;
}
