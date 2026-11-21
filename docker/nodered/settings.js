// Configuration Node-RED du kit TP (copiee dans /data au premier demarrage)

// MongoDB est facultatif : sans MONGODB_URI (fichier .env), le flux fonctionne sans base.
// Le noeud mongodb4 exige une URI valide au demarrage : on met une valeur neutre qui n'est
// jamais contactee (les fonctions n'envoient rien vers MongoDB quand MONGODB_ENABLED vaut 0).
if (process.env.MONGODB_URI && process.env.MONGODB_URI.trim()) {
    process.env.MONGODB_ENABLED = "1";
} else {
    process.env.MONGODB_ENABLED = "0";
    process.env.MONGODB_URI = "mongodb://127.0.0.1:27017";
}
process.env.MONGODB_DB = process.env.MONGODB_DB || "iot_tp";
console.log("[kit TP] MongoDB " + (process.env.MONGODB_ENABLED === "1" ? "ACTIVE (base " + process.env.MONGODB_DB + ")" : "desactive (MONGODB_URI vide dans .env)"));

module.exports = {
    flowFile: "flows.json",
    flowFilePretty: true,
    // Aucun secret dans le flux ; cle fixe pour eviter l'avertissement de chiffrement
    credentialSecret: process.env.NODE_RED_CREDENTIAL_SECRET || "kit-tp-esp32",
    uiPort: process.env.PORT || 1880,
    diagnostics: { enabled: false, ui: false },
    runtimeState: { enabled: false, ui: false },
    logging: { console: { level: "info", metrics: false, audit: false } },
    exportGlobalContextKeys: false,
    externalModules: {},
    editorTheme: { projects: { enabled: false }, tours: false },
    functionExternalModules: false,
    functionGlobalContext: {},
    debugMaxLength: 1000,
    mqttReconnectTime: 5000,
};
