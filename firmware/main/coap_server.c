#include "coap_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "led_manager.h"
#include "mesh_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "mdns_discovery.h"
#include "security_manager.h"
#include "sensor_manager.h"
#include "wifi_manager.h"

static const char *TAG = "coap_server";

static coap_resource_t *s_sensor_res[SENSOR_MAX_VALUES + 2];
static int s_sensor_res_count;

/* Suivi des observateurs (RFC 7641) : libcoap rappelle le handler GET avec la
 * requete d'abonnement d'origine a chaque notification. On distingue donc le
 * premier appel (abonnement) des suivants (notifications) par session+ressource+token. */
#define OBS_TRACK_MAX 16
typedef struct {
    coap_session_t *session;
    coap_resource_t *resource;
    uint8_t token[8];
    size_t token_len;
    uint32_t count;
} obs_track_t;
static obs_track_t s_obs[OBS_TRACK_MAX];

static void token_hex(const uint8_t *t, size_t len, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 3 <= out_len; i++) {
        o += snprintf(out + o, out_len - o, "%02x", t[i]);
    }
    if (len == 0) {
        snprintf(out, out_len, "(vide)");
    }
}

static void log_if_subscription(coap_resource_t *resource, coap_session_t *session, const coap_pdu_t *request)
{
    coap_opt_iterator_t it;
    coap_opt_t *opt = coap_check_option(request, COAP_OPTION_OBSERVE, &it);
    if (!opt) {
        return;
    }
    const coap_bin_const_t *id = coap_session_get_psk_identity(session);
    coap_str_const_t *path = coap_resource_get_uri_path(resource);
    uint32_t action = coap_decode_var_bytes(coap_opt_value(opt), coap_opt_length(opt));
    coap_bin_const_t tok = coap_pdu_get_token(request);
    size_t tlen = tok.length > 8 ? 8 : tok.length;
    char thex[20];
    token_hex(tok.s, tlen, thex, sizeof(thex));
    int idl = id ? (int)id->length : 1;
    const char *ids = id ? (const char *)id->s : "?";
    int pl = path ? (int)path->length : 0;
    const char *ps = path ? (const char *)path->s : "";

    int found = -1, free_slot = -1;
    for (int i = 0; i < OBS_TRACK_MAX; i++) {
        obs_track_t *o = &s_obs[i];
        if (o->session == NULL) {
            if (free_slot < 0) {
                free_slot = i;
            }
        } else if (o->session == session && o->resource == resource) {
            found = i;
        }
    }

    if (action != COAP_OBSERVE_ESTABLISH) {
        ESP_LOGI(TAG, "[OBSERVE] %.*s se desabonne de /%.*s (Observe=1, token=%s)", idl, ids, pl, ps, thex);
        if (found >= 0) {
            memset(&s_obs[found], 0, sizeof(s_obs[found]));
        }
        return;
    }

    if (found >= 0 && s_obs[found].token_len == tlen && memcmp(s_obs[found].token, tok.s, tlen) == 0) {
        s_obs[found].count++;
        ESP_LOGI(TAG, "[OBSERVE] notification #%" PRIu32 " envoyee -> %.*s  /%.*s  token=%s",
                 s_obs[found].count, idl, ids, pl, ps, thex);
        return;
    }

    /* Nouvel abonnement (ou nouveau token = reabonnement) */
    int slot = found >= 0 ? found : free_slot;
    if (slot < 0) {
        slot = 0;   /* table pleine : on recycle */
    }
    s_obs[slot].session = session;
    s_obs[slot].resource = resource;
    memcpy(s_obs[slot].token, tok.s, tlen);
    s_obs[slot].token_len = tlen;
    s_obs[slot].count = 0;
    ESP_LOGI(TAG, "[OBSERVE] %.*s s'abonne a /%.*s (GET + Observe=0, token=%s) - RFC 7641",
             idl, ids, pl, ps, thex);
}

