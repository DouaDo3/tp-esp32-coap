/*
 * TP CoAPS multi-ESP32 : CoAP securise (DTLS-PSK, UDP 5684) + Observe + ESP-Mesh-Lite + MQTT
 *
 * Ordre de demarrage :
 *   LED -> securite -> capteur -> Wi-Fi / Mesh-Lite (IP par DHCP) -> serveur CoAPS
 *   -> tache Observe -> mesh_manager (racine du mesh) -> mDNS -> MQTT -> capteur
 *
 * Reseau maille : la carte reliee a la box est la racine ; les enfants s'abonnent
 * (GET + Observe) a /mesh/data sur la racine et y deposent leurs mesures (PUT).
 * Aucune adresse IP n'est codee en dur : la racine vient de Mesh-Lite, les autres pairs de mDNS.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "actuator_manager.h"
#include "coap_observe.h"
#include "coap_server.h"
#include "led_manager.h"
#include "mdns_discovery.h"
#include "mesh_manager.h"
#include "mqtt_manager.h"
#include "security_manager.h"
#include "sensor_manager.h"
#include "wifi_manager.h"

static const char *TAG = "main";

/* Chaque nouvelle mesure part en CoAP (Observe + PUT vers la racine du mesh)
 * et en MQTT (broker -> Node-RED -> page HTML / MongoDB). */
static void on_new_measure(const sensor_data_t *data)
{
    coap_observe_on_new_measure(data);
    mqtt_manager_publish_measure(data);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Demarrage de la carte %s", mdns_discovery_self_name());

    ESP_ERROR_CHECK(led_manager_init());
    ESP_ERROR_CHECK(actuator_manager_init());   /* commande par MQTT tp/<id>/cmd */
    ESP_ERROR_CHECK(security_manager_init());
    ESP_ERROR_CHECK(sensor_manager_init());     /* avant le serveur : ses grandeurs definissent les ressources */

    ESP_ERROR_CHECK(wifi_manager_start());
    while (!wifi_manager_wait_connected(pdMS_TO_TICKS(10000))) {
        ESP_LOGW(TAG, "Toujours en attente du Wi-Fi \"%s\" (ou d'un parent du mesh)...", CONFIG_APP_WIFI_SSID);
    }

    coap_context_t *ctx = coap_server_create();
    if (!ctx) {
        ESP_LOGE(TAG, "Serveur CoAPS indisponible, redemarrage");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    ESP_ERROR_CHECK(coap_observe_start(ctx));

    /* Enfant du mesh : la racine devient un pair observe (et destinataire des PUT) */
    ESP_ERROR_CHECK(mesh_manager_start(coap_observe_on_peer_event, NULL));

    mdns_discovery_set_callback(coap_observe_on_peer_event, NULL);
    ESP_ERROR_CHECK(mdns_discovery_start(CONFIG_APP_COAPS_PORT));

    /* MQTT : connexion TCP au broker (passe a travers le mesh), reconnexion automatique */
    ESP_ERROR_CHECK(mqtt_manager_start());

    ESP_ERROR_CHECK(sensor_manager_start(on_new_measure));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        mesh_manager_log();
        mqtt_manager_log();
        mdns_discovery_log_peers();
        coap_observe_log_status();
    }
}
