#include "wifi_manager.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#if CONFIG_APP_MESH_ENABLE
#include "esp_bridge.h"
#include "esp_mesh_lite.h"
#endif

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_netif;
static esp_ip4_addr_t s_ip;

#if !CONFIG_APP_MESH_ENABLE
static TimerHandle_t s_retry_timer;
static uint32_t s_backoff_s = 1;

static void retry_timer_cb(TimerHandle_t t)
{
    ESP_LOGI(TAG, "Nouvelle tentative de connexion a \"%s\"...", CONFIG_APP_WIFI_SSID);
    esp_wifi_connect();
}
#endif

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
#if !CONFIG_APP_MESH_ENABLE
        ESP_LOGI(TAG, "Connexion a \"%s\"...", CONFIG_APP_WIFI_SSID);
        esp_wifi_connect();
#endif
        /* En mode mesh, Mesh-Lite choisit lui-meme le parent (box ou autre noeud). */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        s_ip.addr = 0;
#if CONFIG_APP_MESH_ENABLE
        ESP_LOGW(TAG, "Deconnecte du parent (raison %d) : Mesh-Lite cherche un nouveau parent", ev->reason);
#else
        ESP_LOGW(TAG, "Deconnecte (raison %d), reessai dans %" PRIu32 " s", ev->reason, s_backoff_s);
        /* Reconnexion differee via un timer pour ne pas bloquer la tache d'evenements */
        xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_backoff_s * 1000), 0);
        xTimerStart(s_retry_timer, 0);
        s_backoff_s *= 2;
        if (s_backoff_s > CONFIG_APP_WIFI_MAX_BACKOFF_S) {
            s_backoff_s = CONFIG_APP_WIFI_MAX_BACKOFF_S;
        }
#endif
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_ip = ev->ip_info.ip;
#if CONFIG_APP_MESH_ENABLE
        ESP_LOGI(TAG, "Connecte au mesh ! IP obtenue par DHCP : " IPSTR " (passerelle " IPSTR ")",
                 IP2STR(&s_ip), IP2STR(&ev->ip_info.gw));
#else
        s_backoff_s = 1;
        ESP_LOGI(TAG, "Connecte ! IP obtenue par DHCP : " IPSTR, IP2STR(&s_ip));
#endif
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void fill_sta_config(wifi_config_t *wcfg)
{
    memset(wcfg, 0, sizeof(*wcfg));
    strlcpy((char *)wcfg->sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wcfg->sta.ssid));
    strlcpy((char *)wcfg->sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wcfg->sta.password));
    /* Accepte les reseaux ouverts si aucun mot de passe n'est fourni */
    wcfg->sta.threshold.authmode = strlen(CONFIG_APP_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}

esp_err_t wifi_manager_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_APP_MESH_ENABLE
    /* iot_bridge : initialise le Wi-Fi, cree l'interface STA (vers le parent)
     * et l'interface SoftAP (vers les enfants) avec DHCP + NAT entre les deux. */
    esp_bridge_create_all_netif();
    s_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL));

    /* Station : la box (utilisee seulement par la carte qui deviendra racine) */
    wifi_config_t sta;
    fill_sta_config(&sta);
    ESP_ERROR_CHECK(esp_bridge_wifi_set_config(WIFI_IF_STA, &sta));

    /* SoftAP : reseau du mesh, identique sur toutes les cartes */
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, CONFIG_BRIDGE_SOFTAP_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(ap.ap.password));
    ESP_ERROR_CHECK(esp_bridge_wifi_set_config(WIFI_IF_AP, &ap));

    esp_mesh_lite_config_t mesh_cfg = ESP_MESH_LITE_DEFAULT_INIT();
    esp_mesh_lite_init(&mesh_cfg);
    esp_mesh_lite_set_softap_info(CONFIG_BRIDGE_SOFTAP_SSID, CONFIG_BRIDGE_SOFTAP_PASSWORD);
    esp_mesh_lite_start();
    ESP_LOGI(TAG, "Mesh-Lite demarre : box \"%s\", reseau mesh \"%s\", mesh_id %d",
             CONFIG_APP_WIFI_SSID, CONFIG_BRIDGE_SOFTAP_SSID, CONFIG_MESH_LITE_ID);
#else
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(1000), pdFALSE, NULL, retry_timer_cb);
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL));

    wifi_config_t wcfg;
    fill_sta_config(&wcfg);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
#endif

    /* Economie d'energie desactivee : en mode modem-sleep la carte rate une partie
     * des paquets multicast, ce qui rend la decouverte mDNS peu fiable. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    return ESP_OK;
}

bool wifi_manager_wait_connected(TickType_t timeout)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_ip4_addr_t wifi_manager_get_ip(void)
{
    return s_ip;
}

esp_netif_t *wifi_manager_get_netif(void)
{
    return s_netif;
}