static void send_json(coap_resource_t *resource, coap_session_t *session, const coap_pdu_t *request,
                      const coap_string_t *query, coap_pdu_t *response, const char *json, int maxage)
{
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    coap_add_data_large_response(resource, session, request, response, query,
                                 COAP_MEDIATYPE_APPLICATION_JSON, maxage, 0,
                                 strlen(json), (const uint8_t *)json, NULL, NULL);
}

/* GET /sensor/data : toutes les grandeurs */
static void hnd_get_data(coap_resource_t *resource, coap_session_t *session,
                         const coap_pdu_t *request, const coap_string_t *query, coap_pdu_t *response)
{
    log_if_subscription(resource, session, request);
    sensor_data_t d;
    sensor_manager_get(&d);
    char json[320];
    sensor_data_to_json(&d, mdns_discovery_self_name(), json, sizeof(json));
    send_json(resource, session, request, query, response, json, CONFIG_APP_SENSOR_PERIOD_S * 2);
}

/* GET /sensor/<nom> : une grandeur (nom stocke dans userdata de la ressource) */
static void hnd_get_value(coap_resource_t *resource, coap_session_t *session,
                          const coap_pdu_t *request, const coap_string_t *query, coap_pdu_t *response)
{
    log_if_subscription(resource, session, request);
    const char *name = coap_resource_get_userdata(resource);
    sensor_data_t d;
    sensor_manager_get(&d);
    float value;
    const char *unit;
    if (!sensor_data_find(&d, name, &value, &unit)) {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_NOT_FOUND);
        return;
    }
    char json[160];
    snprintf(json, sizeof(json), "{\"device_id\":\"%s\",\"%s\":%g,\"unit\":\"%s\",\"seq\":%" PRIu32 "}",
             mdns_discovery_self_name(), name, value, unit, d.seq);
    send_json(resource, session, request, query, response, json, CONFIG_APP_SENSOR_PERIOD_S * 2);
}

static void hnd_get_info(coap_resource_t *resource, coap_session_t *session,
                         const coap_pdu_t *request, const coap_string_t *query, coap_pdu_t *response)
{
    esp_ip4_addr_t ip = wifi_manager_get_ip();
    sensor_data_t d;
    sensor_manager_get(&d);
    char json[256];
    snprintf(json, sizeof(json),
             "{\"device_id\":\"%s\",\"ip\":\"" IPSTR "\",\"port\":%d,\"sensor\":\"%s\","
             "\"uptime_s\":%lld,\"free_heap\":%u}",
             mdns_discovery_self_name(), IP2STR(&ip), CONFIG_APP_COAPS_PORT, d.model,
             esp_timer_get_time() / 1000000, (unsigned)esp_get_free_heap_size());
    send_json(resource, session, request, query, response, json, 0);
}

#if CONFIG_APP_MESH_ENABLE
/* ------------------------------------------------------------------ */
/* /mesh/data : collecte des mesures du mesh (utile sur la racine)     */
/* ------------------------------------------------------------------ */

#define MESH_JSON_LEN 320           /* = taille du JSON de /sensor/data */

typedef struct {
    char device_id[PEER_NAME_LEN];
    char json[MESH_JSON_LEN];       /* derniere mesure deposee par PUT */
    int64_t last_us;
} mesh_node_t;

static mesh_node_t s_nodes[CONFIG_APP_MAX_PEERS];
static coap_resource_t *s_mesh_res;

static bool node_is_fresh(const mesh_node_t *n, int64_t now)
{
    return n->device_id[0] && now - n->last_us < (int64_t)CONFIG_APP_PEER_TIMEOUT_S * 1000000;
}

