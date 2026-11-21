# TP ESP32 : CoAP/DTLS + ESP-Mesh-Lite + MQTT + Node-RED + MongoDB

Chaque carte ESP32 mesure (capteur) et commande un actionneur (LED sur D2).

- **Entre les cartes** : CoAPS (CoAP sur DTLS-PSK, port 5684) + Observe (RFC 7641), réseau maillé ESP-Mesh-Lite.
- **Sens 1** : carte → MQTT → Mosquitto → Node-RED → page web temps réel (WebSocket) + MongoDB Atlas.
- **Sens 2** : page web → Node-RED → MQTT `tp/<carte>/cmd` → la carte allume ou éteint son actionneur.

Le PC fait tourner **Mosquitto** et **Node-RED** dans **Docker**. Les cartes reçoivent un **firmware déjà compilé** : pas besoin d'installer ESP-IDF.

---

## 0. Ce qu'il faut

- Un PC **Windows 10 ou 11** avec le **Wi-Fi**
- Une carte **ESP32** + un câble USB **de données** (certains câbles servent seulement à charger)
- **Google Chrome** ou **Microsoft Edge**
- Facultatif : un capteur (BMP280, DHT11, DHT22, PIR, MQ135...) et une LED + une résistance de 220 Ω

## 1. Installer Docker Desktop (une seule fois)

1. Télécharger : <https://www.docker.com/products/docker-desktop/>
2. Installer avec les options par défaut (**WSL 2** coché), puis **redémarrer le PC**.
3. Ouvrir **Docker Desktop** et attendre l'indication verte **« Engine running »** en bas à gauche.

> Erreur « virtualization » : activer la virtualisation (VT-x / AMD-V / SVM) dans le BIOS.

## 2. Télécharger le projet

- Bouton vert **Code → Download ZIP**, puis extraire, **ou** `git clone` du dépôt.
- Mettre le dossier dans un **chemin court**, par exemple `C:\tp-esp32-coap`. Éviter le Bureau et OneDrive.

## 3. MongoDB Atlas (facultatif : sans Atlas, tout le reste fonctionne)

