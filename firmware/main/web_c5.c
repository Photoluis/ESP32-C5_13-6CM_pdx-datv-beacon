/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 * Dashboard (B2): vedi web_c5.h. Struttura ripresa dalla pagina del beacon ESP32-S3 (access point, /api/status,
 * /api/set, /api/save), scritta da zero per la C5. */
#include "web_c5.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_status.h"
#include "rf_c5.h"
#include "lwip/sockets.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static pdx_bcn_cfg_t *cfg;
static const char *fw_version;
static httpd_handle_t server;
static volatile bool stop_req, client_seen;
static char state_txt[48] = "CONFIG";

void pdx_web_init(pdx_bcn_cfg_t *c, const char *fw) { cfg = c; fw_version = fw; }
bool pdx_web_running(void) { return server != NULL; }
void pdx_web_set_state(const char *s) { snprintf(state_txt, sizeof state_txt, "%s", s); }
bool pdx_web_take_stop(void) { const bool r = stop_req; stop_req = false; return r; }
bool pdx_web_client_seen(void) { return client_seen; }

static esp_err_t h_index(httpd_req_t *r)
{
    printf("WEB GET /\n");
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start);
}

static esp_err_t send_status(httpd_req_t *r)
{
    char j[640];
    const pdx_bcn_cfg_t *c = cfg;
    snprintf(j, sizeof j,
             "{\"fw\":\"%s\",\"state\":\"%s\",\"call\":\"%s\",\"appass\":\"%s\",\"f13\":%" PRIu32 ",\"f6\":%" PRIu32
             ",\"sr\":%" PRIu32 ",\"amp\":%" PRIi32 ",\"tx\":%u,\"pause\":%u,\"wait\":%u,\"bands\":%u,\"auto\":%u,"
             "\"ant13\":%u,\"ant6\":%u,\"gpio13\":%d,\"gpio6\":%d,\"gpiopol\":%u,\"gpioptt\":%d}",
             fw_version, state_txt, c->call, c->ap_pass, c->f13_khz, c->f6_khz, c->sr, c->amp, c->tx_s, c->pause_s,
             c->wait_s, c->bands, c->auto_tx, c->ant13, c->ant6, c->gpio13, c->gpio6, c->gpio_pol, c->gpio_ptt);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, j);
}

static esp_err_t h_status(httpd_req_t *r) { printf("WEB GET /api/status\n"); return send_status(r); }

/* corpo "chiave=valore&chiave=valore" (form url-encoded, solo caratteri semplici) */
static void urldecode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && s[1] && s[2]) { char h[3] = {s[1], s[2], 0}; *o++ = (char)strtol(h, NULL, 16); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}

static esp_err_t h_set(httpd_req_t *r)
{
    char body[512];
    const int n = httpd_req_recv(r, body, sizeof body - 1);
    if (n <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "corpo");
    body[n] = 0;
    char bad[160] = "";
    pdx_bcn_cfg_t t = *cfg;
    for (char *tok = strtok(body, "&"); tok; tok = strtok(NULL, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        urldecode(eq + 1);
        if (!pdx_bcn_set(&t, tok, eq + 1)) {
            strncat(bad, tok, sizeof bad - strlen(bad) - 2);
            strncat(bad, " ", sizeof bad - strlen(bad) - 1);
        }
    }
    if (bad[0]) {
        httpd_resp_set_status(r, "400 Bad Request");
        char m[200];
        snprintf(m, sizeof m, "{\"err\":\"%s\"}", bad);
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_sendstr(r, m);
    }
    *cfg = t;
    pdx_led_confirm();
    printf("WEB configurazione applicata\n");
    return send_status(r);
}

static esp_err_t h_save(httpd_req_t *r)
{
    const bool ok = pdx_bcn_save(cfg);
    printf("WEB configurazione %s\n", ok ? "salvata" : "NON salvata");
    if (ok) pdx_led_confirm();
    return ok ? send_status(r) : httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS");
}

static esp_err_t h_defaults(httpd_req_t *r)
{
    pdx_bcn_cfg_t d;
    pdx_bcn_defaults(&d);
    snprintf(d.call, sizeof d.call, "%s", cfg->call);       /* nominativo e password restano */
    snprintf(d.ap_pass, sizeof d.ap_pass, "%s", cfg->ap_pass);
    *cfg = d;
    return send_status(r);
}

static esp_err_t h_stop(httpd_req_t *r)
{
    stop_req = true;
    printf("WEB STOP/CONFIG richiesto\n");
    return send_status(r);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(400));                             /* la risposta deve arrivare al telefono */
    pdx_bcn_request_run(0, 1);
}

static esp_err_t h_start(httpd_req_t *r)
{
    printf("WEB START: riavvio e ciclo dalla banda 13 cm\n");
    pdx_web_set_state("START");
    send_status(r);
    xTaskCreate(restart_task, "rst", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void on_sta(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        client_seen = true;
        pdx_led_activity();
        printf("WEB telefono collegato all'access point\n");
    }
}

bool pdx_web_start(void)
{
    if (server) return true;
    static bool netif_done;
    if (!netif_done) {
        esp_netif_create_default_wifi_ap();
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_sta, NULL);
        netif_done = true;
    }
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "%s-BEACON", cfg->call);
    snprintf((char *)ap.ap.password, sizeof ap.ap.password, "%s", cfg->ap_pass);
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;
    pdx_rf_lend_bankB();                                       /* RAM con DMA per i pacchetti TCP */
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        printf("WEB ERR access point\n");
        return false;
    }
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 5120;
    hc.max_uri_handlers = 8;
    hc.max_open_sockets = 3;
    hc.lru_purge_enable = true;
    if (httpd_start(&server, &hc) != ESP_OK) {
        printf("WEB ERR server\n");
        server = NULL;
        return false;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/set", .method = HTTP_POST, .handler = h_set},
        {.uri = "/api/save", .method = HTTP_POST, .handler = h_save},
        {.uri = "/api/defaults", .method = HTTP_POST, .handler = h_defaults},
        {.uri = "/api/stop", .method = HTTP_POST, .handler = h_stop},
        {.uri = "/api/start", .method = HTTP_POST, .handler = h_start},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
    client_seen = false;
    stop_req = false;
    printf("WEB access point \"%s\" attivo: http://192.168.4.1\n", (char *)ap.ap.ssid);
    return true;
}

/* diagnosi: richiesta HTTP della C5 a sé stessa (127.0.0.1:80) */
void pdx_web_selftest(void)
{
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { printf("WEBTEST ERR socket (memoria?)\n"); return; }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(80)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval tv = {.tv_sec = 3};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { printf("WEBTEST ERR connect %d\n", errno); close(s); return; }
    const char *q = "GET /api/status HTTP/1.0\r\n\r\n";
    send(s, q, strlen(q), 0);
    char buf[200];
    const int n = recv(s, buf, sizeof buf - 1, 0);
    close(s);
    if (n <= 0) { printf("WEBTEST ERR nessuna risposta (%d)\n", errno); return; }
    buf[n] = 0;
    for (char *p = buf; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
    printf("WEBTEST OK %d byte: %.120s\n", n, buf);
}