/* PUT /mesh/data : un enfant depose sa derniere mesure */
static void hnd_put_mesh(coap_resource_t *resource, coap_session_t *session,
                         const coap_pdu_t *request, const coap_string_t *query, coap_pdu_t *response)
{
    size_t len = 0, offset = 0, total = 0;
    const uint8_t *data = NULL;
    if (!coap_get_data_large(request, &len, &data, &offset, &total) || len == 0 || len >= MESH_JSON_LEN) {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }
    /* Identifiant : champ device_id, sinon identite PSK de la session DTLS */
    char id[PEER_NAME_LEN];
    const cJSON *jid = cJSON_GetObjectItem(root, "device_id");
    const coap_bin_const_t *psk_id = coap_session_get_psk_identity(session);
    if (cJSON_IsString(jid)) {
        strlcpy(id, jid->valuestring, sizeof(id));
    } else if (psk_id) {
        snprintf(id, sizeof(id), "%.*s", (int)psk_id->length, (const char *)psk_id->s);
    } else {
        strlcpy(id, "inconnu", sizeof(id));
    }
    cJSON_Delete(root);

    /* Case du noeud : la sienne, sinon une libre ou perimee, sinon la plus ancienne */
    int64_t now = esp_timer_get_time();
    mesh_node_t *slot = NULL, *oldest = &s_nodes[0];
    for (int i = 0; i < CONFIG_APP_MAX_PEERS && !slot; i++) {
        if (strcmp(s_nodes[i].device_id, id) == 0) {
            slot = &s_nodes[i];
        }
    }
    for (int i = 0; i < CONFIG_APP_MAX_PEERS && !slot; i++) {
        if (!node_is_fresh(&s_nodes[i], now)) {
            slot = &s_nodes[i];
        } else if (s_nodes[i].last_us < oldest->last_us) {
            oldest = &s_nodes[i];
        }
    }
    if (!slot) {
        slot = oldest;
    }
    bool is_new = strcmp(slot->device_id, id) != 0;
    strlcpy(slot->device_id, id, sizeof(slot->device_id));
    memcpy(slot->json, data, len);
    slot->json[len] = '\0';
    slot->last_us = now;

    ESP_LOGI(TAG, "[MESH] PUT /mesh/data de %s%s : %s", id, is_new ? " (nouveau noeud)" : "", slot->json);
    led_manager_pulse();     /* donnee recue d'un autre ESP32 */

    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
    coap_resource_notify_observers(s_mesh_res, NULL);
}

/* GET /mesh/data : {"root":"esp32-..","level":1,"nodes":[<mesure locale>,<mesures des enfants>...]} */
static void hnd_get_mesh(coap_resource_t *resource, coap_session_t *session,
                         const coap_pdu_t *request, const coap_string_t *query, coap_pdu_t *response)
{
    log_if_subscription(resource, session, request);
    size_t cap = 64 + (CONFIG_APP_MAX_PEERS + 1) * (MESH_JSON_LEN + 1);
    char *buf = malloc(cap);
    if (!buf) {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_INTERNAL_ERROR);
        return;
    }
    size_t n = snprintf(buf, cap, "{\"root\":\"%s\",\"level\":%u,\"nodes\":[",
                        mdns_discovery_self_name(), mesh_manager_level());
    sensor_data_t d;
    sensor_manager_get(&d);
    n += sensor_data_to_json(&d, mdns_discovery_self_name(), buf + n, cap - n);

    int64_t now = esp_timer_get_time();
    for (int i = 0; i < CONFIG_APP_MAX_PEERS && n < cap; i++) {
        if (node_is_fresh(&s_nodes[i], now)) {
            n += snprintf(buf + n, cap - n, ",%s", s_nodes[i].json);
        }
    }
    if (n < cap) {
        snprintf(buf + n, cap - n, "]}");
    }
    send_json(resource, session, request, query, response, buf, CONFIG_APP_SENSOR_PERIOD_S * 2);
    free(buf);
}
#endif /* CONFIG_APP_MESH_ENABLE */

