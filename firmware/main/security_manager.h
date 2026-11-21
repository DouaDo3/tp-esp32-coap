#pragma once
/*
 * security_manager : configuration DTLS en mode PSK (Pre-Shared Key).
 *
 *  - Toutes les cartes partagent une cle de groupe (menuconfig APP_PSK_KEY).
 *  - Identite PSK d'un client = son nom unique (ex. esp32-a1b2c3).
 *  - Le serveur n'accepte que les identites "<prefixe>-..." et derive le
 *    chiffrement de la cle de groupe : sans la bonne cle, le handshake DTLS
 *    echoue (verification du message Finished) -> authentification mutuelle.
 *  - Suite negociee par mbedTLS : TLS-PSK-WITH-AES-128-CCM-8 (obligatoire CoAP).
 */
#include "esp_err.h"
#include "coap3/coap.h"

esp_err_t security_manager_init(void);

/* Installe la PSK serveur + le controle d'identite sur le contexte CoAP. */
esp_err_t security_manager_setup_server(coap_context_t *ctx);

/* Remplit la configuration PSK cliente (identite = nom de la carte). */
void security_manager_fill_client(coap_dtls_cpsk_t *setup);
