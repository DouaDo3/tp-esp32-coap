#include "mdns_discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "wifi_manager.h"

static const char *TAG = "mdns_discovery";

static char s_self_name[PEER_NAME_LEN];
static peer_info_t s_peers[CONFIG_APP_MAX_PEERS];
static bool s_used[CONFIG_APP_MAX_PEERS];
static SemaphoreHandle_t s_lock;
static peer_event_cb_t s_cb;
static void *s_cb_ctx;

/* Evenements collectes sous verrou puis notifies hors verrou */
typedef struct {
    peer_event_t ev;
    peer_info_t peer;
} pending_event_t;

const char *mdns_discovery_self_name(void)
{
    if (s_self_name[0] == '\0') {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_self_name, sizeof(s_self_name), "%s-%02x%02x%02x",
                 CONFIG_APP_DEVICE_PREFIX, mac[3], mac[4], mac[5]);
    }
    return s_self_name;
}

void mdns_discovery_set_callback(peer_event_cb_t cb, void *ctx)
{
    s_cb_ctx = ctx;
    s_cb = cb;
}

static int find_peer(const char *name)
{
    for (int i = 0; i < CONFIG_APP_MAX_PEERS; i++) {
        if (s_used[i] && strcmp(s_peers[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void fire(const pending_event_t *evs, int n)
{
    static const char *names[] = { "AJOUT", "MAJ", "RETRAIT" };
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "[%s] %s -> " IPSTR ":%u", names[evs[i].ev], evs[i].peer.name,
                 IP2STR(&evs[i].peer.ip), evs[i].peer.port);
        if (s_cb) {
            s_cb(evs[i].ev, &evs[i].peer, s_cb_ctx);
        }
    }
}

/* Enregistre/rafraichit un pair. Retourne true si un evenement a ete produit. */
static bool upsert_peer(const char *name, esp_ip4_addr_t ip, uint16_t port, pending_event_t *out)
{
    int64_t now = esp_timer_get_time();
    int idx = find_peer(name);
    if (idx >= 0) {
        peer_info_t *p = &s_peers[idx];
        p->last_seen_us = now;
        if (p->ip.addr != ip.addr || p->port != port) {
            p->ip = ip;
            p->port = port;
            out->ev = PEER_EVENT_UPDATED;
            out->peer = *p;
            return true;
        }
        return false;
    }
    for (int i = 0; i < CONFIG_APP_MAX_PEERS; i++) {
        if (!s_used[i]) {
            s_used[i] = true;
            peer_info_t *p = &s_peers[i];
            strlcpy(p->name, name, sizeof(p->name));
            p->ip = ip;
            p->port = port;
            p->last_seen_us = now;
            out->ev = PEER_EVENT_ADDED;
            out->peer = *p;
            return true;
        }
    }
    ESP_LOGW(TAG, "Table des pairs pleine (%d), %s ignore (augmenter APP_MAX_PEERS)",
             CONFIG_APP_MAX_PEERS, name);
    return false;
}

/* Extrait la premiere adresse IPv4 d'un resultat ; si absente, resout le hostname. */
static bool result_ipv4(const mdns_result_t *r, esp_ip4_addr_t *ip)
{
    for (const mdns_ip_addr_t *a = r->addr; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4 && a->addr.u_addr.ip4.addr != 0) {
            *ip = a->addr.u_addr.ip4;
            return true;
        }
    }
    if (r->hostname && mdns_query_a(r->hostname, 1500, ip) == ESP_OK && ip->addr != 0) {
        return true;
    }
    return false;
}

/* Pairs declares a la main (APP_STATIC_PEERS) : rafraichis a chaque tour,
 * donc jamais expires. Ignores si la meme IP est deja connue via mDNS. */
static void add_static_peers(pending_event_t *evs, int *n, int max)
{
    char list[] = CONFIG_APP_STATIC_PEERS;
    char *save = NULL;
    for (char *tok = strtok_r(list, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        uint16_t port = CONFIG_APP_COAPS_PORT;
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = '\0';
            port = (uint16_t)atoi(colon + 1);
        }
        esp_ip4_addr_t ip;
        ip.addr = esp_ip4addr_aton(tok);
        if (ip.addr == 0 || ip.addr == IPADDR_NONE) {
            ESP_LOGW(TAG, "APP_STATIC_PEERS : adresse invalide '%s'", tok);
            continue;
        }
        char name[PEER_NAME_LEN];
        snprintf(name, sizeof(name), "ip-%s", tok);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool known_by_mdns = false;
        for (int i = 0; i < CONFIG_APP_MAX_PEERS; i++) {
            if (s_used[i] && s_peers[i].ip.addr == ip.addr && strcmp(s_peers[i].name, name) != 0) {
                known_by_mdns = true;
            }
        }
        if (!known_by_mdns && *n < max && upsert_peer(name, ip, port, &evs[*n])) {
            (*n)++;
        }
        xSemaphoreGive(s_lock);
    }
}

static void discovery_round(void)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO,
                                   CONFIG_APP_DISCOVERY_QUERY_TIMEOUT_MS,
                                   CONFIG_APP_MAX_PEERS + 4, &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Echec requete mDNS : %s", esp_err_to_name(err));
        return;
    }

    pending_event_t evs[CONFIG_APP_MAX_PEERS * 2];
    int n = 0;

    for (mdns_result_t *r = results; r; r = r->next) {
        if (!r->instance_name || strcmp(r->instance_name, s_self_name) == 0) {
            continue;   /* sa propre annonce */
        }
        esp_ip4_addr_t ip = { 0 };
        if (!result_ipv4(r, &ip)) {
            ESP_LOGD(TAG, "%s : pas d'adresse IPv4 pour l'instant", r->instance_name);
            continue;
        }
        uint16_t port = r->port ? r->port : CONFIG_APP_COAPS_PORT;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (n < (int)(sizeof(evs) / sizeof(evs[0])) && upsert_peer(r->instance_name, ip, port, &evs[n])) {
            n++;
        }
        xSemaphoreGive(s_lock);
    }
    mdns_query_results_free(results);

    add_static_peers(evs, &n, (int)(sizeof(evs) / sizeof(evs[0])));

    /* Expiration : pairs qui n'ont plus repondu depuis APP_PEER_TIMEOUT_S */
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONFIG_APP_MAX_PEERS; i++) {
        if (s_used[i] && now - s_peers[i].last_seen_us > (int64_t)CONFIG_APP_PEER_TIMEOUT_S * 1000000) {
            if (n < (int)(sizeof(evs) / sizeof(evs[0]))) {
                evs[n].ev = PEER_EVENT_REMOVED;
                evs[n].peer = s_peers[i];
                n++;
            }
            s_used[i] = false;
        }
    }
    xSemaphoreGive(s_lock);

    fire(evs, n);
}

