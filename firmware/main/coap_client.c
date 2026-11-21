#include "coap_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "security_manager.h"

static const char *TAG = "coap_client";

static coap_client_callbacks_t s_cbs;

static void *owner_of(coap_session_t *session)
{
    if (coap_session_get_type(session) != COAP_SESSION_TYPE_CLIENT) {
        return NULL;    /* session serveur (un autre ESP32 qui nous observe) */
    }
    return coap_session_get_app_data(session);
}

/* Affiche les champs qui font d'une reponse une notification Observe (RFC 7641) :
 * option Observe (numero de sequence croissant), token (reference de l'abonnement,
 * identique a celui du GET initial), MID, type de message et code 2.05. */
static void log_observe_notification(coap_session_t *session, const coap_pdu_t *received, coap_mid_t mid)
{
    coap_opt_iterator_t it;
    coap_opt_t *opt = coap_check_option(received, COAP_OPTION_OBSERVE, &it);
    if (!opt) {
        return;     /* reponse simple (ex. GET /device/info), pas une notification */
    }
    uint32_t seq = coap_decode_var_bytes(coap_opt_value(opt), coap_opt_length(opt));

    coap_bin_const_t tok = coap_pdu_get_token(received);
    char thex[20] = "(vide)";
    size_t o = 0;
    for (size_t i = 0; i < tok.length && i < 8; i++) {
        o += snprintf(thex + o, sizeof(thex) - o, "%02x", tok.s[i]);
    }

    char addr[48] = "?";
    coap_print_addr(coap_session_get_addr_remote(session), (unsigned char *)addr, sizeof(addr));

    coap_pdu_type_t t = coap_pdu_get_type(received);
    const char *type = t == COAP_MESSAGE_CON ? "CON" : t == COAP_MESSAGE_NON ? "NON"
                     : t == COAP_MESSAGE_ACK ? "ACK" : "RST";
    coap_pdu_code_t code = coap_pdu_get_code(received);

    ESP_LOGI(TAG, "[OBSERVE] notification recue de %s : Observe=%" PRIu32 " token=%s MID=%u type=%s code=%u.%02u",
             addr, seq, thex, (unsigned)mid, type, (unsigned)COAP_RESPONSE_CLASS(code), (unsigned)(code & 0x1F));
}

static coap_response_t response_handler(coap_session_t *session, const coap_pdu_t *sent,
                                        const coap_pdu_t *received, const coap_mid_t mid)
{
    void *owner = owner_of(session);
    if (!owner) {
        return COAP_RESPONSE_OK;
    }
    coap_pdu_code_t code = coap_pdu_get_code(received);
    unsigned cls = COAP_RESPONSE_CLASS(code);

    if (cls == 2) {
        log_observe_notification(session, received, mid);
        size_t len = 0, offset = 0, total = 0;
        const uint8_t *data = NULL;
        if (coap_get_data_large(received, &len, &data, &offset, &total) && len > 0) {
            s_cbs.on_data(owner, data, len);
        }
    } else if (cls == 4 || cls == 5) {
        coap_string_t *path = sent ? coap_get_uri_path(sent) : NULL;
        char p[48] = "?";
        if (path) {
            snprintf(p, sizeof(p), "%.*s", (int)path->length, (const char *)path->s);
            coap_delete_string(path);
        }
        s_cbs.on_response_error(owner, p, cls, code & 0x1F);
    }
    return COAP_RESPONSE_OK;
}

static void nack_handler(coap_session_t *session, const coap_pdu_t *sent,
                         const coap_nack_reason_t reason, const coap_mid_t mid)
{
    void *owner = owner_of(session);
    if (!owner) {
        return;
    }
    switch (reason) {
    case COAP_NACK_TOO_MANY_RETRIES: s_cbs.on_error(owner, "pas de reponse (retransmissions epuisees)"); break;
    case COAP_NACK_TLS_FAILED:
    case COAP_NACK_TLS_LAYER_FAILED: s_cbs.on_error(owner, "echec DTLS (cle PSK differente ?)"); break;
    case COAP_NACK_RST:              s_cbs.on_error(owner, "RST recu (le pair a oublie la session)"); break;
    case COAP_NACK_NOT_DELIVERABLE:
    case COAP_NACK_ICMP_ISSUE:       s_cbs.on_error(owner, "pair injoignable"); break;
    default:                         s_cbs.on_error(owner, "requete non aboutie"); break;
    }
}

