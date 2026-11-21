#pragma once
/*
 * coap_server : serveur CoAPS (DTLS-PSK) sur le port 5684.
 *
 *  Ressources (JSON, Content-Format 50), creees selon le capteur choisi :
 *   /sensor/data      observable  toutes les grandeurs :
 *                     {"device_id":"esp32-..","sensor":"DHT11","seq":12,"ok":true,
 *                      "temperature":25.6,"humidity":60,"units":{"temperature":"C","humidity":"%"}}
 *   /sensor/<nom>     observable  une grandeur, ex. /sensor/temperature :
 *                     {"device_id":"esp32-..","temperature":25.6,"unit":"C","seq":12}
 *   /device/info      GET         {"device_id":..,"ip":..,"port":5684,"sensor":..,"uptime_s":..,"free_heap":..}
 *
 *  libcoap n'est pas thread-safe ici : appeler uniquement depuis la tache CoAP (coap_observe).
 *  sensor_manager_init() doit avoir ete appele avant coap_server_create().
 */
#include "coap3/coap.h"

coap_context_t *coap_server_create(void);

/* Notification Observe de toutes les ressources capteur. */
void coap_server_notify_sensors(void);
