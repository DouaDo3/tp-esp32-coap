#pragma once
/*
 * coap_client : sessions CoAPS clientes vers les autres cartes.
 *  - ouvre une session DTLS-PSK vers l'IP/port fournis par mDNS
 *  - envoie des GET (avec ou sans option Observe)
 *  - remonte les donnees recues et les erreurs au proprietaire de la session
 *
 *  A appeler uniquement depuis la tache CoAP.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "coap3/coap.h"
#include "mdns_discovery.h"

typedef struct {
    /* Reponse 2.xx avec donnees (reponse initiale ou notification Observe). */
    void (*on_data)(void *owner, const uint8_t *data, size_t len);
    /* Echec de transport : handshake DTLS, timeout, session fermee... */
    void (*on_error)(void *owner, const char *reason);
    /* Reponse CoAP d'erreur (4.xx / 5.xx) a la requete sur 'path' (sans '/' initial, "?" si inconnu). */
    void (*on_response_error)(void *owner, const char *path, unsigned code_class, unsigned code_detail);
} coap_client_callbacks_t;

/* Enregistre les gestionnaires de reponses / evenements sur le contexte. */
void coap_client_init(coap_context_t *ctx, const coap_client_callbacks_t *cbs);

/* Ouvre une session DTLS vers le pair. owner est rendu dans les callbacks. */
coap_session_t *coap_client_open(coap_context_t *ctx, const peer_info_t *peer, void *owner);

/* GET confirmable ; observe=true ajoute Observe:0 (abonnement). */
bool coap_client_get(coap_session_t *session, const char *path, bool observe);

/* PUT confirmable, Content-Format application/json (ex. mesure -> racine du mesh).
 * La reponse 2.04 Changed n'a pas de donnees : seules les erreurs remontent. */
bool coap_client_put_json(coap_session_t *session, const char *path, const char *json);

/* Ferme la session (les callbacks ne sont plus appeles pour elle). */
void coap_client_close(coap_session_t *session);
