idf_component_register(
    SRCS "main.c"
         "wifi_manager.c"
         "mesh_manager.c"
         "mqtt_manager.c"
         "actuator_manager.c"
         "mdns_discovery.c"
         "security_manager.c"
         "coap_server.c"
         "coap_client.c"
         "coap_observe.c"
         "led_manager.c"
         "sensor_manager.c"
         # Pilotes de capteur : un seul est actif selon menuconfig (#if dans chaque fichier)
         "sensor_sim.c"
         "sensor_bmp280.c"
         "sensor_dht.c"
         "sensor_pir.c"
         "sensor_analog.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer json
                  esp_driver_gpio esp_driver_i2c esp_adc
                  mesh_lite iot_bridge mqtt
)
