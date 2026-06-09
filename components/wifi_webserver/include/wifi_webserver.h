#ifndef WIFI_WEBSERVER_H
#define WIFI_WEBSERVER_H

#include "esp_err.h"
#include <stdbool.h>

#define WIFI_WEBSERVER_MAX_BUTTONS 8

typedef struct {
    const char *id;     /**< ID returned in the callback, e.g. "detect" */
    const char *label;  /**< Button text displayed in the UI, e.g. "Detect Target" */
    bool danger;        /**< If true, styled with red highlighting */
} wifi_webserver_button_t;

typedef struct {
    const char *title;            /**< Title of the web page */
    const char *ssid;             /**< SSID for WiFi SoftAP */
    const char *password;         /**< Password for WiFi SoftAP (empty string or NULL for open) */
    bool upload_enabled;          /**< Enable/disable file upload option */
    const char *upload_accept;    /**< Allowed extensions filter (e.g. ".hex") */
    
    wifi_webserver_button_t buttons[WIFI_WEBSERVER_MAX_BUTTONS];
    int button_count;
} wifi_webserver_config_t;

typedef struct {
    /**
     * @brief Triggered when a button is clicked in the browser
     * 
     * @param action_id ID of the clicked button
     * @param out_msg Buffer to write a response status message to print in the web console log
     * @param max_msg_len Size of out_msg buffer
     */
    esp_err_t (*on_button_action)(const char *action_id, char *out_msg, size_t max_msg_len);
    
    /**
     * @brief Triggered when a file is uploaded
     * 
     * @param filename Name of the uploaded file
     * @param file_data Raw string contents of the uploaded file
     * @param file_len Length of file_data in bytes
     * @param out_msg Buffer to write a response status message to print in the web console log
     * @param max_msg_len Size of out_msg buffer
     */
    esp_err_t (*on_file_upload)(const char *filename, const char *file_data, size_t file_len, char *out_msg, size_t max_msg_len);
} wifi_webserver_callbacks_t;

/**
 * @brief Initialize and start WiFi SoftAP and HTTP Web Server
 * 
 * @param config Web server and layout settings
 * @param callbacks Event actions
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_webserver_start(const wifi_webserver_config_t *config, const wifi_webserver_callbacks_t *callbacks);

/**
 * @brief Stop the web server and WiFi SoftAP
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_webserver_stop(void);

#endif // WIFI_WEBSERVER_H
