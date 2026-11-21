#!/bin/bash
# Premier demarrage du volume /data : installe le flux et la configuration du TP.
# (l'image Node-RED contient deja un flows.json d'exemple : on se repere a un fichier temoin)
# Les demarrages suivants ne touchent plus a /data : les modifications faites dans l'editeur sont conservees.
TEMOIN=/data/.kit-tp-installe
if [ ! -f "$TEMOIN" ]; then
  cp /usr/src/node-red/kit/flows.json /usr/src/node-red/kit/settings.js /data/
  date > "$TEMOIN"
  echo "[kit TP] flux et configuration du TP installes dans /data"
fi
exec /usr/src/node-red/entrypoint.sh "$@"