static coap_resource_t *add_resource(coap_context_t *ctx, const char *path,
                                     coap_method_handler_t get, bool observable, void *userdata)
{
    coap_resource_t *r = coap_resource_init(coap_make_str_const(path), 0);  /* chemin copie par libcoap */
    coap_register_request_handler(r, COAP_REQUEST_GET, get);
    coap_resource_set_userdata(r, userdata);
    coap_add_attr(r, coap_make_str_const("ct"), coap_make_str_const("50"), 0);
    if (observable) {
        coap_resource_set_get_observable(r, 1);
        coap_add_attr(r, coap_make_str_const("obs"), NULL, 0);
    }
    coap_add_resource(ctx, r);
    ESP_LOGI(TAG, "  ressource /%s%s", path, observable ? " (observable)" : "");
    return r;
}

coap_context_t *coap_server_create(void)
{
    coap_startup();
    coap_set_log_level(COAP_LOG_WARN);
    coap_dtls_set_log_level(COAP_LOG_WARN);

    coap_context_t *ctx = coap_new_context(NULL);
    if (!ctx) {
        ESP_LOGE(TAG, "coap_new_context a echoue");
        return NULL;
    }
    /* libcoap gere la fragmentation (Block2) et l'Observe des reponses */
    coap_context_set_block_mode(ctx, COAP_BLOCK_USE_LIBCOAP | COAP_BLOCK_SINGLE_BODY);

    if (security_manager_setup_server(ctx) != ESP_OK) {
        ESP_LOGE(TAG, "Configuration PSK serveur impossible");
        coap_free_context(ctx);
        return NULL;
    }

    coap_address_t addr;
    coap_address_init(&addr);
    addr.addr.sin.sin_family = AF_INET;
    addr.addr.sin.sin_addr.s_addr = INADDR_ANY;
    addr.addr.sin.sin_port = htons(CONFIG_APP_COAPS_PORT);

    /* COAP_PROTO_DTLS uniquement : aucun point d'acces CoAP en clair n'est ouvert */
    if (!coap_new_endpoint(ctx, &addr, COAP_PROTO_DTLS)) {
        ESP_LOGE(TAG, "Impossible d'ouvrir le port DTLS %d", CONFIG_APP_COAPS_PORT);
        coap_free_context(ctx);
        return NULL;
    }

    sensor_data_t d;
    sensor_manager_get(&d);
    s_sensor_res_count = 0;
    s_sensor_res[s_sensor_res_count++] = add_resource(ctx, "sensor/data", hnd_get_data, true, NULL);
    /* Alias /sensor (meme JSON) : chemin utilise par le code d'un camarade */
    s_sensor_res[s_sensor_res_count++] = add_resource(ctx, "sensor", hnd_get_data, true, NULL);
    for (int i = 0; i < d.count; i++) {
        char path[48];
        snprintf(path, sizeof(path), "sensor/%s", d.v[i].name);
        s_sensor_res[s_sensor_res_count++] = add_resource(ctx, path, hnd_get_value, true, (void *)d.v[i].name);
    }
    add_resource(ctx, "device/info", hnd_get_info, false, NULL);

#if CONFIG_APP_MESH_ENABLE
    /* Creee sur toutes les cartes : n'importe laquelle peut devenir racine */
    const char *mp = CONFIG_APP_MESH_DATA_PATH;
    s_mesh_res = add_resource(ctx, mp[0] == '/' ? mp + 1 : mp, hnd_get_mesh, true, NULL);
    coap_register_request_handler(s_mesh_res, COAP_REQUEST_PUT, hnd_put_mesh);
    ESP_LOGI(TAG, "  ressource %s : PUT accepte (mesures des enfants du mesh)", mp);
#endif

    ESP_LOGI(TAG, "Serveur CoAPS (DTLS-PSK) a l'ecoute sur UDP %d", CONFIG_APP_COAPS_PORT);
    return ctx;
}

void coap_server_notify_sensors(void)
{
    for (int i = 0; i < s_sensor_res_count; i++) {
        coap_resource_notify_observers(s_sensor_res[i], NULL);
    }
#if CONFIG_APP_MESH_ENABLE
    coap_resource_notify_observers(s_mesh_res, NULL);   /* la mesure locale fait partie de /mesh/data */
#endif
}
