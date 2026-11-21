"""Genere docker/nodered/flows.json (flux AUTONOME du TP) a partir de nodered/page2.html.

Usage : python docker/nodered/generer_flows.py
A relancer apres une modification de page2.html ou des fonctions ci-dessous,
puis reconstruire l'image : docker compose up -d --build
(et docker compose down -v si le flux etait deja installe dans le volume).

Flux (un seul onglet "TP ESP32") :
  SENS 1  mqtt in tp/+/+ -> "cartes -> page / MongoDB" -> websocket out /ws/nodered (page temps reel)
                                                        -> MongoDB collection "mesures"
                                                        -> MongoDB collection "commandes" (confirmations)
  SENS 2  websocket in /ws/nodered -> "page -> carte" -> mqtt out tp/<carte>/cmd
                                                      -> MongoDB collection "commandes" (ordres)
  PAGE    GET /page2 -> template (page2.html) -> http response
MongoDB : URI lue dans la variable d'environnement MONGODB_URI (fichier .env), jamais dans le flux.
"""
import json
import pathlib

ICI = pathlib.Path(__file__).parent
PAGE = ICI.parent.parent / "nodered" / "page2.html"

FN_MQTT = r"""// Messages des cartes : tp/<carte>/data | status | actuator
const [, carte, type] = msg.topic.split('/');
if (!['data', 'status', 'actuator'].includes(type)) return null;   // ignore tp/<carte>/cmd

let p = msg.payload;
if (Buffer.isBuffer(p)) p = p.toString();
if (typeof p === 'string') {
    if (p === '') p = null;                       // etat retenu efface -> carte retiree
    else { try { p = JSON.parse(p); } catch (e) { p = { state: p }; } }
}

// Memoire des cartes : la page la recoit a l'ouverture (message "hello")
const cartes = flow.get('cartes') || {};
if (p === null) {
    delete cartes[carte];
    flow.set('cartes', cartes);
    return [{ payload: JSON.stringify({ type: 'remove', device_id: carte }) }, null, null];
}
const c = cartes[carte] = cartes[carte] || { device_id: carte };
let versBase = null;
let versHisto = null;
if (type === 'data') {
    c.mesure = p;
    c.ts = Date.now();
    // Document enregistre dans MongoDB (collection mesures)
    const doc = Object.assign({}, p, { device_id: carte, date: new Date(c.ts) });
    delete doc.units;
    versBase = { payload: doc };
} else if (type === 'status') {
    c.etat = p;
} else {
    c.actionneur = p;
    // Historique : etat confirme par la carte (pas les messages retenus rejoues a la reconnexion)
    if (!msg.retain && p && p.state) {
        versHisto = { payload: { device_id: carte, type: 'confirmation carte', actionneur: p.actuator || 'LED',
                                 etat: p.state, date: new Date() } };
    }
}
flow.set('cartes', cartes);
node.status({ fill: 'green', shape: 'dot', text: carte + ' : ' + type });

const versPage = { payload: JSON.stringify({ type: type, device_id: carte, data: p, ts: c.ts }) };
const mongo = env.get('MONGODB_ENABLED') === '1';     // MongoDB facultatif (.env)
return [versPage, mongo ? versBase : null, mongo ? versHisto : null];
"""

FN_PAGE = r"""// Messages envoyes par la page au serveur WebSocket interne (/ws/nodered)
let m;
try { m = JSON.parse(msg.payload); } catch (e) { return null; }

if (m.type === 'hello') {
    // Page qui vient de s'ouvrir : on lui renvoie (a elle seule, msg._session) l'etat des cartes
    msg.payload = JSON.stringify({ type: 'snapshot', cartes: flow.get('cartes') || {} });
    return [msg, null, null];
}
if (m.type === 'cmd' && m.device_id && ['on', 'off', 'toggle'].includes(m.state)) {
    // 2e sens : commande de l'actionneur -> MQTT tp/<carte>/cmd
    node.status({ fill: 'blue', shape: 'dot', text: m.device_id + ' -> ' + m.state });
    const cmd = { topic: 'tp/' + m.device_id + '/cmd', payload: JSON.stringify({ state: m.state }) };
    const histo = { payload: { device_id: m.device_id, type: 'ordre page web', commande: m.state, date: new Date() } };
    return [null, cmd, env.get('MONGODB_ENABLED') === '1' ? histo : null];
}
return null;
"""


