#include "mesh_manager.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

#if CONFIG_APP_MESH_ENABLE
#include "esp_mesh_lite.h"

static const char *TAG = "mesh_manager";

static peer_event_cb_t s_cb;
static void *s_cb_ctx;
static volatile uint8_t s_level;
static esp_ip4_addr_t s_root_ip;     /* IP de la racine vue par cette carte (0 si racine ou deconnectee) */

static void emit(peer_event_t ev, esp_ip4_addr_t ip)
{
    peer_info_t p = { 0 };
    strlcpy(p.name, MESH_ROOT_PEER_NAME, sizeof(p.name));
    p.ip = ip;
    p.port = CONFIG_APP_COAPS_PORT;
    p.mesh_root = true;
    if (s_cb) {
        s_cb(ev, &p, s_cb_ctx);
    }
}

static void mesh_task(void *arg)
{
    for (;;) {
        uint8_t level = wifi_manager_is_connected() ? esp_mesh_lite_get_level() : 0;

        esp_ip4_addr_t root = { 0 };
        if (level > 1) {
            esp_ip_addr_t a = { 0 };
            if (esp_mesh_lite_get_root_ip(ESP_IPADDR_TYPE_V4, &a) == ESP_OK) {
                root.addr = a.u_addr.ip4.addr;
            }
        }

        if (level != s_level) {
            if (level == 0) {
                ESP_LOGW(TAG, "[MESH] hors du mesh (recherche d'un parent...)");
            } else if (level == 1) {
                ESP_LOGI(TAG, "[MESH] niveau 1 : cette carte est la RACINE (reliee a \"%s\")", CONFIG_APP_WIFI_SSID);
            } else {
                ESP_LOGI(TAG, "[MESH] niveau %u : cette carte est un ENFANT", level);
            }
            s_level = level;
        }

        if (root.addr != s_root_ip.addr) {
            if (s_root_ip.addr == 0) {
                ESP_LOGI(TAG, "[MESH] racine trouvee : " IPSTR " -> abonnement CoAPS", IP2STR(&root));
                emit(PEER_EVENT_ADDED, root);
            } else if (root.addr == 0) {
                ESP_LOGI(TAG, "[MESH] plus de racine a joindre (ancienne : " IPSTR ")", IP2STR(&s_root_ip));
                emit(PEER_EVENT_REMOVED, s_root_ip);
            } else {
                ESP_LOGI(TAG, "[MESH] la racine a change : " IPSTR, IP2STR(&root));
                emit(PEER_EVENT_UPDATED, root);
            }
            s_root_ip = root;
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_MESH_CHECK_INTERVAL_S * 1000));
    }
}

esp_err_t mesh_manager_start(peer_event_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
    return xTaskCreate(mesh_task, "mesh_mgr", 3072, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

uint8_t mesh_manager_level(void)
{
    return s_level;
}

bool mesh_manager_is_root(void)
{
    return s_level == 1;
}

void mesh_manager_log(void)
{
    esp_ip4_addr_t ip = wifi_manager_get_ip();
    if (s_level == 1) {
        ESP_LOGI(TAG, "[MESH] RACINE  mesh_id=%u  IP(box)=" IPSTR, esp_mesh_lite_get_mesh_id(), IP2STR(&ip));
    } else if (s_level > 1) {
        ESP_LOGI(TAG, "[MESH] ENFANT niveau %u  mesh_id=%u  IP=" IPSTR "  racine=" IPSTR,
                 s_level, esp_mesh_lite_get_mesh_id(), IP2STR(&ip), IP2STR(&s_root_ip));
    } else {
        ESP_LOGI(TAG, "[MESH] pas encore connecte au mesh");
    }
}

#else /* !CONFIG_APP_MESH_ENABLE */

esp_err_t mesh_manager_start(peer_event_cb_t cb, void *ctx) { return ESP_OK; }
uint8_t mesh_manager_level(void) { return 0; }
bool mesh_manager_is_root(void) { return false; }
void mesh_manager_log(void) { }

#endif
