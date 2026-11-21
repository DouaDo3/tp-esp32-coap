# Node-RED du TP : noeud MongoDB preinstalle + flux et page du TP charges au premier demarrage
FROM nodered/node-red:4.0.9

# Installe dans le dossier de l'application (et non dans /data, qui est un volume)
WORKDIR /usr/src/node-red
RUN npm install --no-fund --no-update-notifier --omit=dev node-red-contrib-mongodb4@3.4.0

COPY --chown=node-red:root flows.json settings.js kit-start.sh /usr/src/node-red/kit/
USER root
RUN sed -i 's/\r$//' /usr/src/node-red/kit/kit-start.sh && chmod +x /usr/src/node-red/kit/kit-start.sh
USER node-red

ENTRYPOINT ["/usr/src/node-red/kit/kit-start.sh"]