static void discovery_task(void *arg)
{
    /* Petit delai aleatoire : evite que toutes les cartes allumees en meme temps
     * interrogent le reseau au meme instant. */
    vTaskDelay(pdMS_TO_TICKS(500 + (esp_random() % 1500)));
    for (;;) {
        if (wifi_manager_is_connected()) {
            discovery_round();
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_DISCOVERY_INTERVAL_S * 1000));
    }
}

esp_err_t mdns_discovery_start(uint16_t coaps_port)
{
    const char *name = mdns_discovery_self_name();
    s_lock = xSemaphoreCreateMutex();

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(name), TAG, "hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(name), TAG, "instance");

    mdns_txt_item_t txt[] = {
        { "id",  name },
        { "ver", "1" },
        { "res", "/sensor/data,/device/info" },
        { "sec", "dtls-psk" },
    };
    ESP_RETURN_ON_ERROR(mdns_service_add(name, MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO, coaps_port,
                                         txt, sizeof(txt) / sizeof(txt[0])),
                        TAG, "service_add");

    ESP_LOGI(TAG, "Annonce : %s.local, service %s.%s.%s.local port %u",
             name, name, MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO, coaps_port);

    BaseType_t ok = xTaskCreate(discovery_task, "mdns_disc", 4096, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

int mdns_discovery_get_peers(peer_info_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONFIG_APP_MAX_PEERS && n < max; i++) {
        if (s_used[i]) {
            out[n++] = s_peers[i];
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

void mdns_discovery_forget_peer(const char *name)
{
    pending_event_t ev;
    bool removed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_peer(name);
    if (idx >= 0) {
        ev.ev = PEER_EVENT_REMOVED;
        ev.peer = s_peers[idx];
        s_used[idx] = false;
        removed = true;
    }
    xSemaphoreGive(s_lock);
    if (removed) {
        fire(&ev, 1);
    }
}

void mdns_discovery_log_peers(void)
{
    peer_info_t peers[CONFIG_APP_MAX_PEERS];
    int n = mdns_discovery_get_peers(peers, CONFIG_APP_MAX_PEERS);
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "---- %s : %d pair(s) decouvert(s) ----", s_self_name, n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  %-16s " IPSTR ":%u  (vu il y a %lld s)", peers[i].name,
                 IP2STR(&peers[i].ip), peers[i].port, (now - peers[i].last_seen_us) / 1000000);
    }
}
