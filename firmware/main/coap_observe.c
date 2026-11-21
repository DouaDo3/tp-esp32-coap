#include "coap_observe.h"

#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "coap_client.h"
#include "coap_server.h"
#include "led_manager.h"
#include "mesh_manager.h"

static const char *TAG = "coap_observe";

#define S_TO_US(s) ((int64_t)(s) * 1000000)

typedef enum {
    SUB_FREE = 0,
    SUB_WAIT_RETRY,     /* en attente avant (re)connexion */
    SUB_CONNECTING,     /* handshake DTLS + requetes Observe envoyees */
    SUB_ACTIVE,         /* au moins une donnee recue : abonnement etabli */
} sub_state_t;

static const char *STATE_NAMES[] = { "libre", "attente", "connexion", "ABONNE" };

typedef struct {
    sub_state_t state;
    peer_info_t peer;
    coap_session_t *session;
    int64_t deadline_us;      /* WAIT_RETRY : heure du prochain essai ; CONNECTING : expiration */
    int64_t last_rx_us;
    uint32_t fails;
    uint32_t rx_count;
    bool error;               /* positionne par les callbacks, traite dans la boucle */
    char error_reason[48];
    bool need_fallback;       /* le pair n'a pas une ressource principale : observer les ressources de repli */
    bool fallback_sent;       /* repli deja tente sur cette session */
    char sensor[16];          /* modele annonce par le pair */
    char last_values[96];     /* derniere mesure lisible, ex. "temperature=24.1C humidity=55%" */
} subscription_t;

typedef enum { CMD_PEER, CMD_NOTIFY, CMD_LOG } cmd_type_t;

typedef struct {
    cmd_type_t type;
    peer_event_t peer_event;
    peer_info_t peer;
} cmd_t;

static coap_context_t *s_ctx;
static QueueHandle_t s_queue;
static subscription_t s_subs[CONFIG_APP_MAX_PEERS];

/* ------------------------------------------------------------------ */
/* Callbacks du client (executes dans la tache CoAP)                   */
/* ------------------------------------------------------------------ */

/* "temperature=24.1C humidity=55%" a partir d'un objet JSON de mesure.
 * Unites : objet "units" (format /sensor/data) ou champ "unit" (format /sensor/<nom>). */
static int format_values(const cJSON *obj, char *out, size_t size)
{
    const cJSON *units = cJSON_GetObjectItem(obj, "units");
    const cJSON *unit1 = cJSON_GetObjectItem(obj, "unit");
    int n = 0;
    out[0] = '\0';
    const cJSON *it;
    cJSON_ArrayForEach(it, obj) {
        if (!cJSON_IsNumber(it) || strcmp(it->string, "seq") == 0 || strcmp(it->string, "ok") == 0) {
            continue;
        }
        const cJSON *u = cJSON_GetObjectItem(units, it->string);
        const char *us = cJSON_IsString(u) ? u->valuestring : (cJSON_IsString(unit1) ? unit1->valuestring : "");
        if (n < (int)size) {
            n += snprintf(out + n, size - n, "%s%s=%g%s", n ? " " : "", it->string, it->valuedouble, us);
        }
    }
    return n;
}

/* Reponse de la racine du mesh : {"root":..,"nodes":[{mesure},{mesure}...]} */
static void on_mesh_data(subscription_t *sub, const cJSON *nodes)
{
    const char *self = mdns_discovery_self_name();
    int others = 0;
    char line[96];
    const cJSON *node;
    cJSON_ArrayForEach(node, nodes) {
        const cJSON *id = cJSON_GetObjectItem(node, "device_id");
        const char *dev = cJSON_IsString(id) ? id->valuestring : "?";
        if (strcmp(dev, self) == 0) {
            continue;   /* notre propre mesure, renvoyee par la racine */
        }
        format_values(node, line, sizeof(line));
        ESP_LOGI(TAG, "<< [MESH] %s : %s", dev, line);
        if (others == 0) {
            /* 20 + 2 + 72 + '\0' <= 96 : resume tronque volontairement pour le tableau d'etat */
            snprintf(sub->last_values, sizeof(sub->last_values), "%.20s: %.72s", dev, line);
        }
        others++;
    }
    ESP_LOGI(TAG, "<< [MESH] %d noeud(s) du mesh recus via la racine", others);
}