static int event_handler(coap_session_t *session, const coap_event_t event)
{
    void *owner = owner_of(session);
    if (owner) {
        switch (event) {
        case COAP_EVENT_DTLS_CONNECTED: ESP_LOGI(TAG, "Handshake DTLS reussi (session chiffree)"); break;
        case COAP_EVENT_DTLS_ERROR:     s_cbs.on_error(owner, "erreur DTLS"); break;
        case COAP_EVENT_DTLS_CLOSED:    s_cbs.on_error(owner, "session DTLS fermee par le pair"); break;
        case COAP_EVENT_SESSION_FAILED: s_cbs.on_error(owner, "session en echec"); break;
        default: break;
        }
    } else if (coap_session_get_type(session) == COAP_SESSION_TYPE_SERVER) {
        const coap_bin_const_t *id = coap_session_get_psk_identity(session);
        int idl = id ? (int)id->length : 1;
        const char *ids = id ? (const char *)id->s : "?";
        if (event == COAP_EVENT_DTLS_CONNECTED) {
            ESP_LOGI(TAG, "[serveur] client authentifie par DTLS-PSK : %.*s", idl, ids);
        } else if (event == COAP_EVENT_DTLS_ERROR) {
            ESP_LOGW(TAG, "[serveur] handshake DTLS refuse (mauvaise cle ou identite)");
        } else if (event == COAP_EVENT_DTLS_CLOSED) {
            ESP_LOGI(TAG, "[serveur] session DTLS fermee : %.*s", idl, ids);
        }
    }
    return 0;
}

void coap_client_init(coap_context_t *ctx, const coap_client_callbacks_t *cbs)
{
    s_cbs = *cbs;
    coap_register_response_handler(ctx, response_handler);
    coap_register_nack_handler(ctx, nack_handler);
    coap_register_event_handler(ctx, event_handler);
}

coap_session_t *coap_client_open(coap_context_t *ctx, const peer_info_t *peer, void *owner)
{
    coap_address_t dst;
    coap_address_init(&dst);
    dst.addr.sin.sin_family = AF_INET;
    dst.addr.sin.sin_addr.s_addr = peer->ip.addr;    /* deja en ordre reseau */
    dst.addr.sin.sin_port = htons(peer->port);

    coap_dtls_cpsk_t cpsk;
    security_manager_fill_client(&cpsk);

    coap_session_t *s = coap_new_client_session_psk2(ctx, NULL, &dst, COAP_PROTO_DTLS, &cpsk);
    if (!s) {
        ESP_LOGW(TAG, "Creation session DTLS vers %s impossible", peer->name);
        return NULL;
    }
    coap_session_set_app_data(s, owner);
    return s;
}

static coap_pdu_t *new_request(coap_session_t *session, coap_pdu_code_t code)
{
    coap_pdu_t *pdu = coap_new_pdu(COAP_MESSAGE_CON, code, session);
    if (pdu) {
        uint8_t token[8];
        size_t tlen = 0;
        coap_session_new_token(session, &tlen, token);
        coap_add_token(pdu, tlen, token);
    }
    return pdu;
}

/* Une option Uri-Path (11) par segment du chemin "/a/b" */
static void add_uri_path(coap_pdu_t *pdu, const char *path)
{
    const char *seg = path[0] == '/' ? path + 1 : path;
    while (*seg) {
        const char *end = strchr(seg, '/');
        size_t len = end ? (size_t)(end - seg) : strlen(seg);
        coap_add_option(pdu, COAP_OPTION_URI_PATH, len, (const uint8_t *)seg);
        seg += len;
        if (*seg == '/') {
            seg++;
        }
    }
}

bool coap_client_get(coap_session_t *session, const char *path, bool observe)
{
    coap_pdu_t *pdu = new_request(session, COAP_REQUEST_CODE_GET);
    if (!pdu) {
        return false;
    }
    /* Les options doivent etre ajoutees par numero croissant : Observe(6) puis Uri-Path(11) */
    if (observe) {
        uint8_t buf[4];
        coap_add_option(pdu, COAP_OPTION_OBSERVE,
                        coap_encode_var_safe(buf, sizeof(buf), COAP_OBSERVE_ESTABLISH), buf);
    }
    add_uri_path(pdu, path);
    return coap_send(session, pdu) != COAP_INVALID_MID;
}

bool coap_client_put_json(coap_session_t *session, const char *path, const char *json)
{
    coap_pdu_t *pdu = new_request(session, COAP_REQUEST_CODE_PUT);
    if (!pdu) {
        return false;
    }
    /* Uri-Path(11) puis Content-Format(12) = 50 (application/json) */
    add_uri_path(pdu, path);
    uint8_t buf[4];
    coap_add_option(pdu, COAP_OPTION_CONTENT_FORMAT,
                    coap_encode_var_safe(buf, sizeof(buf), COAP_MEDIATYPE_APPLICATION_JSON), buf);
    if (!coap_add_data(pdu, strlen(json), (const uint8_t *)json)) {
        coap_delete_pdu(pdu);
        return false;
    }
    return coap_send(session, pdu) != COAP_INVALID_MID;
}

void coap_client_close(coap_session_t *session)
{
    if (session) {
        coap_session_set_app_data(session, NULL);   /* plus aucun callback pour cette session */
        coap_session_release(session);
    }
}
