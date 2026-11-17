/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 网络自检：把「连不上 API」拆成可判读的几段。
 * 依次测量 DNS 解析、TCP 建连、完整 HTTPS 请求，各跑三轮；
 * 同时输出 RSSI、信道、开机以来的断连次数和最后一次断连原因码。
 *
 * 判断思路：
 *   - RSSI 低于 -78 dBm 且丢包/超时 → 信号问题
 *   - DNS 快、TCP 快、HTTPS 报 reset/超时 → 链路或对端问题，不是 Wi-Fi 弱
 *   - 三轮全 200/4xx → 网络正常，之前的失败来自限流或对端偶发
 */
#include "http_server_priv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_claw.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "netdiag";

#define NETDIAG_ROUNDS             3
#define NETDIAG_CONNECT_TIMEOUT_MS 4000
#define NETDIAG_HTTP_TIMEOUT_MS    7000

typedef struct {
    int dns_ms;      /* 解析耗时，-1 表示失败 */
    int tcp_ms;      /* TCP 握手耗时，-1 表示失败 */
    int http_ms;     /* 完整 HTTPS 请求耗时，-1 表示失败 */
    int status;      /* HTTP 状态码，0 表示没有拿到响应 */
    int err;         /* esp_err_t 或 errno，便于定位 */
} netdiag_round_t;

/* 自检本身要连三轮 TLS，堆紧张时跑它只会加重问题，所以同一时刻只允许一次 */
static bool s_netdiag_busy;

static int64_t netdiag_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* 从 base_url 里取出主机名和端口，只接受 http/https */
static bool netdiag_parse_url(const char *url, char *host, size_t host_size,
                              int *port, bool *use_tls)
{
    const char *p = url;
    const char *colon = NULL;
    const char *slash = NULL;
    size_t host_len = 0;

    if (!url || !host || !port || !use_tls || host_size == 0) {
        return false;
    }
    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        *use_tls = true;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        *use_tls = false;
    } else {
        return false;
    }

    slash = strchr(p, '/');
    host_len = slash ? (size_t)(slash - p) : strlen(p);
    if (host_len == 0 || host_len >= host_size) {
        return false;
    }

    colon = memchr(p, ':', host_len);
    if (colon) {
        host_len = (size_t)(colon - p);
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) {
            return false;
        }
    } else {
        *port = *use_tls ? 443 : 80;
    }
    if (host_len == 0) {
        return false;
    }

    memcpy(host, p, host_len);
    host[host_len] = '\0';
    return true;
}

/* 只测 DNS 解析耗时：解析卡住时后续一切都会超时，先把它单独拎出来 */
static int netdiag_dns(const char *host, int port, int *err_out)
{
    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    char port_str[8];
    int64_t start = 0;
    int rc = 0;
    int elapsed = -1;

    snprintf(port_str, sizeof(port_str), "%d", port);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    start = netdiag_now_ms();
    rc = getaddrinfo(host, port_str, &hints, &res);
    elapsed = (int)(netdiag_now_ms() - start);

    if (rc != 0 || !res) {
        if (err_out) *err_out = rc;
        if (res) freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return elapsed;
}

/* 只做 TCP 握手，不建 TLS：用来区分「连不上」是网络层还是应用层 */
static int netdiag_tcp_connect(const char *host, int port, int *err_out)
{
    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    char port_str[8];
    int64_t start = 0;
    int fd = -1;
    int rc = 0;
    int elapsed = -1;

    snprintf(port_str, sizeof(port_str), "%d", port);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || !res) {
        if (err_out) *err_out = rc;
        return -1;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        if (err_out) *err_out = errno;
        freeaddrinfo(res);
        return -1;
    }

    /* 非阻塞 connect + select，避免信号差时卡死在默认超时上 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    start = netdiag_now_ms();
    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        struct timeval tv;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = NETDIAG_CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (NETDIAG_CONNECT_TIMEOUT_MS % 1000) * 1000;
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc > 0) {
            int so_err = 0;
            socklen_t len = sizeof(so_err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) == 0 && so_err != 0) {
                errno = so_err;
                rc = -1;
            }
        } else {
            /* 超时或 select 出错 */
            if (err_out) *err_out = rc == 0 ? ETIMEDOUT : errno;
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
    }

    elapsed = (int)(netdiag_now_ms() - start);
    if (rc < 0) {
        if (err_out) *err_out = errno;
        elapsed = -1;
    }

    close(fd);
    freeaddrinfo(res);
    return elapsed;
}

/* 完整走一遍和模型请求相同的链路（含 TLS 与证书校验） */
static int netdiag_http_once(const char *url, int *status_out, int *err_out)
{
    esp_http_client_config_t cfg = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_OK;
    int64_t start = 0;
    int elapsed = -1;

    cfg.url = url;
    cfg.timeout_ms = NETDIAG_HTTP_TIMEOUT_MS;
    /* 和 LLM 传输层保持一致的缓冲区大小，测出来的耗时才有可比性 */
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;

    client = esp_http_client_init(&cfg);
    if (!client) {
        if (err_out) *err_out = ESP_ERR_NO_MEM;
        return -1;
    }

    start = netdiag_now_ms();
    err = esp_http_client_perform(client);
    elapsed = (int)(netdiag_now_ms() - start);

    if (err != ESP_OK) {
        if (err_out) *err_out = err;
        if (status_out) *status_out = 0;
        esp_http_client_cleanup(client);
        return -1;
    }

    if (status_out) *status_out = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return elapsed;
}

