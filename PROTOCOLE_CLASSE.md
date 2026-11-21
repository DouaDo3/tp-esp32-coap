# Protocole commun - échange de données entre nos ESP32

Chacun garde **son propre code**. Pour que nos cartes puissent quand même échanger
leurs mesures, on doit tous respecter **les mêmes 5 règles** ci-dessous.
(Comme un navigateur et un site web : codes différents, mais même protocole HTTP.)

> À valider ensemble AVANT de coder / au début de la séance.
> Si on change une valeur, tout le monde la change.

## Règle 1 - Réseau
- Même Wi-Fi **2,4 GHz** pour toutes les cartes (de préférence le partage de
  connexion d'un téléphone : le Wi-Fi de l'école bloque souvent le multicast).
- IP obtenue par DHCP. **Personne ne saisit d'adresse IP.**

## Règle 2 - Découverte automatique : mDNS (composant `espressif/mdns`)
Chaque carte **annonce** un service et **cherche** ce même service :

| Élément | Valeur commune |
|---|---|
| Type de service | `_coaps`, protocole `_udp` → `_coaps._udp.local` |
| Nom d'instance | unique par carte, ex. `esp32-a1b2c3` |
| Port | `5684` |

```c
mdns_init();
mdns_hostname_set("esp32-a1b2c3");
mdns_service_add("esp32-a1b2c3", "_coaps", "_udp", 5684, NULL, 0);
// recherche (à répéter toutes les ~10 s) :
mdns_query_ptr("_coaps", "_udp", 3000, 20, &results);  // -> IP + port de chaque carte
```

## Règle 3 - Sécurité : CoAP sur DTLS, mode PSK, UDP 5684
| Élément | Valeur commune |
|---|---|
| Clé PSK | `tp-coaps-cle-de-groupe-2026` (identique pour tous) |
| Identité PSK (client) | libre, ex. le nom de la carte |
| Port | `5684`, uniquement DTLS (pas de CoAP en clair sur 5683) |

Options sdkconfig utiles (libcoap `espressif/coap` + mbedTLS) :
```
CONFIG_MBEDTLS_SSL_PROTO_DTLS=y
CONFIG_MBEDTLS_PSK_MODES=y
CONFIG_MBEDTLS_KEY_EXCHANGE_PSK=y
CONFIG_COAP_MBEDTLS_PSK=y
CONFIG_COAP_SERVER_SUPPORT=y
CONFIG_COAP_CLIENT_SUPPORT=y
```

## Règle 4 - Ressource à publier (observable)
Chaque carte publie **au minimum** :

| Ressource | Méthode | Rôle |
|---|---|---|
| `/sensor/data` | GET + **Observe** | toutes les mesures de la carte |

Recommandé en plus : `/sensor/temperature`, `/sensor/humidity` (une grandeur
chacune), `/device/info`.

Le serveur doit envoyer une **notification Observe à chaque nouvelle mesure**
(libcoap : `coap_resource_set_get_observable(res, 1)` puis
`coap_resource_notify_observers(res, NULL)` après chaque mesure).

## Règle 5 - Format des données : JSON
```json
{"device_id":"esp32-a1b2c3","sensor":"DHT11","temperature":24.1,"humidity":55}
```
- `device_id` : nom de la carte (obligatoire)
- `sensor` : modèle du capteur (facultatif)
- puis une clé numérique par grandeur mesurée. Noms conseillés :
  `temperature` (°C), `humidity` (%), `pressure` (hPa), `motion` (0/1),
  `air_quality` (%), `light` (%), `distance` (cm)

## Côté réception (client)
1. Pour chaque carte trouvée par mDNS : ouvrir une session DTLS-PSK vers IP:5684.
2. Envoyer `GET /sensor/data` avec l'option **Observe = 0** (abonnement).
3. À chaque notification reçue : lire le JSON et **allumer la LED** ~500 ms.
4. Si une carte disparaît de mDNS ou ne notifie plus : fermer la session.

## Test rapide entre deux cartes
- Les deux logs doivent montrer la carte de l'autre découverte (IP + port 5684),
  puis des données reçues toutes les quelques secondes, et la LED qui clignote.
- Avec une clé PSK différente : la carte est découverte mais **aucune donnée ne
  passe** (échec du handshake DTLS) → preuve que la communication est sécurisée.
