// ============================================================================
// Web API - SensorFusion (agregation de sondes de temperature -> MQTT)
//   GET  /api/sensorfusion   etat (valeur fusionnee, sondes, configuration)
//   POST /api/sensorfusion   configuration (JSON, champs absents inchanges)
// Page : web_src/js/page_sensorfusion.js (Mesures -> SensorFusion)
// ============================================================================

#include "web_sensorfusion.h"
#include "web_server.h"
#include "SensorFusion.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BODY_MAX 2048

static esp_err_t send_status(httpd_req_t *req, const char *status, const char *msg)
{
    char buf[200];
    bool ok = strncmp(status, "200", 3) == 0;
    snprintf(buf, sizeof(buf), "{\"status\":\"%s\",\"ok\":%s,\"message\":\"%s\"}",
             ok ? "ok" : "error", ok ? "true" : "false", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t sf_get(httpd_req_t *req)
{
    char *json = sensor_fusion_status_json();
    if (!json) return send_status(req, "500 Internal Server Error", "Memoire insuffisante");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, json);
    free(json);
    return e;
}

static esp_err_t sf_post(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0) return send_status(req, "400 Bad Request", "Corps JSON attendu");
    if (len > BODY_MAX) return send_status(req, "413 Payload Too Large", "Corps trop long");
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) return send_status(req, "500 Internal Server Error", "Memoire insuffisante");
    int got = 0, timeouts = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (r <= 0) { free(buf); return send_status(req, "400 Bad Request", "Corps incomplet"); }
        got += r;
    }
    buf[got] = '\0';
    char err[96];
    esp_err_t e = sensor_fusion_set_config_json(buf, err, sizeof(err));
    free(buf);
    if (e == ESP_ERR_INVALID_ARG) return send_status(req, "400 Bad Request", err);
    if (e != ESP_OK) return send_status(req, "500 Internal Server Error", err[0] ? err : "Erreur");
    return send_status(req, "200 OK", "Configuration enregistree");
}

void web_sensorfusion_register(httpd_handle_t server)
{
    httpd_uri_t get = {};
    get.uri = "/api/sensorfusion";
    get.method = HTTP_GET;
    get.handler = sf_get;
    web_server_register_uri(server, &get);

    httpd_uri_t post = {};
    post.uri = "/api/sensorfusion";
    post.method = HTTP_POST;
    post.handler = sf_post;
    web_server_register_uri(server, &post);
}
