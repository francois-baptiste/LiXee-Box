#pragma once

// ============================================================================
// SensorFusion - Agregation de sondes de temperature Zigbee -> MQTT
//
// Fusionne la temperature (cluster 0x0402, attribut 0x0000, 0.01 degC) d'une
// liste de sondes identifiees par leur adresse IEEE et publie le resultat sur
// un topic MQTT libre (par defaut "openfrisquet/ext_temp"), destine au module
// OpenFrisquetVisio.
//
// Fonctionnement :
//   - sensor_fusion_on_temperature() est appele depuis le flux ZCL (tache
//     ZiGate) : il ne fait que memoriser la valeur + l'horodatage et marquer
//     une publication en attente. Aucun appel reseau, aucun blocage.
//   - sensor_fusion_loop() est appele chaque seconde par la boucle main :
//     expiration des sondes muettes, calcul (moyenne / min / max) et
//     publication avec un anti-rebond (10 s par defaut entre deux messages).
//   - Une sonde dont la derniere mesure date de plus de timeout_s (45 min par
//     defaut) est ignoree. Si aucune sonde n'est valide, rien n'est publie.
//
// Configuration : /littlefs/config/sensorFusion.json (cree avec les valeurs
// par defaut, sans sonde, s'il est absent, relu automatiquement s'il est modifie, par
// exemple via POST /api/files/write ; l'interface web passe par
// GET/POST /api/sensorfusion, page Mesures -> SensorFusion) :
//   {
//     "enabled": true,
//     "mode": "avg",                      // "avg" | "min" | "max"
//     "topic": "openfrisquet/ext_temp",
//     "timeout_s": 2700,                  // 45 min
//     "debounce_s": 10,
//     "retain": true,
//     "qos": 0,
//     "sensors": ["00124B0012345678"]
//   }
//
// Le flux normal vers Home Assistant (ziwifi32/...) n'est pas modifie.
// ============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_FUSION_MAX_SENSORS   16
#define SENSOR_FUSION_CONFIG_PATH   "/littlefs/config/sensorFusion.json"

typedef enum {
    SENSOR_FUSION_AVG = 0,
    SENSOR_FUSION_MIN,
    SENSOR_FUSION_MAX,
} sensor_fusion_mode_t;

/** Charge la configuration (la cree si absente). A appeler apres le montage de LittleFS. */
esp_err_t sensor_fusion_init(void);

/**
 * Nouvelle mesure de temperature (degC) recue d'un appareil Zigbee.
 * Ignoree si l'IEEE n'est pas dans la liste configuree. Non bloquant,
 * appelable depuis n'importe quelle tache.
 */
void sensor_fusion_on_temperature(uint64_t ieee_addr, float temp_c);

/** A appeler periodiquement (~1 s) depuis la boucle principale. */
void sensor_fusion_loop(void);

/**
 * Derniere valeur fusionnee calculee sur les sondes valides.
 * @return nombre de sondes valides (0 : pas de valeur, *out non modifie)
 */
int sensor_fusion_get(float *out);

/**
 * Etat courant pour l'interface web (GET /api/sensorfusion) :
 * {"config":{...},"mqttConnected":b,"validCount":n,"value":x|null,
 *  "lastPublishAgo":s|null,"lastPublishValue":x|null,"pending":b,
 *  "probes":[{"ieee":"...","valid":b,"temp":x|null,"ago":s|null}]}
 * @return chaine allouee (cJSON_free / free), NULL si memoire insuffisante
 */
char *sensor_fusion_status_json(void);

/**
 * Remplace la configuration (POST /api/sensorfusion) : champs absents
 * inchanges, valeurs verifiees (bornes, topic sans joker, IEEE). Le fichier
 * est ecrit tout de suite et la configuration appliquee par la boucle main.
 */
esp_err_t sensor_fusion_set_config_json(const char *json, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