static void netdiag_add_wifi(cJSON *root, const http_server_wifi_status_t *wifi)
{
    cJSON *obj = cJSON_CreateObject();

    if (!obj) return;
    cJSON_AddBoolToObject(obj, "connected", wifi->wifi_connected);
    cJSON_AddNumberToObject(obj, "rssi", wifi->rssi);
    cJSON_AddNumberToObject(obj, "channel", wifi->channel);
    cJSON_AddNumberToObject(obj, "reconnect_count", wifi->reconnect_count);
    cJSON_AddNumberToObject(obj, "last_disconnect_reason", wifi->last_disconnect_reason);
    cJSON_AddItemToObject(root, "wifi", obj);
}

static esp_err_t netdiag_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wifi_status_t wifi = {0};
    netdiag_round_t rounds[NETDIAG_ROUNDS];
    char base_url[APP_CLAW_STR_LEN] = {0};
    char host[96] = {0};
    bool use_tls = true;
    int port = 0;
    int ok_count = 0;
    int dns_sum = 0;
    int dns_n = 0;
    int tcp_sum = 0;
    int tcp_n = 0;
    int http_sum = 0;
    int http_n = 0;
    cJSON *root = NULL;
    cJSON *arr = NULL;
    cJSON *summary = NULL;
    cJSON *heap = NULL;

    memset(rounds, 0, sizeof(rounds));

    esp_err_t err = app_claw_get_llm_base_url(base_url, sizeof(base_url));
    if (err != ESP_OK || base_url[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "LLM base URL is not configured");
    }
    if (!netdiag_parse_url(base_url, host, sizeof(host), &port, &use_tls)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "LLM base URL is not a valid http(s) URL");
    }

    if (s_netdiag_busy) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "another diagnostic is running");
    }
    /* 一轮 TLS 大约要 20 KB，堆不够时先拒绝，别把仅剩的内存也耗掉 */
    if (esp_get_free_heap_size() < 45 * 1024) {
        ESP_LOGW(TAG, "skip diagnostic: heap too low");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "not enough memory, retry later");
    }
    s_netdiag_busy = true;

    if (ctx->services.get_wifi_status) {
        ctx->services.get_wifi_status(&wifi);
    }

    for (int i = 0; i < NETDIAG_ROUNDS; i++) {
        int err_code = 0;

        rounds[i].dns_ms = netdiag_dns(host, port, &err_code);
        rounds[i].tcp_ms = netdiag_tcp_connect(host, port, &err_code);
        rounds[i].http_ms = netdiag_http_once(base_url, &rounds[i].status, &err_code);
        rounds[i].err = err_code;

        if (rounds[i].http_ms >= 0) {
            ok_count++;
        }
        if (rounds[i].dns_ms >= 0) { dns_sum += rounds[i].dns_ms; dns_n++; }
        if (rounds[i].tcp_ms >= 0) { tcp_sum += rounds[i].tcp_ms; tcp_n++; }
        if (rounds[i].http_ms >= 0) { http_sum += rounds[i].http_ms; http_n++; }

        /* 三轮之间留一点间隔，避免瞬时突发全都撞在同一个拥塞窗口上 */
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    root = cJSON_CreateObject();
    if (!root) {
        s_netdiag_busy = false;
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "target", base_url);
    cJSON_AddStringToObject(root, "host", host);
    cJSON_AddNumberToObject(root, "port", port);
    cJSON_AddBoolToObject(root, "tls", use_tls);
    netdiag_add_wifi(root, &wifi);

    arr = cJSON_AddArrayToObject(root, "rounds");
    for (int i = 0; i < NETDIAG_ROUNDS && arr; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) break;
        cJSON_AddNumberToObject(item, "dns_ms", rounds[i].dns_ms);
        cJSON_AddNumberToObject(item, "tcp_ms", rounds[i].tcp_ms);
        cJSON_AddNumberToObject(item, "http_ms", rounds[i].http_ms);
        cJSON_AddNumberToObject(item, "status", rounds[i].status);
        cJSON_AddNumberToObject(item, "err", rounds[i].err);
        cJSON_AddItemToArray(arr, item);
    }

    summary = cJSON_AddObjectToObject(root, "summary");
    if (summary) {
        cJSON_AddNumberToObject(summary, "rounds", NETDIAG_ROUNDS);
        cJSON_AddNumberToObject(summary, "ok", ok_count);
        cJSON_AddNumberToObject(summary, "fail", NETDIAG_ROUNDS - ok_count);
        cJSON_AddNumberToObject(summary, "avg_dns_ms", dns_n ? dns_sum / dns_n : -1);
        cJSON_AddNumberToObject(summary, "avg_tcp_ms", tcp_n ? tcp_sum / tcp_n : -1);
        cJSON_AddNumberToObject(summary, "avg_http_ms", http_n ? http_sum / http_n : -1);
    }

    heap = cJSON_AddObjectToObject(root, "heap");
    if (heap) {
        cJSON_AddNumberToObject(heap, "free", esp_get_free_heap_size());
        cJSON_AddNumberToObject(heap, "min_free", esp_get_minimum_free_heap_size());
        cJSON_AddNumberToObject(heap,
                                "largest_block",
                                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    }

    ESP_LOGI(TAG, "diagnose %s:%d ok=%d/%d", host, port, ok_count, NETDIAG_ROUNDS);
    s_netdiag_busy = false;
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_netdiag_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/netdiag", .method = HTTP_GET, .handler = netdiag_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", handlers[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
