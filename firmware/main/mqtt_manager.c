#include "mqtt_manager.h"

#if CONFIG_APP_MQTT_ENABLE

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "actuator_manager.h"
#include "mdns_discovery.h"
#include "mesh_manager.h"
#include "wifi_manager.h"

static const char *TAG = "mqtt_manager";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static uint32_t s_published;
static char s_topic_data[64];
static char s_topic_status[64];
static char s_topic_cmd[64];
static char s_topic_actuator[64];
static char s_lwt[96];

static void publish_status_online(void)
{
    sensor_data_t d;
    sensor_manager_get(&d);
    esp_ip4_addr_t ip = wifi_manager_get_ip();
    char json[192];
    snprintf(json, sizeof(json),
             "{\"device_id\":\"%s\",\"state\":\"online\",\"ip\":\"" IPSTR "\",\"mesh_level\":%u,\"sensor\":\"%s\"}",
             mdns_discovery_self_name(), IP2STR(&ip), mesh_manager_level(), d.model);
    esp_mqtt_client_publish(s_client, s_topic_status, json, 0, 1, 1);   /* QoS 1, retained */
}

/* Etat de l'actionneur, retained : la page le connait meme si elle s'ouvre apres */
static void publish_actuator_state(void)
{
    char json[128];
    snprintf(json, sizeof(json), "{\"device_id\":\"%s\",\"actuator\":\"%s\",\"state\":\"%s\"}",
             mdns_discovery_self_name(), actuator_manager_name(), actuator_manager_get() ? "on" : "off");
    esp_mqtt_client_publish(s_client, s_topic_actuator, json, 0, 1, 1);
}

/* tp/<id>/cmd : {"state":"on"|"off"|"toggle"} ou simplement on / off / toggle */
static void handle_command(const char *data, int len)
{
    char cmd[16] = "";
    cJSON *root = cJSON_ParseWithLength(data, len);
    const cJSON *state = cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "state") : NULL;
    if (cJSON_IsString(state)) {
        strlcpy(cmd, state->valuestring, sizeof(cmd));
    } else if (cJSON_IsString(root)) {
        strlcpy(cmd, root->valuestring, sizeof(cmd));        /* "on" entre guillemets */
    } else if (!root && len > 0 && len < (int)sizeof(cmd)) {
        memcpy(cmd, data, len);                              /* texte brut : on */
        cmd[len] = '\0';
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "<< [MQTT] commande recue sur %s : %.*s", s_topic_cmd, len, data);
    if (cmd[0] && actuator_manager_command(cmd)) {
        publish_actuator_state();
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_mqtt_event_handle_t ev = event_data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "[MQTT] connecte au broker %s", CONFIG_APP_MQTT_BROKER_URI);
        publish_status_online();
        /* 2e sens : application -> carte */
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        ESP_LOGI(TAG, "[MQTT] abonne a %s (commandes de l'actionneur)", s_topic_cmd);
        publish_actuator_state();
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len == (int)strlen(s_topic_cmd) && strncmp(ev->topic, s_topic_cmd, ev->topic_len) == 0 &&
            ev->current_data_offset == 0 && ev->data_len == ev->total_data_len) {
            handle_command(ev->data, ev->data_len);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            ESP_LOGW(TAG, "[MQTT] deconnecte du broker (reconnexion automatique)");
        }
        s_connected = false;
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "[MQTT] broker injoignable (%s) : verifier l'IP du PC et que Mosquitto ecoute",
                     CONFIG_APP_MQTT_BROKER_URI);
        } else if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "[MQTT] connexion refusee par le broker (code %d)", ev->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

esp_err_t mqtt_manager_start(void)
{
    const char *id = mdns_discovery_self_name();
    snprintf(s_topic_data, sizeof(s_topic_data), "%s/%s/data", CONFIG_APP_MQTT_TOPIC_PREFIX, id);
    snprintf(s_topic_status, sizeof(s_topic_status), "%s/%s/status", CONFIG_APP_MQTT_TOPIC_PREFIX, id);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "%s/%s/cmd", CONFIG_APP_MQTT_TOPIC_PREFIX, id);
    snprintf(s_topic_actuator, sizeof(s_topic_actuator), "%s/%s/actuator", CONFIG_APP_MQTT_TOPIC_PREFIX, id);
    snprintf(s_lwt, sizeof(s_lwt), "{\"device_id\":\"%s\",\"state\":\"offline\"}", id);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_APP_MQTT_BROKER_URI,
        .credentials.client_id = id,
        .credentials.username = CONFIG_APP_MQTT_USERNAME[0] ? CONFIG_APP_MQTT_USERNAME : NULL,
        .credentials.authentication.password = CONFIG_APP_MQTT_PASSWORD[0] ? CONFIG_APP_MQTT_PASSWORD : NULL,
        .session.keepalive = 30,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = s_lwt,
            .qos = 1,
            .retain = 1,
        },
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_LOGI(TAG, "[MQTT] connexion a %s, publication sur %s", CONFIG_APP_MQTT_BROKER_URI, s_topic_data);
    return esp_mqtt_client_start(s_client);
}

void mqtt_manager_publish_measure(const sensor_data_t *data)
{
    if (!s_connected) {
        return;
    }
    char json[320];
    int len = sensor_data_to_json(data, mdns_discovery_self_name(), json, sizeof(json));
    if (len <= 0 || len >= (int)sizeof(json)) {
        return;
    }
    if (esp_mqtt_client_publish(s_client, s_topic_data, json, len, 0, 0) >= 0) {
        s_published++;
        ESP_LOGI(TAG, ">> [MQTT] %s (seq %" PRIu32 ")", s_topic_data, data->seq);
    }
}

bool mqtt_manager_is_connected(void)
{
    return s_connected;
}

void mqtt_manager_log(void)
{
    ESP_LOGI(TAG, "[MQTT] %s %s, %" PRIu32 " mesure(s) publiee(s)",
             s_connected ? "CONNECTE a" : "en attente de", CONFIG_APP_MQTT_BROKER_URI, s_published);
}

#else /* !CONFIG_APP_MQTT_ENABLE */

esp_err_t mqtt_manager_start(void) { return ESP_OK; }
void mqtt_manager_publish_measure(const sensor_data_t *data) { }
bool mqtt_manager_is_connected(void) { return false; }
void mqtt_manager_log(void) { }

#endif
