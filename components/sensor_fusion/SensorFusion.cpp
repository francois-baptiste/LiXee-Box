// ============================================================================
// SensorFusion - voir include/SensorFusion.h
// ============================================================================

#include "SensorFusion.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mqtt_manager.h"
#include "config_manager.h"

static const char *TAG = "SensorFusion";

#define DEFAULT_TOPIC        "openfrisquet/ext_temp"
#define DEFAULT_TIMEOUT_S    (45 * 60)
#define DEFAULT_DEBOUNCE_S   10
#define MIN_TIMEOUT_S        60
#define MAX_TIMEOUT_S        (24 * 3600)
#define MIN_DEBOUNCE_S       10             // anti-rebond minimum impose
#define MAX_DEBOUNCE_S       3600
#define CONFIG_CHECK_MS      30000          // relecture du fichier si modifie
#define CONFIG_MAX_BYTES     4096
#define TEMP_MIN_VALID       (-60.0f)       // bornes de plausibilite (0x8000 =
#define TEMP_MAX_VALID       (125.0f)       //  "invalide" en ZCL -> -327.68)

typedef struct {
    uint64_t ieee;
    float    temp_c;
    int64_t  ts_ms;     // horodatage (equivalent millis(), 64 bits : pas de rebouclage)
    bool     seen;      // au moins une mesure recue depuis le demarrage
    bool     valid;     // mesure recue et non expiree
} probe_t;

typedef struct {
    bool                 enabled;
    sensor_fusion_mode_t mode;
    char                 topic[128];
    uint32_t             timeout_s;
    uint32_t             debounce_s;
    bool                 retain;
    int                  qos;
    int                  sensor_count;
    uint64_t             sensors[SENSOR_FUSION_MAX_SENSORS];
} fusion_cfg_t;

// Etat partage entre la tache ZiGate (on_temperature), la boucle main et le
// serveur web : sections critiques tres courtes (copies) -> spinlock.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static fusion_cfg_t s_cfg;
static probe_t      s_probes[SENSOR_FUSION_MAX_SENSORS];
static int          s_probe_count;
static bool         s_pending;
static int64_t      s_last_pub_ms = -1;
static float        s_last_pub_value;
static float        s_last_value;
static int          s_last_count;
static bool         s_inited;

// Suivi du fichier de configuration (tache main uniquement)
static time_t   s_cfg_mtime;
static off_t    s_cfg_size;
static int64_t  s_cfg_check_ms;
static volatile bool s_reload_requested;

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *mode_name(sensor_fusion_mode_t m)
{
    return m == SENSOR_FUSION_MIN ? "min" : m == SENSOR_FUSION_MAX ? "max" : "avg";
}

/** "0012:4B00:..." / "0x00124B00..." -> uint64. 0 si invalide. */
static uint64_t parse_ieee(const char *s)
{
    uint64_t v = 0;
    int digits = 0;
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c == ':' || c == '-' || c == ' ') continue;
        if (!isxdigit(c)) return 0;
        v = (v << 4) | (uint64_t)(isdigit(c) ? c - '0' : (toupper(c) - 'A' + 10));
        if (++digits > 16) return 0;
    }
    return digits ? v : 0;
}

static void set_defaults(fusion_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->enabled    = true;
    c->mode       = SENSOR_FUSION_AVG;
    strlcpy(c->topic, DEFAULT_TOPIC, sizeof(c->topic));
    c->timeout_s  = DEFAULT_TIMEOUT_S;
    c->debounce_s = DEFAULT_DEBOUNCE_S;
    c->retain     = true;
    c->qos        = 0;
}

/**
 * Lit une configuration JSON dans *c (prealablement initialisee).
 * strict : refuse les valeurs hors bornes (API web) au lieu de les ignorer
 * (fichier edite a la main).
 */
