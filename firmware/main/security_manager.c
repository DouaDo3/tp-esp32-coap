#include "security_manager.h"

#include <string.h>
#include "esp_log.h"
#include "mdns_discovery.h"

static const char *TAG = "security";

static coap_bin_const_t s_key;
static coap_bin_const_t s_hint;
static coap_bin_const_t s_identity;
static char s_prefix[24];

/* Appelee par libcoap pendant le handshake DTLS, quand un client presente son identite. */
static const coap_bin_const_t *validate_identity(coap_bin_const_t *identity,
                                                 coap_session_t *session, void *arg)
{
    (void)session;
    (void)arg;
    bool bad = identity == NULL || identity->length == 0 || identity->length >= 64;
#ifdef CONFIG_APP_PSK_REQUIRE_PREFIX
    size_t plen = strlen(s_prefix);
    bad = bad || identity->length <= plen || memcmp(identity->s, s_prefix, plen) != 0;
#endif
    if (bad) {
        ESP_LOGW(TAG, "Identite PSK refusee : \"%.*s\"",
                 identity ? (int)identity->length : 0, identity ? (const char *)identity->s : "");
        return NULL;    /* -> handshake interrompu */
    }
    ESP_LOGI(TAG, "Identite PSK acceptee : %.*s (cle verifiee a la fin du handshake)",
             (int)identity->length, (const char *)identity->s);
    return &s_key;
}

esp_err_t security_manager_init(void)
{
    const char *key = CONFIG_APP_PSK_KEY;
    if (strlen(key) == 0) {
        ESP_LOGE(TAG, "APP_PSK_KEY vide");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(key) < 8) {
        ESP_LOGW(TAG, "APP_PSK_KEY courte (%u caracteres) : acceptee pour l'interop, mais peu robuste",
                 (unsigned)strlen(key));
    }
    s_key.s = (const uint8_t *)key;
    s_key.length = strlen(key);
    s_hint.s = (const uint8_t *)CONFIG_APP_PSK_HINT;
    s_hint.length = strlen(CONFIG_APP_PSK_HINT);

    const char *self = mdns_discovery_self_name();
    s_identity.s = (const uint8_t *)self;
    s_identity.length = strlen(self);
    snprintf(s_prefix, sizeof(s_prefix), "%s-", CONFIG_APP_DEVICE_PREFIX);

    if (!coap_dtls_is_supported() || !coap_dtls_psk_is_supported()) {
        ESP_LOGE(TAG, "libcoap compile sans DTLS/PSK : verifier sdkconfig.defaults");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "DTLS-PSK pret : identite=%s, cle de %u octets", self, (unsigned)s_key.length);
    return ESP_OK;
}

esp_err_t security_manager_setup_server(coap_context_t *ctx)
{
    coap_dtls_spsk_t spsk;
    memset(&spsk, 0, sizeof(spsk));
    spsk.version = COAP_DTLS_SPSK_SETUP_VERSION;
    spsk.validate_id_call_back = validate_identity;
    spsk.psk_info.hint = s_hint;
    spsk.psk_info.key = s_key;
    return coap_context_set_psk2(ctx, &spsk) ? ESP_OK : ESP_FAIL;
}

void security_manager_fill_client(coap_dtls_cpsk_t *setup)
{
    memset(setup, 0, sizeof(*setup));
    setup->version = COAP_DTLS_CPSK_SETUP_VERSION;
    setup->psk_info.identity = s_identity;
    setup->psk_info.key = s_key;
}
