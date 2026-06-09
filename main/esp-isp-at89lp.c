#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "at89lp_isp.h"
#include "wifi_webserver.h"

// Define GPIO pins for ESP32 -> AT89LP6440 connections
#define CONFIG_ISP_PIN_MOSI 22
#define CONFIG_ISP_PIN_MISO 23
#define CONFIG_ISP_PIN_SCK  19
#define CONFIG_ISP_PIN_SS   21
#define CONFIG_ISP_PIN_RST  18
#define CONFIG_ISP_SPI_SPEED 100000 // 100 kHz

static const char *TAG = "app_main";

#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif

// Static configuration structure for ISP driver
static const at89lp_isp_config_t isp_cfg = {
    .pin_mosi = CONFIG_ISP_PIN_MOSI,
    .pin_miso = CONFIG_ISP_PIN_MISO,
    .pin_sck  = CONFIG_ISP_PIN_SCK,
    .pin_ss   = CONFIG_ISP_PIN_SS,
    .pin_rst  = CONFIG_ISP_PIN_RST,
    .spi_speed_hz = CONFIG_ISP_SPI_SPEED,
};

/* ----------------- Web UI Callbacks ----------------- */

/**
 * @brief Handles dynamic buttons (Detect Chip & Erase Chip)
 */
static esp_err_t app_on_button_action(const char *action_id, char *out_msg, size_t max_msg_len) {
    ESP_LOGI(TAG, "Button Action Event: '%s'", action_id);
    
    // Initialize the ISP driver on-demand
    esp_err_t err = at89lp_isp_init(&isp_cfg);
    if (err != ESP_OK) {
        snprintf(out_msg, max_msg_len, "Failed to init ISP driver: %s", esp_err_to_name(err));
        return err;
    }
    
    if (strcmp(action_id, "detect") == 0) {
        uint8_t sig[3] = {0};
        
        err = at89lp_isp_enter();
        if (err != ESP_OK) {
            snprintf(out_msg, max_msg_len, "Failed to enter ISP: %s", esp_err_to_name(err));
            at89lp_isp_deinit();
            return err;
        }
        
        err = at89lp_isp_read_signature(sig);
        at89lp_isp_exit();
        at89lp_isp_deinit();
        
        if (err != ESP_OK) {
            snprintf(out_msg, max_msg_len, "Failed to read signature: %s", esp_err_to_name(err));
            return err;
        }
        
        const char *chip_name = "Unknown Device";
        if (sig[0] == 0x1E) {
            chip_name = "Atmel AT89LP device";
        }
        
        snprintf(out_msg, max_msg_len, "Detected: %s (Sig: [0x%02X, 0x%02X, 0x%02X])", 
                 chip_name, sig[0], sig[1], sig[2]);
        return ESP_OK;
        
    } else if (strcmp(action_id, "erase") == 0) {
        err = at89lp_isp_enter();
        if (err != ESP_OK) {
            snprintf(out_msg, max_msg_len, "Failed to enter ISP: %s", esp_err_to_name(err));
            at89lp_isp_deinit();
            return err;
        }
        
        err = at89lp_isp_chip_erase();
        at89lp_isp_exit();
        at89lp_isp_deinit();
        
        if (err != ESP_OK) {
            snprintf(out_msg, max_msg_len, "Failed to erase: %s", esp_err_to_name(err));
            return err;
        }
        
        snprintf(out_msg, max_msg_len, "Chip erased successfully!");
        return ESP_OK;
    }
    
    at89lp_isp_deinit();
    snprintf(out_msg, max_msg_len, "Unknown action ID: %s", action_id);
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Handles file upload stream processing (now receives raw binary data directly)
 */
static esp_err_t app_on_file_upload(const char *filename, const char *file_data, size_t file_len, char *out_msg, size_t max_msg_len) {
    ESP_LOGI(TAG, "File Upload Event (Binary): '%s' (%d bytes)", filename, (int)file_len);
    
    if (file_len == 0) {
        snprintf(out_msg, max_msg_len, "Error: Uploaded file is empty");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Allocate 64KB target flash buffer dynamically
    uint8_t *flash_buffer = malloc(65536);
    if (flash_buffer == NULL) {
        snprintf(out_msg, max_msg_len, "Failed to allocate 64KB target buffer in ESP32 RAM");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize buffer with 0xFF (blank erased state) and copy the binary data
    memset(flash_buffer, 0xFF, 65536);
    size_t bytes_to_copy = MIN(file_len, 65536);
    memcpy(flash_buffer, file_data, bytes_to_copy);
    
    // Initialize ISP driver on-demand
    esp_err_t err = at89lp_isp_init(&isp_cfg);
    if (err != ESP_OK) {
        free(flash_buffer);
        snprintf(out_msg, max_msg_len, "Failed to init ISP driver: %s", esp_err_to_name(err));
        return err;
    }
    
    // Enter ISP Mode
    err = at89lp_isp_enter();
    if (err != ESP_OK) {
        at89lp_isp_deinit();
        free(flash_buffer);
        snprintf(out_msg, max_msg_len, "Failed to connect to target: %s", esp_err_to_name(err));
        return err;
    }
    
    // Erase chip prior to programming
    err = at89lp_isp_chip_erase();
    if (err != ESP_OK) {
        at89lp_isp_exit();
        at89lp_isp_deinit();
        free(flash_buffer);
        snprintf(out_msg, max_msg_len, "Erase prior to programming failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // Program page by page (64 bytes per page) based on actual binary size
    uint32_t pages = (bytes_to_copy + 63) / 64;
    ESP_LOGI(TAG, "Programming %d pages...", (int)pages);
    
    for (uint32_t page = 0; page < pages; page++) {
        uint16_t addr = page * 64;
        
        // Optimize: skip writing pages that are entirely 0xFF
        bool page_has_data = false;
        for (int i = 0; i < 64; i++) {
            if (flash_buffer[addr + i] != 0xFF) {
                page_has_data = true;
                break;
            }
        }
        
        if (page_has_data) {
            err = at89lp_isp_write_page(addr, &flash_buffer[addr], 64);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Page write error at 0x%04X: %s", addr, esp_err_to_name(err));
                uint8_t status_val = 0;
                at89lp_isp_read_status(&status_val);
                at89lp_isp_exit();
                at89lp_isp_deinit();
                free(flash_buffer);
                snprintf(out_msg, max_msg_len, "Failed writing page at 0x%04X: %s (status=0x%02X)", addr, esp_err_to_name(err), status_val);
                return err;
            }
        }
    }
    
    // Verify target flash memory by reading back and doing memcmp
    ESP_LOGI(TAG, "Verifying target flash contents...");
    uint8_t verify_buf[64];
    for (uint32_t page = 0; page < pages; page++) {
        uint16_t addr = page * 64;
        err = at89lp_isp_read_page(addr, verify_buf, 64);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Page read error at 0x%04X: %s", addr, esp_err_to_name(err));
            at89lp_isp_exit();
            at89lp_isp_deinit();
            free(flash_buffer);
            snprintf(out_msg, max_msg_len, "Failed reading page at 0x%04X for verification: %s", addr, esp_err_to_name(err));
            return err;
        }
        
        if (memcmp(verify_buf, &flash_buffer[addr], 64) != 0) {
            ESP_LOGE(TAG, "Verification mismatch at address 0x%04X", addr);
            at89lp_isp_exit();
            at89lp_isp_deinit();
            free(flash_buffer);
            snprintf(out_msg, max_msg_len, "Verification Mismatch at address 0x%04X!", addr);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    
    // Exit ISP mode and boot target CPU, then deinit
    at89lp_isp_exit();
    at89lp_isp_deinit();
    free(flash_buffer);
    
    snprintf(out_msg, max_msg_len, "Programmed %d bytes. Flash Verification Successful!", (int)bytes_to_copy);
    return ESP_OK;
}

void app_main(void) {
    // 1. Initialize NVS (needed for WiFi configurations)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Configure Web Server Settings
    wifi_webserver_config_t web_cfg = {
        .title = "AT89LP6440 ISP Programmer",
        .ssid = "ESP32-AT89LP-ISP",
        .password = "", // open AP
        .upload_enabled = true,
        .upload_accept = ".hex",
        .buttons = {
            { .id = "detect", .label = "Detect Target", .danger = false },
            { .id = "erase",  .label = "Erase Chip",    .danger = true }
        },
        .button_count = 2
    };
    
    // Register UI Events Callbacks
    wifi_webserver_callbacks_t web_callbacks = {
        .on_button_action = app_on_button_action,
        .on_file_upload = app_on_file_upload
    };

    // 3. Start WiFi SoftAP and HTTP Web Server
    ESP_LOGI(TAG, "Starting Wireless Control AP and HTTP Server...");
    ESP_ERROR_CHECK(wifi_webserver_start(&web_cfg, &web_callbacks));
}
