#pragma once
/*
 * mqtt_manager : publication des mesures vers le broker MQTT (Mosquitto sur le PC),
 * puis Node-RED -> WebSocket -> page HTML / MongoDB Atlas.
 *
 *  Topics (prefixe APP_MQTT_TOPIC_PREFIX, "tp" par defaut ; <id> = esp32-xxxxxx) :
 *   tp/<id>/data    a chaque mesure : meme JSON que /sensor/data en CoAP
 *                   {"device_id":"esp32-..","sensor":"BME280","seq":12,"ok":true,
 *                    "temperature":24.1,...,"units":{...}}
 *   tp/<id>/status  retained : {"device_id":..,"state":"online","ip":..,"mesh_level":..,"sensor":..}
 *                   le broker publie {"device_id":..,"state":"offline"} si la carte disparait
 *                   (Last Will and Testament)
 *   tp/<id>/cmd     (abonnement) 2e sens, page -> carte : {"state":"on"|"off"|"toggle"}
 *   tp/<id>/actuator retained : {"device_id":..,"actuator":"LED","state":"on"} apres chaque commande
 *
 *  Fonctionne aussi a travers le mesh : c'est l'ESP qui ouvre la connexion TCP
 *  vers le broker, le NAT de chaque niveau la laisse passer.
 *  Reconnexion automatique (esp-mqtt). Sans APP_MQTT_ENABLE, fonctions neutres.
 */
#include <stdbool.h>
#include "esp_err.h"
#include "sensor_manager.h"

/* A appeler une fois le reseau connecte. */
esp_err_t mqtt_manager_start(void);

/* Publie la mesure sur tp/<id>/data (ignoree si le broker n'est pas connecte). Thread-safe. */
void mqtt_manager_publish_measure(const sensor_data_t *data);

bool mqtt_manager_is_connected(void);

/* Affiche l'etat de la connexion au broker et le nombre de messages publies. */
void mqtt_manager_log(void);