static void on_data(void *owner, const uint8_t *data, size_t len)
{
    subscription_t *sub = owner;
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    if (!cJSON_IsObject(root)) {
        /* Une autre carte peut envoyer du texte brut ("25.6") ou un simple nombre JSON :
         * c'est quand meme une donnee recue. */
        snprintf(sub->last_values, sizeof(sub->last_values), "%.*s", (int)len, (const char *)data);
        ESP_LOGI(TAG, "<< %s : %s (non JSON objet)", sub->peer.name, sub->last_values);
        cJSON_Delete(root);
        goto received;
    }
    const cJSON *id = cJSON_GetObjectItem(root, "device_id");
    const char *dev = cJSON_IsString(id) ? id->valuestring : sub->peer.name;
    const cJSON *model = cJSON_GetObjectItem(root, "sensor");
    if (cJSON_IsString(model)) {
        strlcpy(sub->sensor, model->valuestring, sizeof(sub->sensor));
    }

    const cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
    if (cJSON_IsArray(nodes)) {
        /* /mesh/data de la racine : mesures de tout le mesh */
        on_mesh_data(sub, nodes);
    } else if (cJSON_GetObjectItem(root, "uptime_s")) {
        /* reponse a /device/info */
        ESP_LOGI(TAG, "<< %s : info %.*s", dev, (int)len, (const char *)data);
    } else {
        /* Mesure : on affiche toutes les grandeurs numeriques, quel que soit le capteur. */
        char vals[96];
        int n = format_values(root, vals, sizeof(vals));
        if (n == 0) {
            snprintf(vals, sizeof(vals), "%.*s", (int)len, (const char *)data);
        }
        strlcpy(sub->last_values, vals, sizeof(sub->last_values));
        ESP_LOGI(TAG, "<< %s : %s", dev, vals);
    }
    cJSON_Delete(root);

received:
    /* Donnee effectivement recue d'un autre ESP32 -> LED */
    led_manager_pulse();

    sub->rx_count++;
    sub->last_rx_us = esp_timer_get_time();
    if (sub->state == SUB_CONNECTING) {
        sub->state = SUB_ACTIVE;
        sub->fails = 0;
        ESP_LOGI(TAG, "Abonnement Observe etabli avec %s (" IPSTR ")", sub->peer.name, IP2STR(&sub->peer.ip));
    }
}

static void on_error(void *owner, const char *reason)
{
    subscription_t *sub = owner;
    if (!sub->error) {
        sub->error = true;
        strlcpy(sub->error_reason, reason, sizeof(sub->error_reason));
    }
}

/* Le chemin (sans '/' initial) fait-il partie de la liste "/a/b,/c" ? */
static bool path_in_list(const char *path, const char *list)
{
    size_t plen = strlen(path);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '/') {
            p++;
        }
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        while (len && p[len - 1] == ' ') {
            len--;
        }
        if (len == plen && strncmp(p, path, len) == 0) {
            return true;
        }
        p += len;
        while (*p && *p != ',') {
            p++;
        }
    }
    return false;
}

/* Envoie un GET (Observe ou non) pour chaque chemin de la liste "/a/b,/c". */
static bool get_path_list(coap_session_t *session, const char *list, bool observe)
{
    char buf[128];
    strlcpy(buf, list, sizeof(buf));
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') {
            tok++;
        }
        size_t len = strlen(tok);
        while (len && tok[len - 1] == ' ') {
            tok[--len] = '\0';
        }
        if (len && !coap_client_get(session, tok, observe)) {
            return false;
        }
    }
    return true;
}

static void on_response_error(void *owner, const char *path, unsigned cls, unsigned detail)
{
    subscription_t *sub = owner;
    if (cls == 4 && detail == 4 && !sub->fallback_sent && CONFIG_APP_OBSERVE_FALLBACK_PATHS[0] != '\0' &&
        path_in_list(path, CONFIG_APP_OBSERVE_PATHS)) {
        /* Carte d'un autre groupe sans cette ressource : on tente les ressources de repli */
        ESP_LOGW(TAG, "%s n'a pas /%s -> abonnement de repli a %s",
                 sub->peer.name, path, CONFIG_APP_OBSERVE_FALLBACK_PATHS);
        sub->need_fallback = true;
        sub->fallback_sent = true;
    } else {
        /* Ressource absente ou refusee : on le signale sans couper la session */
        ESP_LOGW(TAG, "%s : /%s -> erreur %u.%02u", sub->peer.name, path, cls, detail);
    }
}

/* ------------------------------------------------------------------ */
/* Machine a etats des abonnements                                     */
/* ------------------------------------------------------------------ */

static subscription_t *find_sub(const char *name)
{
    for (int i = 0; i < CONFIG_APP_MAX_PEERS; i++) {
        if (s_subs[i].state != SUB_FREE && strcmp(s_subs[i].peer.name, name) == 0) {
            return &s_subs[i];
        }
    }
    return NULL;
}

static void close_session(subscription_t *sub)
{
    coap_client_close(sub->session);
    sub->session = NULL;
    sub->error = false;
}

