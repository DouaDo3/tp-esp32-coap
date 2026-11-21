#pragma once
/*
 * mesh_manager : role de la carte dans le reseau ESP-Mesh-Lite.
 *
 *  Verifie periodiquement (APP_MESH_CHECK_INTERVAL_S) :
 *   - le niveau de la carte : 1 = racine (reliee a la box), 2, 3... = enfant
 *   - l'adresse IP de la racine (esp_mesh_lite_get_root_ip)
 *  Sur un enfant, la racine est signalee comme un pair "mesh-root"
 *  (PEER_EVENT_ADDED / UPDATED / REMOVED) : coap_observe s'y abonne en CoAPS
 *  et lui envoie ses mesures par PUT. La racine ne peut pas ouvrir de session
 *  vers un enfant (NAT a chaque niveau) : ce sont toujours les enfants qui appellent.
 *
 *  Sans APP_MESH_ENABLE, toutes les fonctions sont neutres.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "mdns_discovery.h"

#define MESH_ROOT_PEER_NAME "mesh-root"

/* Sans mesh, ce chemin n'existe pas dans menuconfig : valeur neutre
 * pour que coap_server / coap_observe compilent (jamais utilise). */
#include "sdkconfig.h"
#ifndef CONFIG_APP_MESH_DATA_PATH
#define CONFIG_APP_MESH_DATA_PATH "/mesh/data"
#endif

/* Lance la surveillance ; cb recoit les evenements du pair "mesh-root". */
esp_err_t mesh_manager_start(peer_event_cb_t cb, void *ctx);

/* Niveau dans le mesh (0 = pas encore connecte, 1 = racine). */
uint8_t mesh_manager_level(void);

bool mesh_manager_is_root(void);

/* Affiche niveau, IP de la racine et nombre de noeuds connus. */
void mesh_manager_log(void);