static bool parse_config(const cJSON *root, fusion_cfg_t *c, bool strict, char *err, size_t err_len)
{
#define FAIL(...) do { if (strict) { if (err) snprintf(err, err_len, __VA_ARGS__); return false; } } while (0)
    const cJSON *it;
    if ((it = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(it))
        c->enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "mode")) && cJSON_IsString(it)) {
        if (!strcasecmp(it->valuestring, "min"))      c->mode = SENSOR_FUSION_MIN;
        else if (!strcasecmp(it->valuestring, "max")) c->mode = SENSOR_FUSION_MAX;
        else if (!strcasecmp(it->valuestring, "avg")) c->mode = SENSOR_FUSION_AVG;
        else FAIL("Mode inconnu (avg, min ou max)");
    }
    if ((it = cJSON_GetObjectItem(root, "topic")) && cJSON_IsString(it)) {
        const char *t = it->valuestring;
        size_t n = strlen(t);
        if (n == 0 || n >= sizeof(c->topic) || strpbrk(t, "#+"))
            FAIL("Topic invalide (1 a %u caracteres, sans # ni +)", (unsigned)sizeof(c->topic) - 1);
        else strlcpy(c->topic, t, sizeof(c->topic));
    }
    if ((it = cJSON_GetObjectItem(root, "timeout_s")) && cJSON_IsNumber(it)) {
        if (it->valuedouble < MIN_TIMEOUT_S || it->valuedouble > MAX_TIMEOUT_S)
            FAIL("Timeout entre %d et %d s", MIN_TIMEOUT_S, MAX_TIMEOUT_S);
        else c->timeout_s = (uint32_t)it->valuedouble;
    }
    if ((it = cJSON_GetObjectItem(root, "debounce_s")) && cJSON_IsNumber(it)) {
        if (it->valuedouble < MIN_DEBOUNCE_S || it->valuedouble > MAX_DEBOUNCE_S)
            FAIL("Anti-rebond entre %d et %d s", MIN_DEBOUNCE_S, MAX_DEBOUNCE_S);
        else c->debounce_s = (uint32_t)it->valuedouble;
    }
    if ((it = cJSON_GetObjectItem(root, "retain")) && cJSON_IsBool(it))
        c->retain = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "qos")) && cJSON_IsNumber(it)) {
        if (it->valueint < 0 || it->valueint > 1) FAIL("QoS 0 ou 1");
        else c->qos = it->valueint;
    }
    const cJSON *arr = cJSON_GetObjectItem(root, "sensors");
    if (arr) {
        c->sensor_count = 0;
        const cJSON *s;
        cJSON_ArrayForEach(s, arr) {
            uint64_t v = cJSON_IsString(s) ? parse_ieee(s->valuestring) : 0;
            if (!v) {
                FAIL("Adresse IEEE invalide : %.24s", cJSON_IsString(s) ? s->valuestring : "?");
                ESP_LOGW(TAG, "Adresse IEEE ignoree : %s", cJSON_IsString(s) ? s->valuestring : "?");
                continue;
            }
            bool dup = false;
            for (int i = 0; i < c->sensor_count; i++) dup |= c->sensors[i] == v;
            if (dup) continue;
            if (c->sensor_count >= SENSOR_FUSION_MAX_SENSORS) {
                FAIL("%d sondes maximum", SENSOR_FUSION_MAX_SENSORS);
                ESP_LOGW(TAG, "Plus de %d sondes : surplus ignore", SENSOR_FUSION_MAX_SENSORS);
                break;
            }
            c->sensors[c->sensor_count++] = v;
        }
    }
    return true;
#undef FAIL
}

static cJSON *config_to_json(const fusion_cfg_t *c)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddBoolToObject(root, "enabled", c->enabled);
    cJSON_AddStringToObject(root, "mode", mode_name(c->mode));
    cJSON_AddStringToObject(root, "topic", c->topic);
    cJSON_AddNumberToObject(root, "timeout_s", c->timeout_s);
    cJSON_AddNumberToObject(root, "debounce_s", c->debounce_s);
    cJSON_AddBoolToObject(root, "retain", c->retain);
    cJSON_AddNumberToObject(root, "qos", c->qos);
    cJSON *arr = cJSON_AddArrayToObject(root, "sensors");
    for (int i = 0; arr && i < c->sensor_count; i++) {
        char hex[17];
        snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)c->sensors[i]);
        cJSON_AddItemToArray(arr, cJSON_CreateString(hex));
    }
    return root;
}