static void schedule_retry(subscription_t *sub, const char *reason)
{
    close_session(sub);
    sub->fails++;
    uint32_t delay = 1u << (sub->fails < 6 ? sub->fails : 6);
    if (delay > CONFIG_APP_OBSERVE_MAX_BACKOFF_S) {
        delay = CONFIG_APP_OBSERVE_MAX_BACKOFF_S;
    }
    ESP_LOGW(TAG, "%s : %s -> nouvel essai dans %" PRIu32 " s", sub->peer.name, reason, delay);
    sub->state = SUB_WAIT_RETRY;
    sub->deadline_us = esp_timer_get_time() + S_TO_US(delay);
}

static void start_subscription(subscription_t *sub)
{
    ESP_LOGI(TAG, "Connexion CoAPS a %s (" IPSTR ":%u)...", sub->peer.name,
             IP2STR(&sub->peer.ip), sub->peer.port);
    sub->error = false;
    sub->need_fallback = false;
    sub->fallback_sent = false;
    sub->session = coap_client_open(s_ctx, &sub->peer, sub);
    if (!sub->session) {
        schedule_retry(sub, "ouverture de session impossible");
        return;
    }
    /* Requetes mises en file par libcoap et envoyees des la fin du handshake DTLS.
     * Par defaut une seule souscription par pair : /sensor/data contient toutes ses grandeurs.
     * Racine du mesh : /mesh/data contient les grandeurs de tous les noeuds. */
#if CONFIG_APP_MESH_ENABLE
    const char *paths = sub->peer.mesh_root ? CONFIG_APP_MESH_DATA_PATH : CONFIG_APP_OBSERVE_PATHS;
#else
    const char *paths = CONFIG_APP_OBSERVE_PATHS;
#endif
    bool ok = get_path_list(sub->session, paths, true) &&
              coap_client_get(sub->session, "/device/info", false);
    if (!ok) {
        schedule_retry(sub, "envoi des requetes impossible");
        return;
    }
    sub->state = SUB_CONNECTING;
    sub->deadline_us = esp_timer_get_time() + S_TO_US(CONFIG_APP_OBSERVE_CONNECT_TIMEOUT_S);
}

/* Autre abonnement vers la meme adresse (ex. racine vue a la fois par mDNS et par le mesh) */
static subscription_t *find_sub_by_addr(const peer_info_t *p)
{
    for (int i = 0; i < CONFIG_APP_MAX_PEERS; i++) {
        subscription_t *s = &s_subs[i];
        if (s->state != SUB_FREE && strcmp(s->peer.name, p->name) != 0 &&
            s->peer.ip.addr == p->ip.addr && s->peer.port == p->port) {
            return s;
        }
    }
    return NULL;
}

static void handle_peer_cmd(const cmd_t *cmd)
{
    subscription_t *sub = find_sub(cmd->peer.name);
    switch (cmd->peer_event) {
    case PEER_EVENT_ADDED:
    case PEER_EVENT_UPDATED: {
        subscription_t *dup = find_sub_by_addr(&cmd->peer);
        if (dup) {
            if (!cmd->peer.mesh_root) {
                ESP_LOGI(TAG, "%s = %s (meme adresse) : deja abonne", cmd->peer.name, dup->peer.name);
                return;
            }
            /* La racine du mesh remplace l'abonnement mDNS vers la meme carte */
            ESP_LOGI(TAG, "%s est la racine du mesh : abonnement remplace par %s",
                     dup->peer.name, CONFIG_APP_MESH_DATA_PATH);
            close_session(dup);
            dup->state = SUB_FREE;
        }
        if (!sub) {
            for (int i = 0; i < CONFIG_APP_MAX_PEERS && !sub; i++) {
                if (s_subs[i].state == SUB_FREE) {
                    sub = &s_subs[i];
                    memset(sub, 0, sizeof(*sub));
                }
            }
            if (!sub) {
                ESP_LOGW(TAG, "Plus de place pour s'abonner a %s", cmd->peer.name);
                return;
            }
        } else {
            close_session(sub);     /* adresse modifiee : on repart de zero */
        }
        sub->peer = cmd->peer;
        sub->fails = 0;
        start_subscription(sub);
        break;
    }

    case PEER_EVENT_REMOVED:
        if (sub) {
            ESP_LOGI(TAG, "%s a quitte le reseau : abonnement supprime", sub->peer.name);
            close_session(sub);
            sub->state = SUB_FREE;
        }
        break;
    }
}

