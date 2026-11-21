#pragma once
/*
 * sensor_manager : acquisition periodique des mesures, independante du capteur.
 *
 *  Le capteur est choisi dans menuconfig (TP CoAPS -> Capteur) :
 *   simule | BMP280 | DHT11 | DHT22 | PIR | analogique (MQ135, LDR...)
 *  Chaque capteur fournit 1 a SENSOR_MAX_VALUES grandeurs nommees, par exemple
 *  {temperature, humidity} ou {motion}. Le serveur CoAP cree automatiquement
 *  une ressource /sensor/<nom> par grandeur + /sensor/data (toutes les valeurs).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SENSOR_MAX_VALUES 4

typedef struct {
    const char *name;   /* ex. "temperature" (chaine statique) */
    const char *unit;   /* ex. "C" */
    float value;
} sensor_value_t;

typedef struct {
    const char *model;  /* ex. "DHT11" */
    uint32_t seq;       /* numero de mesure */
    bool ok;            /* false si la derniere lecture a echoue (valeurs = dernieres valides) */
    uint8_t count;
    sensor_value_t v[SENSOR_MAX_VALUES];
} sensor_data_t;

/* Appele (depuis la tache capteur) a chaque nouvelle mesure. */
typedef void (*sensor_cb_t)(const sensor_data_t *data);

/* Initialise le capteur et fait une premiere lecture (avant le serveur CoAP). */
esp_err_t sensor_manager_init(void);

/* Lance la tache de mesure periodique. */
esp_err_t sensor_manager_start(sensor_cb_t cb);

/* Derniere mesure (copie thread-safe). */
void sensor_manager_get(sensor_data_t *out);

/* Cherche une grandeur par nom ; retourne false si absente. */
bool sensor_data_find(const sensor_data_t *d, const char *name, float *value, const char **unit);

/* JSON complet : {"device_id":..,"sensor":..,"seq":..,"ok":..,<nom>:<valeur>...,"units":{..}} */
int sensor_data_to_json(const sensor_data_t *d, const char *device_id, char *buf, size_t len);
