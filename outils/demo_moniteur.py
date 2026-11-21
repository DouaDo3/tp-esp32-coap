"""
Moniteur de démonstration : affiche en direct ce que chaque carte ENVOIE et REÇOIT.

Usage :  python demo_moniteur.py            (COM4 et COM7)
         python demo_moniteur.py COM4 COM7
Ctrl+C pour arrêter.
"""
import os
import re
import sys
import threading
import time

import serial

os.system("")  # active les couleurs ANSI dans la console Windows
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PORTS = sys.argv[1:3] if len(sys.argv) >= 3 else ["COM4", "COM7"]
COULEURS = ["\033[96m", "\033[95m"]  # cyan, magenta
VERT, JAUNE, ROUGE, GRIS, RAZ = "\033[92m", "\033[93m", "\033[91m", "\033[90m", "\033[0m"

# Lignes sans rapport avec la démo (anciennes cartes de la classe)
IGNORER = re.compile(r"172\.16\.20\.237|carte_esp32|mq2|handshake DTLS refuse")
R_OBSERVE = re.compile(r"\[OBSERVE\] (.*)")
BLEU = "\033[94m"
ANSI = re.compile(r"\x1b\[[0-9;]*m")

R_NOM = re.compile(r"Demarrage de la carte (\S+)")
R_CAPTEUR = re.compile(r"Capteur (\S+) : \d+ grandeur")
R_WIFI = re.compile(r"Connecte ! IP obtenue par DHCP : (\S+)")
R_ENVOI = re.compile(r"Mesure #(\d+) \((\S+)\) : (.*)")
R_RECU = re.compile(r"<< (\S+) : (?!info)(.*)")
R_ABO = re.compile(r"Abonnement Observe etabli avec (\S+) \((\S+)\)")
R_VU = re.compile(r"\[AJOUT\] (\S+) -> (\S+)")
R_PERDU = re.compile(r"(\S+) a quitte le reseau")
R_WIFI_KO = re.compile(r"Deconnecte \(raison (\d+)\)")
# 2e sens : page web -> Node-RED -> MQTT -> carte -> actionneur
R_CMD = re.compile(r"<< \[MQTT\] commande recue sur (\S+) : (.*)")
R_ACT = re.compile(r"\[ACTIONNEUR\] (\S+) -> (ON|OFF) \(GPIO (\d+)\)")
R_MQTT_OK = re.compile(r"\[MQTT\] connecte au broker (\S+)")

verrou = threading.Lock()


def afficher(etiquette, couleur, symbole, texte, c_texte=""):
    h = time.strftime("%H:%M:%S")
    with verrou:
        print(f"{GRIS}{h}{RAZ} {couleur}[{etiquette:^16}]{RAZ} {symbole} {c_texte}{texte}{RAZ}", flush=True)


def lire(port, couleur, numero):
    etiquette = f"Carte {numero} {port}"
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = False  # ouvrir sans redémarrer la carte
    s.rts = False
    try:
        s.open()
    except serial.SerialException as e:
        afficher(etiquette, couleur, "!!", f"port inaccessible ({e})", ROUGE)
        return
    afficher(etiquette, couleur, "..", "en écoute (appuyer sur EN pour voir le démarrage)", GRIS)
    while True:
        try:
            brut = s.readline()
        except serial.SerialException:
            afficher(etiquette, couleur, "!!", "carte débranchée", ROUGE)
            return
        if not brut:
            continue
        ligne = ANSI.sub("", brut.decode("utf-8", "replace")).strip()
        if not ligne or IGNORER.search(ligne):
            continue
        if m := R_CMD.search(ligne):
            afficher(etiquette, couleur, "PAGE WEB", f"ordre recu par MQTT : {m.group(2)}", JAUNE)
        elif m := R_ACT.search(ligne):
            etat = m.group(2)
            afficher(etiquette, couleur, "ACTION", f"{m.group(1)} {etat}  ->  broche D{m.group(3)} = {'3,3 V' if etat == 'ON' else '0 V'}",
                     VERT if etat == "ON" else ROUGE)
        elif m := R_MQTT_OK.search(ligne):
            afficher(etiquette, couleur, "MQTT", f"connectée au broker {m.group(1)}", JAUNE)
        elif m := R_OBSERVE.search(ligne):
            afficher(etiquette, couleur, "OBSERVE", m.group(1), BLEU)
        elif m := R_NOM.search(ligne):
            etiquette = f"Carte {numero}"
            afficher(etiquette, couleur, "**", "démarrage", JAUNE)
        elif m := R_CAPTEUR.search(ligne):
            etiquette = f"Carte {numero} {m.group(1)}"
            afficher(etiquette, couleur, "**", f"capteur {m.group(1)} prêt", JAUNE)
        elif m := R_WIFI.search(ligne):
            afficher(etiquette, couleur, "WiFi", f"connectée, IP {m.group(1)}", JAUNE)
        elif m := R_WIFI_KO.search(ligne):
            afficher(etiquette, couleur, "WiFi", f"connexion en cours... (raison {m.group(1)})", GRIS)
        elif m := R_VU.search(ligne):
            afficher(etiquette, couleur, "mDNS", f"a trouvé {m.group(1)} ({m.group(2)})", JAUNE)
        elif m := R_ABO.search(ligne):
            afficher(etiquette, couleur, "DTLS", f"abonnée à {m.group(1)} (connexion chiffrée OK)", VERT)
        elif m := R_PERDU.search(ligne):
            afficher(etiquette, couleur, "--", f"{m.group(1)} a quitté le réseau", ROUGE)
        elif m := R_ENVOI.search(ligne):
            afficher(etiquette, couleur, "ENVOIE ->", m.group(3))
        elif m := R_RECU.search(ligne):
            afficher(etiquette, couleur, "REÇOIT <-", f"{m.group(2)}   (de {m.group(1)})", VERT)


print(f"Moniteur CoAPS / DTLS / MQTT : {PORTS[0]} et {PORTS[1]}   (Ctrl+C pour quitter)")
print("Cliquer sur Allumer / Eteindre dans http://localhost:1880/page2 : la carte affiche ACTION ON / OFF")
print("Reference : CoAP = RFC 7252, Observe = RFC 7641, DTLS 1.2 = RFC 6347, PSK = RFC 4279\n")
for i, (p, c) in enumerate(zip(PORTS, COULEURS), 1):
    threading.Thread(target=lire, args=(p, c, i), daemon=True).start()
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("\nArrêt.")
