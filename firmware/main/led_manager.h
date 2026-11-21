#pragma once
/*
 * led_manager : LED de reception.
 *  - eteinte au demarrage
 *  - led_manager_pulse() l'allume APP_LED_ON_MS ms puis l'eteint automatiquement
 *  - un nouvel appel pendant l'allumage relance la duree (retriggerable)
 *  - appelable depuis n'importe quelle tache (non bloquant)
 */
#include "esp_err.h"

esp_err_t led_manager_init(void);
void led_manager_pulse(void);