1. Créer un compte gratuit : <https://cloud.mongodb.com>
2. Créer un cluster **gratuit (M0)**.
3. **Database Access → Add New Database User** : choisir un nom et un mot de passe (sans caractères spéciaux, c'est plus simple).
4. **Network Access → Add IP Address → Allow access from anywhere** (`0.0.0.0/0`) → Confirm.
5. **Connect → Drivers** : copier le lien `mongodb+srv://...`.
6. Dans le dossier du projet : **copier `.env.example` et renommer la copie en `.env`**. L'ouvrir avec le Bloc-notes et coller le lien en remplaçant `<db_password>` par le mot de passe :
   ```
   MONGODB_URI=mongodb+srv://utilisateur:motdepasse@cluster0.xxxxx.mongodb.net/
   MONGODB_DB=iot_tp
   ```

> Dans l'Explorateur Windows, activer **Affichage → Extensions de noms de fichiers** pour bien voir `.env` (et non `.env.txt`).

## 4. Lancer Mosquitto et Node-RED

Dans le dossier du projet : clic droit dans le vide → **Ouvrir dans le Terminal**, puis :

```
docker compose up -d
```

La première fois, comptez 2 à 5 minutes. Vérifier :

- <http://localhost:1880> → éditeur Node-RED (onglet **TP ESP32**)
- <http://localhost:1880/page2> → la page du TP (encore vide)

| Commande | Effet |
|---|---|
| `docker compose ps` | voir si les 2 conteneurs tournent |
| `docker compose logs -f nodered` | voir les messages de Node-RED (Ctrl+C pour quitter) |
| `docker compose restart nodered` | redémarrer Node-RED (après une modification de `.env`) |
| `docker compose down` | tout arrêter |

> Les ports **1883** et **1880** doivent être libres. Si Mosquitto ou Node-RED sont déjà installés sur le PC, les arrêter d'abord (`Stop-Service mosquitto` en administrateur, et fermer Node-RED).

## 5. Ouvrir le pare-feu (une seule fois)

**PowerShell en administrateur** (clic droit sur le menu Démarrer → **Terminal (administrateur)**) :

```powershell
New-NetFirewallRule -DisplayName "TP ESP32 MQTT" -Direction Inbound -Protocol TCP -LocalPort 1883 -Action Allow -Profile Any
```

## 6. Créer le point d'accès Wi-Fi du PC

**Paramètres → Réseau et Internet → Point d'accès sans fil mobile → Modifier** :

| Réglage | Valeur |
|---|---|
| Nom du réseau | **`TP-ESP32`** |
| Mot de passe | **`coap2026tp`** |
| Bande | **2,4 GHz** (obligatoire : l'ESP32 ne capte pas le 5 GHz) |

**Enregistrer**, puis **activer** le point d'accès. Désactiver **« Économie d'énergie »** s'il y a l'option.

> Pourquoi ? Le firmware est le même pour tout le monde : il se connecte au Wi-Fi `TP-ESP32` et envoie ses données à `192.168.137.1`, qui est **toujours** l'adresse du PC qui partage sa connexion sous Windows.

## 7. Flasher la carte ESP32

1. Dans **Releases** (colonne de droite de la page GitHub), télécharger **le fichier qui correspond à votre capteur** :

   | Capteur | Fichier | Branchement |
   |---|---|---|
   | Aucun (valeurs simulées) | `tp_simule.bin` | - |
   | BMP280 / BME280 | `tp_bmp280.bin` | SDA → 21, SCL → 22, VCC → 3V3, GND → GND |
   | DHT11 | `tp_dht11.bin` | DATA → 4, VCC → 3V3, GND → GND |
   | DHT22 / AM2302 | `tp_dht22.bin` | DATA → 4, VCC → 3V3, GND → GND |
   | PIR (HC-SR501) | `tp_pir.bin` | OUT → 27, VCC → 5V (VIN), GND → GND |
   | MQ135 / LDR / potentiomètre | `tp_analog.bin` | sortie analogique → 34 |
   | **Actionneur (tous)** | | **LED : D2 → résistance 220 Ω → patte longue ; patte courte → GND** |

2. Brancher la carte en USB. Si aucun port COM n'apparaît, installer le pilote de la puce USB de la carte : **CP210x** (<https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers>) ou **CH340 / CH9102** (<https://www.wch-ic.com/downloads/CH341SER_EXE.html>).
3. Dans **Chrome**, ouvrir **<https://espressif.github.io/esptool-js/>**.
4. **Connect** → choisir le port COM de la carte.
5. **Flash Address = `0x0`**, choisir le fichier `.bin` → **Program**.
6. À la fin, appuyer sur le bouton **EN** de la carte (redémarrage).

> Si la connexion échoue : maintenir le bouton **BOOT** enfoncé pendant le clic sur **Connect**.

## 8. Tester

1. Après environ 10 s, la carte apparaît dans **Point d'accès sans fil mobile → Appareils connectés**.
2. <http://localhost:1880/page2> : un bloc par carte (`ESP32-xxxxxx`) avec les mesures toutes les 5 s.
3. **Allumer / Éteindre** : la LED sur D2 s'allume ou s'éteint, et l'état **ON/OFF** est confirmé par la carte sur la page.
4. Atlas : **Browse Collections → iot_tp** :
   - `mesures` : une ligne par mesure.
   - `commandes` : à chaque clic, 2 lignes (« ordre page web » + « confirmation carte »).
   - Pour voir les plus récentes : Options → Sort `{ "date": -1 }`.

> L'identifiant `esp32-xxxxxx` vient de l'adresse MAC de la carte : chaque carte a le sien.

## 9. Problèmes fréquents

| Symptôme | Solution |
|---|---|
| La carte ne se connecte pas au Wi-Fi | Point d'accès **allumé**, en **2,4 GHz**, nom `TP-ESP32` et mot de passe `coap2026tp` exacts |
| La carte est connectée mais la page reste vide | Règle du pare-feu (étape 5) ; `docker compose ps` doit montrer 2 conteneurs « running » |
| `docker compose up` : « port is already allocated » | Un autre Mosquitto ou Node-RED tourne déjà sur le PC : l'arrêter (voir l'étape 4) |
| Atlas n'enregistre rien | Lien correct dans `.env`, IP autorisée dans Network Access, puis `docker compose restart nodered` |
| Atlas : « bad auth » | Utilisateur créé dans **Database Access** (pas un projet), mot de passe correct dans le lien |
| Aucune LED ne s'allume | Beaucoup de cartes (ex. ESP32-DevKitC V4) n'ont **pas** de LED programmable : brancher une LED sur D2 |
| esptool-js ne trouve pas la carte | Câble de données, pilote CP210x/CH340, bouton BOOT pendant Connect, fermer les autres logiciels qui utilisent le port COM (Arduino...) |
| En classe, mes données arrivent chez un autre groupe | **Un seul point d'accès `TP-ESP32` allumé par groupe** : sinon une carte peut se connecter au PC d'un autre |

Pour revenir au flux d'origine après des modifications dans l'éditeur : `docker compose down -v` puis `docker compose up -d` (efface aussi l'historique Mosquitto).

## 10. Voir ce que fait la carte (moniteur série, facultatif)

Avec Python et `pyserial` (`pip install pyserial`) :

```
python outils/demo_moniteur.py COM3 COM4
```

Le moniteur affiche les mesures envoyées et reçues, les abonnements CoAP Observe, les ordres de la page (« PAGE WEB ») et l'actionneur (« ACTION LED ON → broche D2 = 3,3 V »). Remplacer COM3 et COM4 par les ports de vos cartes.

---

## Pour aller plus loin

### Compiler le firmware soi-même (facultatif)

Prérequis : **ESP-IDF 5.5.6** (installateur Windows d'Espressif), projet dans un **chemin court** hors OneDrive.

```powershell
cd firmware
. 'C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1'   # active ESP-IDF
.\build_all.ps1              # les 6 variantes -> dist\tp_<capteur>.bin
.\build_all.ps1 simule       # une seule variante
```

- Réglages communs : `firmware/sdkconfig.defaults` (Wi-Fi, broker, broches de l'actionneur...).
- Réglages par capteur : `firmware/sensors/<capteur>`.
- Pour un autre Wi-Fi : modifier `CONFIG_APP_WIFI_SSID` / `CONFIG_APP_WIFI_PASSWORD` dans `sdkconfig.defaults`, puis supprimer `firmware/build_*` avant de recompiler.
- `idf.py` ne fonctionne pas dans Git Bash : utiliser PowerShell.

### Modifier la page ou le flux Node-RED

- La page : `nodered/page2.html`. Les fonctions du flux : `docker/nodered/generer_flows.py`.
- Régénérer puis reconstruire :
  ```
  python docker/nodered/generer_flows.py
  docker compose down -v
  docker compose up -d --build
  ```

### Topics MQTT

| Topic | Sens | Contenu |
|---|---|---|
| `tp/<id>/data` | carte → PC | mesures JSON, toutes les 5 s |
| `tp/<id>/status` | carte → PC | `online` / `offline` (retenu + dernière volonté) |
| `tp/<id>/actuator` | carte → PC | état de l'actionneur (retenu) |
| `tp/<id>/cmd` | PC → carte | `{"state":"on"}`, `{"state":"off"}` ou `{"state":"toggle"}` |

Le protocole CoAP entre les cartes est décrit dans [PROTOCOLE_CLASSE.md](PROTOCOLE_CLASSE.md).