static esp_err_t save_config(const fusion_cfg_t *c)
{
    cJSON *root = config_to_json(c);
    char *txt = root ? cJSON_Print(root) : NULL;
    cJSON_Delete(root);
    if (!txt) return ESP_ERR_NO_MEM;
    esp_err_t e = cfg_write_file(SENSOR_FUSION_CONFIG_PATH, txt);
    cJSON_free(txt);
    if (e != ESP_OK) ESP_LOGW(TAG, "Ecriture de %s impossible", SENSOR_FUSION_CONFIG_PATH);
    return e;
}

/** Applique une configuration (conserve les mesures des sondes restees listees). */
static void apply_config(const fusion_cfg_t *cfg)
{
    probe_t probes[SENSOR_FUSION_MAX_SENSORS] = {};
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < cfg->sensor_count; i++) {
        probes[i].ieee = cfg->sensors[i];
        for (int j = 0; j < s_probe_count; j++) {
            if (s_probes[j].ieee == cfg->sensors[i]) { probes[i] = s_probes[j]; break; }
        }
    }
    memcpy(s_probes, probes, sizeof(s_probes));
    s_probe_count = cfg->sensor_count;
    s_cfg = *cfg;
    s_pending = true;                    // republier avec la nouvelle configuration
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "%s : %d sonde(s), mode %s, topic \"%s\", timeout %us, anti-rebond %us",
             cfg->enabled ? "Actif" : "Inactif", cfg->sensor_count, mode_name(cfg->mode),
             cfg->topic, (unsigned)cfg->timeout_s, (unsigned)cfg->debounce_s);
}

static void remember_file_stamp(void)
{
    struct stat st;
    if (stat(SENSOR_FUSION_CONFIG_PATH, &st) == 0) {
        s_cfg_mtime = st.st_mtime;
        s_cfg_size  = st.st_size;
    }
}

/** Lit le fichier (le cree avec les valeurs par defaut s'il est absent) et l'applique. */
static void load_config(void)
{
    fusion_cfg_t cfg;
    set_defaults(&cfg);

    struct stat st;
    if (stat(SENSOR_FUSION_CONFIG_PATH, &st) != 0) {
        // Aucune sonde par defaut : a choisir dans Mesures -> SensorFusion
        if (save_config(&cfg) == ESP_OK)
            ESP_LOGI(TAG, "Configuration par defaut creee : %s", SENSOR_FUSION_CONFIG_PATH);
        apply_config(&cfg);
        remember_file_stamp();
        return;
    }

    char *buf = NULL;
    FILE *f = fopen(SENSOR_FUSION_CONFIG_PATH, "rb");
    if (f && st.st_size > 0 && st.st_size <= CONFIG_MAX_BYTES) {
        buf = (char *)malloc(st.st_size + 1);
        if (buf) {
            size_t rd = fread(buf, 1, st.st_size, f);
            buf[rd] = '\0';
        }
    }
    if (f) fclose(f);

    cJSON *root = buf ? cJSON_Parse(buf) : NULL;
    free(buf);
    if (root) {
        parse_config(root, &cfg, false, NULL, 0);
        cJSON_Delete(root);
    } else {
        ESP_LOGW(TAG, "%s illisible : valeurs par defaut, aucune sonde", SENSOR_FUSION_CONFIG_PATH);
    }
    apply_config(&cfg);
    remember_file_stamp();
}

