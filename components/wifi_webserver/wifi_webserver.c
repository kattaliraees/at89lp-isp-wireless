#include "wifi_webserver.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include "index_html.h"

static const char *TAG = "wifi_webserver";

static httpd_handle_t server_handle = NULL;
static wifi_webserver_config_t server_config;
static wifi_webserver_callbacks_t server_callbacks;
static bool wifi_initialized = false;

/* ----------------- HTTP Route Handlers ----------------- */

/**
 * @brief GET / - Serves index.html
 */
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, strlen(index_html));
}

/**
 * @brief GET /favicon.ico - Serves 204 No Content to suppress browser warnings
 */
static esp_err_t favicon_get_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/**
 * @brief GET /api/config - Returns the dynamic UI layout config as JSON
 */
static esp_err_t config_get_handler(httpd_req_t *req) {
    char *resp = malloc(1024);
    if (!resp) {
        return httpd_resp_send_500(req);
    }
    
    int len = snprintf(resp, 1024, "{\"title\":\"%s\",\"upload_enabled\":%s,\"upload_accept\":\"%s\",\"buttons\":[",
                       server_config.title ? server_config.title : "ESP32 Web Control",
                       server_config.upload_enabled ? "true" : "false",
                       server_config.upload_accept ? server_config.upload_accept : "");
                       
    for (int i = 0; i < server_config.button_count; i++) {
        len += snprintf(resp + len, 1024 - len, "%s{\"id\":\"%s\",\"label\":\"%s\",\"danger\":%s}",
                        (i > 0) ? "," : "",
                        server_config.buttons[i].id,
                        server_config.buttons[i].label,
                        server_config.buttons[i].danger ? "true" : "false");
    }
    
    snprintf(resp + len, 1024 - len, "]}");
    
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, resp);
    free(resp);
    return err;
}

/**
 * @brief POST /api/action?id=<action_id> - Triggers button action callback
 */
static esp_err_t action_post_handler(httpd_req_t *req) {
    char url_query[64];
    char action_id[32] = {0};
    char resp_msg[256] = {0};
    char response_json[384];
    
    if (httpd_req_get_url_query_str(req, url_query, sizeof(url_query)) == ESP_OK) {
        httpd_query_key_value(url_query, "id", action_id, sizeof(action_id));
    }
    
    if (strlen(action_id) == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing action ID\"}");
    }
    
    esp_err_t err = ESP_FAIL;
    if (server_callbacks.on_button_action) {
        err = server_callbacks.on_button_action(action_id, resp_msg, sizeof(resp_msg));
    } else {
        snprintf(resp_msg, sizeof(resp_msg), "No button callback registered");
    }
    
    // Clean JSON response message
    // Escape characters like double quotes or backslashes if necessary, but keep it basic for now
    if (err == ESP_OK) {
        snprintf(response_json, sizeof(response_json), "{\"status\":\"ok\",\"message\":\"%s\"}", resp_msg);
    } else {
        snprintf(response_json, sizeof(response_json), "{\"status\":\"error\",\"message\":\"%s\"}", resp_msg);
    }
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response_json);
}

/**
 * @brief POST /api/upload - Accepts file upload stream, buffers it, and triggers upload callback
 */
static esp_err_t upload_post_handler(httpd_req_t *req) {
    char url_query[128];
    char filename[64] = "uploaded_file.bin";
    char resp_msg[256] = {0};
    char response_json[384];
    
    if (httpd_req_get_url_query_str(req, url_query, sizeof(url_query)) == ESP_OK) {
        httpd_query_key_value(url_query, "filename", filename, sizeof(filename));
    }
    
    size_t total_len = req->content_len;
    if (total_len > 256 * 1024) { // 256KB max limit to prevent out-of-memory
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"File size too large (max 256KB)\"}");
    }
    
    char *file_buf = malloc(total_len + 1);
    if (!file_buf) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Out of memory on Web Server\"}");
    }
    
    size_t cur_len = 0;
    int received = 0;
    while (cur_len < total_len) {
        received = httpd_req_recv(req, file_buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(file_buf);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"File stream interrupted\"}");
        }
        cur_len += received;
    }
    file_buf[total_len] = '\0';
    
    esp_err_t err = ESP_FAIL;
    if (server_callbacks.on_file_upload) {
        err = server_callbacks.on_file_upload(filename, file_buf, total_len, resp_msg, sizeof(resp_msg));
    } else {
        snprintf(resp_msg, sizeof(resp_msg), "No upload callback registered");
    }
    
    free(file_buf);
    
    if (err == ESP_OK) {
        snprintf(response_json, sizeof(response_json), "{\"status\":\"ok\",\"message\":\"%s\"}", resp_msg);
    } else {
        snprintf(response_json, sizeof(response_json), "{\"status\":\"error\",\"message\":\"%s\"}", resp_msg);
    }
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response_json);
}

/* ----------------- WiFi SoftAP Setup ----------------- */

static void wifi_init_softap(const char *ssid, const char *password) {
    if (wifi_initialized) return;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);
    
    if (password && strlen(password) >= 8) {
        strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi SoftAP started. SSID: %s", ssid);
}

/* ----------------- Public API Functions ----------------- */

esp_err_t wifi_webserver_start(const wifi_webserver_config_t *config, const wifi_webserver_callbacks_t *callbacks) {
    if (server_handle != NULL) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }
    
    server_config = *config;
    server_callbacks = *callbacks;
    
    // 1. Initialize WiFi SoftAP
    wifi_init_softap(config->ssid ? config->ssid : "ESP32-Webserver", config->password);
    
    // 2. Start HTTP Server
    httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
    server_cfg.max_uri_handlers = 8;
    server_cfg.stack_size = 8192; // Large enough stack size for upload parsing
    
    ESP_LOGI(TAG, "Starting web server on port: '%d'...", server_cfg.server_port);
    esp_err_t err = httpd_start(&server_handle, &server_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }
    
    // Register URI Routes
    httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_handle, &root_uri);

    httpd_uri_t favicon_uri = {
        .uri      = "/favicon.ico",
        .method   = HTTP_GET,
        .handler  = favicon_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_handle, &favicon_uri);

    httpd_uri_t config_uri = {
        .uri      = "/api/config",
        .method   = HTTP_GET,
        .handler  = config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_handle, &config_uri);

    httpd_uri_t action_uri = {
        .uri      = "/api/action",
        .method   = HTTP_POST,
        .handler  = action_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_handle, &action_uri);

    httpd_uri_t upload_uri = {
        .uri      = "/api/upload",
        .method   = HTTP_POST,
        .handler  = upload_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_handle, &upload_uri);

    return ESP_OK;
}

esp_err_t wifi_webserver_stop(void) {
    if (server_handle == NULL) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping HTTP server...");
    esp_err_t err = httpd_stop(server_handle);
    if (err == ESP_OK) {
        server_handle = NULL;
    }
    
    return err;
}
