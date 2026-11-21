#pragma once
/*
 * Interface interne des pilotes de capteur. Un seul pilote est compile,
 * selon le choix menuconfig (chaque sensor_*.c est entoure d'un #if).
 */
#include "esp_err.h"
#include "sensor_manager.h"

/* Nom du modele, ex. "BMP280". */
extern const char *const sensor_drv_model;

/* Initialise le materiel et declare les grandeurs (name/unit/count) dans d. */
esp_err_t sensor_drv_init(sensor_data_t *d);

/* Met a jour d->v[i].value. Retourne ESP_OK si la lecture est valide. */
esp_err_t sensor_drv_read(sensor_data_t *d);

/* Optionnel : capteurs evenementiels (PIR). Le pilote appelle cette fonction
 * (depuis une ISR) pour demander une mesure immediate. */
void sensor_manager_wake_from_isr(void);
