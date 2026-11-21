#pragma once
/*
 * actuator_manager : actionneur de la carte, commande depuis l'application
 * (page HTML -> WebSocket -> Node-RED -> MQTT tp/<id>/cmd -> cette carte).
 *
 *  Sortie numerique ON/OFF sur APP_ACTUATOR_GPIO : LED externe, relais,
 *  buzzer actif... (meme code, seul le branchement change).
 */
#include <stdbool.h>
#include "esp_err.h"

esp_err_t actuator_manager_init(void);

/* Applique "on" / "off" / "toggle" (aussi "1" / "0"). false si commande inconnue. */
bool actuator_manager_command(const char *cmd);

bool actuator_manager_get(void);

/* Nom lisible de l'actionneur (menuconfig), ex. "LED" */
const char *actuator_manager_name(void);