/** Calcule la valeur fusionnee. Appelant : section critique tenue. */
static int compute_locked(float *out)
{
    int count = 0;
    float acc = 0.0f;
    for (int i = 0; i < s_probe_count; i++) {
        if (!s_probes[i].valid) continue;
        float t = s_probes[i].temp_c;
        if (count == 0) acc = t;
        else if (s_cfg.mode == SENSOR_FUSION_MIN) acc = fminf(acc, t);
        else if (s_cfg.mode == SENSOR_FUSION_MAX) acc = fmaxf(acc, t);
        else acc += t;
        count++;
    }
    if (count && s_cfg.mode == SENSOR_FUSION_AVG) acc /= (float)count;
    if (count) *out = acc;
    return count;
}

// ============================================================================
// API publique
// ============================================================================

esp_err_t sensor_fusion_init(void)
{
    load_config();
    s_cfg_check_ms = now_ms();
    s_inited = true;
    return ESP_OK;
}

void sensor_fusion_on_temperature(uint64_t ieee_addr, float temp_c)
{
    if (!s_inited || !isfinite(temp_c) || temp_c < TEMP_MIN_VALID || temp_c > TEMP_MAX_VALID) return;
    bool hit = false;
    taskENTER_CRITICAL(&s_lock);
    if (s_cfg.enabled) {
        for (int i = 0; i < s_probe_count; i++) {
            if (s_probes[i].ieee == ieee_addr) {
                s_probes[i].temp_c = temp_c;
                s_probes[i].ts_ms  = now_ms();
                s_probes[i].seen   = true;
                s_probes[i].valid  = true;
                s_pending = true;
                hit = true;
                break;
            }
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (hit) {
        ESP_LOGD(TAG, "Sonde %016llX : %.2f C", (unsigned long long)ieee_addr, temp_c);
    }
}

void sensor_fusion_loop(void)
{
    if (!s_inited) return;
    int64_t now = now_ms();

    // Configuration modifiee (ex : /api/files/write) -> relecture
    if (s_reload_requested || now - s_cfg_check_ms >= CONFIG_CHECK_MS) {
        s_cfg_check_ms = now;
        struct stat st;
        if (s_reload_requested ||
            (stat(SENSOR_FUSION_CONFIG_PATH, &st) == 0 &&
             (st.st_mtime != s_cfg_mtime || st.st_size != s_cfg_size))) {
            ESP_LOGI(TAG, "Configuration modifiee : relecture");
            s_reload_requested = false;
            load_config();
        }
    }

    char  topic[sizeof(s_cfg.topic)];
    bool  retain = false, publish = false;
    int   qos = 0, count = 0, expired = 0;
    uint32_t timeout_s = 0;
    float value = 0.0f;

    taskENTER_CRITICAL(&s_lock);
    if (s_cfg.enabled) {
        timeout_s = s_cfg.timeout_s;
        int64_t timeout_ms = (int64_t)s_cfg.timeout_s * 1000;
        for (int i = 0; i < s_probe_count; i++) {
            if (s_probes[i].valid && now - s_probes[i].ts_ms > timeout_ms) {
                s_probes[i].valid = false;           // sonde muette : ecartee
                s_pending = true;
                expired++;
            }
        }
        count = compute_locked(&value);
        if (count) s_last_value = value;
        s_last_count = count;
        bool debounce_ok = s_last_pub_ms < 0 ||
                           now - s_last_pub_ms >= (int64_t)s_cfg.debounce_s * 1000;
        if (s_pending && debounce_ok) {
            s_pending = false;                       // une mise a jour ulterieure le reposera
            publish = count > 0;
            strlcpy(topic, s_cfg.topic, sizeof(topic));
            retain = s_cfg.retain;
            qos = s_cfg.qos;
        }
    } else {
        s_last_count = 0;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (expired) {
        ESP_LOGW(TAG, "%d sonde(s) sans mesure depuis plus de %u s : ignoree(s)",
                 expired, (unsigned)timeout_s);
    }
    if (!publish) return;

    char payload[16];
    snprintf(payload, sizeof(payload), "%.2f", value);
    esp_err_t err = mqtt_manager_is_connected()
                    ? mqtt_manager_publish_raw(topic, payload, qos, retain)
                    : ESP_ERR_INVALID_STATE;

    taskENTER_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        s_last_pub_ms = now;
        s_last_pub_value = value;
    } else {
        s_pending = true;                            // nouvel essai a la prochaine seconde
    }
    taskEXIT_CRITICAL(&s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s -> %s (%d sonde(s) valide(s))", topic, payload, count);
    }
}

int sensor_fusion_get(float *out)
{
    taskENTER_CRITICAL(&s_lock);
    int count = s_last_count;
    if (count && out) *out = s_last_value;
    taskEXIT_CRITICAL(&s_lock);
    return count;
}

char *sensor_fusion_status_json(void)
{
    fusion_cfg_t cfg;
    probe_t probes[SENSOR_FUSION_MAX_SENSORS];
    int n, count;
    float value;
    int64_t last_pub_ms;
    float last_pub_value;
    bool pending;

    taskENTER_CRITICAL(&s_lock);
    cfg = s_cfg;
    n = s_probe_count;
    memcpy(probes, s_probes, sizeof(probes));
    count = s_last_count;
    value = s_last_value;
    last_pub_ms = s_last_pub_ms;
    last_pub_value = s_last_pub_value;
    pending = s_pending;
    taskEXIT_CRITICAL(&s_lock);

    int64_t now = now_ms();
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddItemToObject(root, "config", config_to_json(&cfg));
    cJSON_AddBoolToObject(root, "mqttConnected", mqtt_manager_is_connected());
    cJSON_AddNumberToObject(root, "validCount", count);
    if (count) cJSON_AddNumberToObject(root, "value", roundf(value * 100.0f) / 100.0f);
    else       cJSON_AddNullToObject(root, "value");
    if (last_pub_ms >= 0) {
        cJSON_AddNumberToObject(root, "lastPublishAgo", (double)((now - last_pub_ms) / 1000));
        cJSON_AddNumberToObject(root, "lastPublishValue", roundf(last_pub_value * 100.0f) / 100.0f);
    } else {
        cJSON_AddNullToObject(root, "lastPublishAgo");
        cJSON_AddNullToObject(root, "lastPublishValue");
    }
    cJSON_AddBoolToObject(root, "pending", pending);
    cJSON *arr = cJSON_AddArrayToObject(root, "probes");
    for (int i = 0; arr && i < n; i++) {
        cJSON *p = cJSON_CreateObject();
        char hex[17];
        snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)probes[i].ieee);
        cJSON_AddStringToObject(p, "ieee", hex);
        cJSON_AddBoolToObject(p, "valid", probes[i].valid);
        if (probes[i].seen) {
            cJSON_AddNumberToObject(p, "temp", roundf(probes[i].temp_c * 100.0f) / 100.0f);
            cJSON_AddNumberToObject(p, "ago", (double)((now - probes[i].ts_ms) / 1000));
        } else {
            cJSON_AddNullToObject(p, "temp");
            cJSON_AddNullToObject(p, "ago");
        }
        cJSON_AddItemToArray(arr, p);
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return txt;
}

esp_err_t sensor_fusion_set_config_json(const char *json, char *err, size_t err_len)
{
    if (err && err_len) err[0] = '\0';
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        if (err) snprintf(err, err_len, "JSON invalide");
        return ESP_ERR_INVALID_ARG;
    }
    fusion_cfg_t cfg;
    taskENTER_CRITICAL(&s_lock);
    cfg = s_cfg;                                     // champs absents : inchanges
    taskEXIT_CRITICAL(&s_lock);
    bool ok = parse_config(root, &cfg, true, err, err_len);
    cJSON_Delete(root);
    if (!ok) return ESP_ERR_INVALID_ARG;
    esp_err_t e = save_config(&cfg);
    if (e != ESP_OK) {
        if (err) snprintf(err, err_len, "Ecriture de la configuration impossible");
        return e;
    }
    // Application par la boucle main (relecture du fichier, seule tache qui
    // touche aux horodatages du fichier)
    s_reload_requested = true;
    return ESP_OK;
}
