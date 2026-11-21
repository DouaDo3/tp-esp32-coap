#pragma once
/*
 * mdns_discovery : annonce et decouverte automatique des cartes ESP32.
 *
 *  Annonce  : hostname   <prefixe>-xxxxxx.local
 *             service    <prefixe>-xxxxxx._coaps._udp.local  port 5684
 *             TXT        id=<nom>  ver=1  res=/sensor/temperature,...
 *  Decouverte : requete PTR periodique sur _coaps._udp.local
 *             -> liste dynamique des pairs (IP, port, derniere vue)
 *             -> un pair qui ne repond plus pendant APP_PEER_TIMEOUT_S est retire
 *
 *  Aucune adresse IP n'est codee en dur : tout provient des reponses mDNS.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#include "sdkconfig.h"

#define MDNS_SERVICE_TYPE  CONFIG_APP_MDNS_SERVICE_TYPE    /* "_coaps" par defaut */
#define MDNS_SERVICE_PROTO "_udp"
#define PEER_NAME_LEN      32

typedef struct {
    char name[PEER_NAME_LEN];   /* identifiant unique (= nom d'instance mDNS) */
    esp_ip4_addr_t ip;
    uint16_t port;
    int64_t last_seen_us;       /* horodatage esp_timer de la derniere reponse */
    bool mesh_root;             /* racine Mesh-Lite (fournie par mesh_manager, pas par mDNS) */
} peer_info_t;

typedef enum {
    PEER_EVENT_ADDED,           /* nouvelle carte decouverte */
    PEER_EVENT_UPDATED,         /* adresse IP ou port modifie */
    PEER_EVENT_REMOVED,         /* carte disparue (timeout) */
} peer_event_t;

/* Appele depuis la tache de decouverte (ne pas bloquer longtemps). */
typedef void (*peer_event_cb_t)(peer_event_t event, const peer_info_t *peer, void *ctx);

/* Calcule l'identifiant unique a partir de l'adresse MAC (utilisable avant le Wi-Fi). */
const char *mdns_discovery_self_name(void);

/* Demarre mDNS, publie le service _coaps._udp et lance la tache de decouverte.
 * A appeler une fois le Wi-Fi demarre. */
esp_err_t mdns_discovery_start(uint16_t coaps_port);

/* Un seul abonne suffit (le module coap_observe a l'etape 6). */
void mdns_discovery_set_callback(peer_event_cb_t cb, void *ctx);

/* Copie la liste courante des pairs ; retourne le nombre copie. */
int mdns_discovery_get_peers(peer_info_t *out, int max);

/* Force le retrait d'un pair (ex. erreurs CoAPS repetees). Il sera rajoute
 * automatiquement s'il repond de nouveau a mDNS. */
void mdns_discovery_forget_peer(const char *name);

/* Affiche la table des pairs dans le moniteur serie. */
void mdns_discovery_log_peers(void);
