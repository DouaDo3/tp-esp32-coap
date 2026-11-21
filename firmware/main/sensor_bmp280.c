/*
 * BMP280 (ou BME280) en I2C : temperature (C) + pression (hPa).
 * Cablage : VCC->3V3, GND->GND, SDA->APP_SENSOR_I2C_SDA, SCL->APP_SENSOR_I2C_SCL.
 * Adresse detectee automatiquement : 0x76 (SDO a GND) ou 0x77 (SDO a VCC).
 * Formules de compensation : datasheet Bosch BMP280, section 8.1 (version flottante).
 */
#include "sdkconfig.h"
#if CONFIG_APP_SENSOR_BMP280

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sensor_driver.h"

static const char *TAG = "bmp280";
const char *const sensor_drv_model = "BMP280";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint16_t T1, P1;
static int16_t T2, T3, P2, P3, P4, P5, P6, P7, P8, P9;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100);
}

esp_err_t sensor_drv_init(sensor_data_t *d)
{
    d->count = 2;
    d->v[0] = (sensor_value_t){ "temperature", "C", 0 };
    d->v[1] = (sensor_value_t){ "pressure", "hPa", 0 };

    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_APP_SENSOR_I2C_SDA,
        .scl_io_num = CONFIG_APP_SENSOR_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_bus), TAG, "bus I2C");

    uint16_t addr = 0;
    if (i2c_master_probe(s_bus, 0x76, 100) == ESP_OK) {
        addr = 0x76;
    } else if (i2c_master_probe(s_bus, 0x77, 100) == ESP_OK) {
        addr = 0x77;
    } else {
        ESP_LOGE(TAG, "Aucun BMP280 en 0x76/0x77 (SDA=%d SCL=%d)", CONFIG_APP_SENSOR_I2C_SDA, CONFIG_APP_SENSOR_I2C_SCL);
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev, &s_dev), TAG, "device");

    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(rd(0xD0, &id, 1), TAG, "chip id");
    if (id != 0x58 && id != 0x60) {
        ESP_LOGE(TAG, "Identifiant 0x%02x inattendu (0x58 = BMP280, 0x60 = BME280)", id);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t c[24];
    ESP_RETURN_ON_ERROR(rd(0x88, c, sizeof(c)), TAG, "calibration");
    T1 = (uint16_t)(c[1] << 8 | c[0]);
    T2 = (int16_t)(c[3] << 8 | c[2]);
    T3 = (int16_t)(c[5] << 8 | c[4]);
    P1 = (uint16_t)(c[7] << 8 | c[6]);
    P2 = (int16_t)(c[9] << 8 | c[8]);
    P3 = (int16_t)(c[11] << 8 | c[10]);
    P4 = (int16_t)(c[13] << 8 | c[12]);
    P5 = (int16_t)(c[15] << 8 | c[14]);
    P6 = (int16_t)(c[17] << 8 | c[16]);
    P7 = (int16_t)(c[19] << 8 | c[18]);
    P8 = (int16_t)(c[21] << 8 | c[20]);
    P9 = (int16_t)(c[23] << 8 | c[22]);

    ESP_RETURN_ON_ERROR(wr(0xF5, 0xA0), TAG, "config");     /* standby 1 s, pas de filtre */
    ESP_RETURN_ON_ERROR(wr(0xF4, 0x27), TAG, "ctrl_meas");  /* T x1, P x1, mode normal */
    ESP_LOGI(TAG, "BMP280 detecte a l'adresse 0x%02x (id 0x%02x)", addr, id);
    return ESP_OK;
}

esp_err_t sensor_drv_read(sensor_data_t *d)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t r[6];
    ESP_RETURN_ON_ERROR(rd(0xF7, r, sizeof(r)), TAG, "lecture");
    int32_t adc_p = (int32_t)r[0] << 12 | (int32_t)r[1] << 4 | r[2] >> 4;
    int32_t adc_t = (int32_t)r[3] << 12 | (int32_t)r[4] << 4 | r[5] >> 4;
    if (adc_t == 0x80000) {
        return ESP_ERR_INVALID_RESPONSE;    /* mesure pas encore disponible */
    }

    double v1 = (adc_t / 16384.0 - T1 / 1024.0) * T2;
    double v2 = (adc_t / 131072.0 - T1 / 8192.0) * (adc_t / 131072.0 - T1 / 8192.0) * T3;
    double t_fine = v1 + v2;
    d->v[0].value = (float)(t_fine / 5120.0);

    v1 = t_fine / 2.0 - 64000.0;
    v2 = v1 * v1 * P6 / 32768.0;
    v2 = v2 + v1 * P5 * 2.0;
    v2 = v2 / 4.0 + P4 * 65536.0;
    v1 = (P3 * v1 * v1 / 524288.0 + P2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * P1;
    if (v1 == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    double p = 1048576.0 - adc_p;
    p = (p - v2 / 4096.0) * 6250.0 / v1;
    v1 = P9 * p * p / 2147483648.0;
    v2 = p * P8 / 32768.0;
    p = p + (v1 + v2 + P7) / 16.0;
    d->v[1].value = (float)(p / 100.0);
    return ESP_OK;
}

#endif
