#pragma once
/*
 * coap_observe : gestion dynamique des abonnements Observe.
 *
 *  Possede l'unique tache CoAP (libcoap n'est pas thread-safe ici) :
 *   - fait tourner coap_io_process()
 *   - recoit par file FreeRTOS les evenements mDNS (pair ajoute / modifie / retire)
 *     et les nouvelles mesures (-> notifications Observe du serveur local)
 *   - pour chaque pair : ouvre une session DTLS, s'abonne a /sensor/temperature
 *     et /sensor/humidity, lit /device/info une fois
 *   - surveille les abonnements : silence prolonge ou erreur -> fermeture et
 *     nouvel essai avec delai croissant ; pair retire par mDNS -> abonnement supprime
 *   - chaque donnee recue d'un pair allume la LED
 */
#include "esp_err.h"
#include "coap3/coap.h"
#include "mdns_discovery.h"
#include "sensor_manager.h"

esp_err_t coap_observe_start(coap_context_t *ctx);

/* A brancher sur mdns_discovery_set_callback(). Thread-safe. */
void coap_observe_on_peer_event(peer_event_t event, const peer_info_t *peer, void *ctx);

/* A brancher sur sensor_manager_start(). Thread-safe. */
void coap_observe_on_new_measure(const sensor_data_t *data);

/* Demande l'affichage de l'etat des abonnements. Thread-safe. */
void coap_observe_log_status(void);