def flux():
    T = "tp_onglet"
    n = lambda **k: dict(z=T, **k)
    return [
        {"id": T, "type": "tab", "label": "TP ESP32", "disabled": False,
         "info": "Flux du kit TP : cartes ESP32 -> MQTT -> page /page2 + MongoDB ; page -> MQTT -> cartes."},

        n(id="tp_c1", type="comment", name="SENS 1 : cartes ESP32 -> MQTT -> page temps reel + MongoDB", x=260, y=40, wires=[]),
        n(id="tp_mqtt_in", type="mqtt in", name="cartes ESP32 (tp/+/+)", topic="tp/+/+", qos="1",
          datatype="auto-detect", broker="tp_broker", nl=False, rap=True, rh=0, inputs=0,
          x=150, y=100, wires=[["tp_fn_mqtt"]]),
        n(id="tp_fn_mqtt", type="function", name="cartes -> page / MongoDB", func=FN_MQTT, outputs=3, noerr=0,
          outputLabels=["vers la page", "MongoDB mesures", "MongoDB commandes"],
          x=400, y=100, wires=[["tp_ws_out"], ["tp_mongo_mesures"], ["tp_mongo_cmd"]]),
        n(id="tp_ws_out", type="websocket out", name="vers la page (/ws/nodered)", server="tp_ws", client="",
          x=700, y=60, wires=[]),
        n(id="tp_mongo_mesures", type="mongodb4", name="MongoDB : mesures", clientNode="tp_mongo",
          mode="collection", collection="mesures", operation="insertOne", output="toArray",
          maxTimeMS="0", handleDocId=False, x=680, y=110, wires=[["tp_dbg_mongo"]]),
        n(id="tp_dbg_mongo", type="debug", name="resultat MongoDB", active=True, tosidebar=True, console=False,
          tostatus=True, complete="payload", targetType="msg", statusVal="payload.insertedId",
          statusType="msg", x=920, y=110, wires=[]),

        n(id="tp_c2", type="comment", name="SENS 2 : page -> WebSocket -> MQTT tp/<carte>/cmd -> carte", x=250, y=220, wires=[]),
        n(id="tp_ws_in", type="websocket in", name="depuis la page (/ws/nodered)", server="tp_ws", client="",
          x=160, y=280, wires=[["tp_fn_page"]]),
        n(id="tp_fn_page", type="function", name="page -> carte", func=FN_PAGE, outputs=3, noerr=0,
          outputLabels=["reponse a la page", "commande MQTT", "MongoDB commandes"],
          x=400, y=280, wires=[["tp_ws_out"], ["tp_mqtt_out", "tp_dbg_cmd"], ["tp_mongo_cmd"]]),
        n(id="tp_mqtt_out", type="mqtt out", name="commande -> tp/<carte>/cmd", topic="", qos="1",
          retain="false", broker="tp_broker", x=680, y=260, wires=[]),
        n(id="tp_dbg_cmd", type="debug", name="commandes envoyees", active=True, tosidebar=True, console=False,
          tostatus=False, complete="true", targetType="full", x=670, y=300, wires=[]),
        n(id="tp_mongo_cmd", type="mongodb4", name="MongoDB : commandes", clientNode="tp_mongo",
          mode="collection", collection="commandes", operation="insertOne", output="toArray",
          maxTimeMS="0", handleDocId=False, x=690, y=350, wires=[["tp_dbg_mongo_cmd"]]),
        n(id="tp_dbg_mongo_cmd", type="debug", name="resultat MongoDB commandes", active=True, tosidebar=True,
          console=False, tostatus=True, complete="payload", targetType="msg", statusVal="payload.insertedId",
          statusType="msg", x=940, y=350, wires=[]),

        n(id="tp_c3", type="comment", name="PAGE : http://localhost:1880/page2", x=190, y=440, wires=[]),
        n(id="tp_http_in", type="http in", name="GET /page2", url="/page2", method="get", upload=False,
          swaggerDoc="", x=140, y=500, wires=[["tp_template"]]),
        n(id="tp_template", type="template", name="page2.html", field="payload", fieldType="msg",
          format="html", syntax="plain", template=PAGE.read_text(encoding="utf-8"), output="str",
          x=340, y=500, wires=[["tp_http_resp"]]),
        n(id="tp_http_resp", type="http response", name="", statusCode="",
          headers={"content-type": "text/html; charset=utf-8"}, x=530, y=500, wires=[]),

        # ---- configurations ----
        {"id": "tp_broker", "type": "mqtt-broker", "name": "Mosquitto (conteneur)", "broker": "mosquitto",
         "port": "1883", "clientid": "", "autoConnect": True, "usetls": False, "protocolVersion": "4",
         "keepalive": "60", "cleansession": True, "autoUnsubscribe": True,
         "birthTopic": "", "closeTopic": "", "willTopic": ""},
        {"id": "tp_ws", "type": "websocket-listener", "path": "/ws/nodered", "wholemsg": "false"},
        {"id": "tp_mongo", "type": "mongodb4-client", "name": "MongoDB Atlas (lien dans .env)",
         "protocol": "mongodb+srv", "hostname": "", "port": "", "dbName": "${MONGODB_DB}",
         "appName": "node-red-tp", "authSource": "", "authMechanism": "", "tls": False,
         "tlsCAFile": "", "tlsCertificateKeyFile": "", "tlsInsecure": False, "connectTimeoutMS": "30000",
         "socketTimeoutMS": "0", "minPoolSize": "0", "maxPoolSize": "100", "maxIdleTimeMS": "0",
         "uri": "${MONGODB_URI}", "advanced": "{}", "uriTabActive": "tab-uri-advanced"},
    ]


if __name__ == "__main__":
    sortie = ICI / "flows.json"
    sortie.write_text(json.dumps(flux(), ensure_ascii=False, indent=2), encoding="utf-8")
    print("flows.json genere :", sortie, f"({len(flux())} noeuds)")
