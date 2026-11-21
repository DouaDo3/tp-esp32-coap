"""Simule deux cartes ESP32 (capteur + actionneur) sur Mosquitto, sans materiel.

Usage : python nodered/simuler_cartes.py            (Ctrl+C pour arreter)
        python nodered/simuler_cartes.py --broker 192.168.1.20

Memes topics / JSON que le firmware :
  tp/<id>/status    (retained)  {"device_id":..,"state":"online",...}
  tp/<id>/data                  {"device_id":..,"sensor":..,"seq":..,<grandeurs>,"units":{..}}
  tp/<id>/cmd       (ecoute)    {"state":"on"|"off"|"toggle"}   <- envoye par la page via Node-RED
  tp/<id>/actuator  (retained)  {"device_id":..,"actuator":"LED","state":"on"}
A l'arret, les cartes passent "offline" puis leurs etats retenus sont effaces.
"""
import argparse
import json
import random
import subprocess
import threading
import time

PUB = r"C:\Program Files\Mosquitto\mosquitto_pub.exe"
SUB = r"C:\Program Files\Mosquitto\mosquitto_sub.exe"

CARDS = {
    "esp32-sim001": {"sensor": "BME280", "ip": "192.168.1.50", "mesh_level": 1, "actuator": "LED"},
    "esp32-sim002": {"sensor": "PIR", "ip": "192.168.5.2", "mesh_level": 2, "actuator": "Buzzer"},
}
actuators = {cid: False for cid in CARDS}


def pub(broker, topic, payload, retain=False):
    args = [PUB, "-h", broker, "-t", topic]
    args += ["-n"] if payload is None else ["-m", json.dumps(payload)]
    if retain:
        args.append("-r")
    subprocess.run(args, check=True)


def publish_actuator(broker, cid):
    pub(broker, f"tp/{cid}/actuator", {"device_id": cid, "actuator": CARDS[cid]["actuator"],
                                       "state": "on" if actuators[cid] else "off"}, retain=True)


def listen_commands(broker, proc):
    """Lit les commandes tp/<id>/cmd comme le ferait la carte."""
    for line in proc.stdout:
        topic, _, payload = line.strip().partition(" ")
        cid = topic.split("/")[1] if topic.count("/") == 2 else None
        if cid not in CARDS:
            continue
        try:
            state = json.loads(payload).get("state")
        except (ValueError, AttributeError):
            state = payload
        if state == "toggle":
            actuators[cid] = not actuators[cid]
        elif state in ("on", "off"):
            actuators[cid] = state == "on"
        else:
            print(f"  {cid} : commande inconnue {payload!r}")
            continue
        print(f"  << commande recue par {cid} : {state} -> {CARDS[cid]['actuator']} "
              f"{'ON' if actuators[cid] else 'OFF'}")
        publish_actuator(broker, cid)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default="127.0.0.1")
    ap.add_argument("--periode", type=float, default=2.0)
    a = ap.parse_args()

    sub = subprocess.Popen([SUB, "-h", a.broker, "-t", "tp/+/cmd", "-v"], stdout=subprocess.PIPE,
                           text=True, bufsize=1)
    threading.Thread(target=listen_commands, args=(a.broker, sub), daemon=True).start()

    for cid, c in CARDS.items():
        info = {k: v for k, v in c.items() if k != "actuator"}
        pub(a.broker, f"tp/{cid}/status", {"device_id": cid, "state": "online", **info}, retain=True)
        publish_actuator(a.broker, cid)
    print("Cartes simulees en ligne :", ", ".join(CARDS), "- Ctrl+C pour arreter")

    seq, temp, hum = 0, 23.0, 50.0
    try:
        while True:
            seq += 1
            temp += random.uniform(-0.3, 0.3)
            hum += random.uniform(-1, 1)
            pub(a.broker, "tp/esp32-sim001/data", {
                "device_id": "esp32-sim001", "sensor": "BME280", "seq": seq, "ok": True,
                "temperature": round(temp, 2), "humidity": round(hum, 1),
                "pressure": round(1013 + random.uniform(-2, 2), 1),
                "units": {"temperature": "C", "humidity": "%", "pressure": "hPa"}})
            pub(a.broker, "tp/esp32-sim002/data", {
                "device_id": "esp32-sim002", "sensor": "PIR", "seq": seq, "ok": True,
                "motion": int(random.random() < 0.3), "units": {"motion": ""}})
            print(f"seq {seq} : temperature={temp:.2f} C")
            time.sleep(a.periode)
    except KeyboardInterrupt:
        pass
    finally:
        sub.terminate()
        for cid in CARDS:
            pub(a.broker, f"tp/{cid}/status", {"device_id": cid, "state": "offline"})
            pub(a.broker, f"tp/{cid}/status", None, retain=True)     # efface l'etat retenu
            pub(a.broker, f"tp/{cid}/actuator", None, retain=True)
        print("\nCartes simulees hors ligne.")


if __name__ == "__main__":
    main()