static void tick(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < CONFIG_APP_MAX_PEERS; i++) {
        subscription_t *sub = &s_subs[i];
        if (sub->need_fallback && sub->session) {
            sub->need_fallback = false;
            if (!get_path_list(sub->session, CONFIG_APP_OBSERVE_FALLBACK_PATHS, true)) {
                on_error(sub, "envoi des requetes de repli impossible");
            }
        }
        switch (sub->state) {
        case SUB_FREE:
            break;
        case SUB_WAIT_RETRY:
            if (now >= sub->deadline_us) {
                start_subscription(sub);
            }
            break;
        case SUB_CONNECTING:
            if (sub->error) {
                schedule_retry(sub, sub->error_reason);
            } else if (now >= sub->deadline_us) {
                schedule_retry(sub, "delai depasse (handshake ou abonnement)");
            }
            break;
        case SUB_ACTIVE:
            if (sub->error) {
                schedule_retry(sub, sub->error_reason);
            } else if (now - sub->last_rx_us > S_TO_US(CONFIG_APP_OBSERVE_STALE_S)) {
                schedule_retry(sub, "plus de notification recue");
            }
            break;
        }
    }
}

static void log_status(void)
{
    int64_t now = esp_timer_get_time();
    int n = 0;
    ESP_LOGI(TAG, "---- Abonnements Observe (%s) ----", mdns_discovery_self_name());
    for (int i = 0; i < CONFIG_APP_MAX_PEERS; i++) {
        subscription_t *s = &s_subs[i];
        if (s->state == SUB_FREE) {
            continue;
        }
        n++;
        ESP_LOGI(TAG, "  %-14s " IPSTR " %-9s %-7s rx=%-5" PRIu32 " (il y a %llds) %s",
                 s->peer.name, IP2STR(&s->peer.ip), STATE_NAMES[s->state], s->sensor, s->rx_count,
                 s->last_rx_us ? (now - s->last_rx_us) / 1000000 : -1LL, s->last_values);
    }
    if (n == 0) {
        ESP_LOGI(TAG, "  (aucun pair pour l'instant)");
    }
    ESP_LOGI(TAG, "  Tas libre : %u octets", (unsigned)esp_get_free_heap_size());
}

/* Enfant du mesh : depose la nouvelle mesure chez la racine (PUT CoAPS sur la
 * session DTLS deja ouverte pour l'abonnement Observe). */
static void push_to_mesh_root(void)
{
#if CONFIG_APP_MESH_ENABLE
    for (int i = 0; i < CONFIG_APP_MAX_PEERS; i++) {
        subscription_t *sub = &s_subs[i];
        if (!sub->peer.mesh_root || sub->state != SUB_ACTIVE || !sub->session) {
            continue;
        }
        sensor_data_t d;
        sensor_manager_get(&d);
        char json[320];
        sensor_data_to_json(&d, mdns_discovery_self_name(), json, sizeof(json));
        if (coap_client_put_json(sub->session, CONFIG_APP_MESH_DATA_PATH, json)) {
            ESP_LOGI(TAG, ">> [MESH] PUT %s -> racine " IPSTR " (seq %" PRIu32 ")",
                     CONFIG_APP_MESH_DATA_PATH, IP2STR(&sub->peer.ip), d.seq);
        } else {
            on_error(sub, "envoi du PUT vers la racine impossible");
        }
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Tache CoAP                                                          */
/* ------------------------------------------------------------------ */

static void coap_task(void *arg)
{
    for (;;) {
        coap_io_process(s_ctx, 50);

        cmd_t cmd;
        while (xQueueReceive(s_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
            case CMD_PEER:   handle_peer_cmd(&cmd); break;
            case CMD_NOTIFY: coap_server_notify_sensors(); push_to_mesh_root(); break;
            case CMD_LOG:    log_status(); break;
            }
        }
        tick();
    }
}

esp_err_t coap_observe_start(coap_context_t *ctx)
{
    s_ctx = ctx;
    s_queue = xQueueCreate(16, sizeof(cmd_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    const coap_client_callbacks_t cbs = {
        .on_data = on_data,
        .on_error = on_error,
        .on_response_error = on_response_error,
    };
    coap_client_init(ctx, &cbs);
    return xTaskCreate(coap_task, "coap", 8192, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void coap_observe_on_peer_event(peer_event_t event, const peer_info_t *peer, void *ctx)
{
    cmd_t cmd = { .type = CMD_PEER, .peer_event = event, .peer = *peer };
    if (xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "File pleine : evenement de %s perdu", peer->name);
    }
}

void coap_observe_on_new_measure(const sensor_data_t *data)
{
    cmd_t cmd = { .type = CMD_NOTIFY };
    xQueueSend(s_queue, &cmd, 0);
}

void coap_observe_log_status(void)
{
    cmd_t cmd = { .type = CMD_LOG };
    xQueueSend(s_queue, &cmd, 0);
}
