#pragma once
/*
 * wifi_manager : connexion au reseau.
 *  - APP_MESH_ENABLE : ESP-Mesh-Lite (STA vers le parent + SoftAP vers les enfants).
 *    La carte qui capte la box devient racine ; Mesh-Lite gere les reconnexions.
 *  - sinon : simple station (STA) avec reconnexion a delai croissant
 *  - SSID / mot de passe de la box configurables via menuconfig (TP CoAPS -> Wi-Fi)
 *  - l'adresse IP est obtenue par DHCP (jamais codee en dur)
 */
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"

/* Initialise NVS, la pile reseau, la boucle d'evenements et demarre le Wi-Fi. */
esp_err_t wifi_manager_start(void);

/* Bloque jusqu'a obtention d'une adresse IP (ou expiration). true si connecte. */
bool wifi_manager_wait_connected(TickType_t timeout);

bool wifi_manager_is_connected(void);

/* Adresse IPv4 actuelle de la carte (0 si non connectee). */
esp_ip4_addr_t wifi_manager_get_ip(void);

esp_netif_t *wifi_manager_get_netif(void);
