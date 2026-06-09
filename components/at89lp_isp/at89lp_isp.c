#include "at89lp_isp.h"
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AT89LP_ISP";

#define SPI_HOST_ID SPI2_HOST

static spi_device_handle_t spi_handle = NULL;
static at89lp_isp_config_t isp_config;
static bool is_initialized = false;
static bool bus_initialized = false;


/**
 * @brief Helper to wrap an ISP command with software SS control
 */
static esp_err_t send_isp_command(const uint8_t *cmd, size_t cmd_len, const uint8_t *tx_data, uint8_t *rx_data, size_t data_len) {
    if (spi_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    size_t total_len = cmd_len + data_len;
    if (total_len > 128) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t tx_buf[128];
    uint8_t rx_buf[128];
    
    memcpy(tx_buf, cmd, cmd_len);
    if (tx_data != NULL) {
        memcpy(tx_buf + cmd_len, tx_data, data_len);
    } else {
        memset(tx_buf + cmd_len, 0, data_len);
    }
    memset(rx_buf, 0, total_len);
    
    // 1. Pull SS low to start the transaction frame
    gpio_set_level(isp_config.pin_ss, 0);
    esp_rom_delay_us(1); // t_SSE setup delay
    
    // 2. Perform a single SPI transaction
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = total_len * 8; // In bits
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;
    
    esp_err_t err = spi_device_polling_transmit(spi_handle, &t);
    
    // 3. Pull SS high to finish the transaction frame
    esp_rom_delay_us(1); // t_SSD lag delay
    gpio_set_level(isp_config.pin_ss, 1);
    
    if (err == ESP_OK && rx_data != NULL) {
        memcpy(rx_data, rx_buf + cmd_len, data_len);
    }
    
    return err;
}

esp_err_t at89lp_isp_init(const at89lp_isp_config_t *config) {
    if (is_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    isp_config = *config;
    
    // Configure SS and RST GPIO pins as Outputs
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << config->pin_ss) | (1ULL << config->pin_rst),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SS/RST GPIOs: %s", esp_err_to_name(err));
        return err;
    }
    
    // Set initial levels: SS=1 (inactive), RST=1 (inactive, CPU running)
    gpio_set_level(config->pin_ss, 1);
    gpio_set_level(config->pin_rst, 1);
    
    // Configure SPI Bus
    spi_bus_config_t buscfg = {
        .miso_io_num = config->pin_miso,
        .mosi_io_num = config->pin_mosi,
        .sclk_io_num = config->pin_sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    
    err = spi_bus_initialize(SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(err));
        return err;
    }
    bus_initialized = true;
    
    // Add SPI Device (Software SS, Mode 0, MSB first)
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = config->spi_speed_hz,
        .mode = 0,                  // Mode 0 (CPOL=0, CPHA=0)
        .spics_io_num = -1,         // We control SS manually in software
        .queue_size = 1,
        .flags = 0,
    };
    
    err = spi_bus_add_device(SPI_HOST_ID, &devcfg, &spi_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(err));
        spi_bus_free(SPI_HOST_ID);
        bus_initialized = false;
        return err;
    }
    
    is_initialized = true;
    ESP_LOGI(TAG, "AT89LP ISP Driver initialized successfully.");
    return ESP_OK;
}

esp_err_t at89lp_isp_deinit(void) {
    if (!is_initialized) {
        return ESP_OK;
    }
    
    // Ensure target reset is released before deinit
    gpio_set_level(isp_config.pin_rst, 1);
    
    if (spi_handle) {
        spi_bus_remove_device(spi_handle);
        spi_handle = NULL;
    }
    
    if (bus_initialized) {
        spi_bus_free(SPI_HOST_ID);
        bus_initialized = false;
    }
    
    // Reset pins to high impedance
    gpio_reset_pin(isp_config.pin_ss);
    gpio_reset_pin(isp_config.pin_rst);
    
    is_initialized = false;
    ESP_LOGI(TAG, "AT89LP ISP Driver deinitialized.");
    return ESP_OK;
}

esp_err_t at89lp_isp_enter(void) {
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Entering ISP mode...");
    
    // Reset Entry Sequence:
    // 1. Pull SS high, SCK low (already low by SPI Mode 0 configuration)
    gpio_set_level(isp_config.pin_ss, 1);
    
    // 2. Drive RST active (low)
    gpio_set_level(isp_config.pin_rst, 0);
    
    // 3. Wait settling time (t_STL = 100ns minimum, we wait 10ms to let power stabilize)
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 4. Send Program Enable command: 0xAA, 0x55, 0xAC, 0x53, dummy 0x00
    uint8_t cmd[4] = {0xAA, 0x55, 0xAC, 0x53};
    uint8_t tx_dummy = 0x00;
    uint8_t rx_miso = 0x00;
    
    esp_err_t err = send_isp_command(cmd, 4, &tx_dummy, &rx_miso, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ISP Entry SPI transaction failed");
        return err;
    }
    
    // Target must return the echo 0x53 in response to the dummy byte
    if (rx_miso != 0x53) {
        ESP_LOGE(TAG, "ISP Enable failed! MISO returned 0x%02X (expected 0x53)", rx_miso);
        // Release reset so target doesn't hang in reset state
        gpio_set_level(isp_config.pin_rst, 1);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    ESP_LOGI(TAG, "ISP Mode entered successfully.");
    return ESP_OK;
}

esp_err_t at89lp_isp_exit(void) {
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Exiting ISP mode...");
    
    // Exit Sequence:
    // 1. Pull SS high
    gpio_set_level(isp_config.pin_ss, 1);
    esp_rom_delay_us(10); // Hold delay
    
    // 2. Release RST (drive high) to boot the CPU
    gpio_set_level(isp_config.pin_rst, 1);
    
    ESP_LOGI(TAG, "ISP Mode exited. Target is running.");
    return ESP_OK;
}

esp_err_t at89lp_isp_read_status(uint8_t *status) {
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    
    uint8_t cmd[5] = {0xAA, 0x55, 0x60, 0x00, 0x00};
    uint8_t tx_dummy = 0x00;
    
    return send_isp_command(cmd, 5, &tx_dummy, status, 1);
}

esp_err_t at89lp_isp_wait_ready(uint32_t timeout_ms) {
    uint32_t elapsed = 0;
    uint8_t status = 0;
    
    while (elapsed < timeout_ms) {
        esp_err_t err = at89lp_isp_read_status(&status);
        if (err != ESP_OK) {
            return err;
        }
        
        // Bit 0 of Status is BUSY_bar (1 = ready/idle, 0 = busy)
        if (status & 0x01) {
            return ESP_OK;
        }
        
        vTaskDelay(pdMS_TO_TICKS(5));
        elapsed += 5;
    }
    
    ESP_LOGE(TAG, "Timeout waiting for target ready (status=0x%02X)", status);
    return ESP_ERR_TIMEOUT;
}

esp_err_t at89lp_isp_read_signature(uint8_t *sig) {
    if (sig == NULL) return ESP_ERR_INVALID_ARG;
    
    uint8_t cmd[5] = {0xAA, 0x55, 0x38, 0x00, 0x00}; // Opcode 0x38, Address 0x0000
    uint8_t dummy[3] = {0x00, 0x00, 0x00};
    
    return send_isp_command(cmd, 5, dummy, sig, 3);
}

esp_err_t at89lp_isp_chip_erase(void) {
    ESP_LOGI(TAG, "Erasing target memory...");
    uint8_t cmd[3] = {0xAA, 0x55, 0x8A}; // Opcode 0x8A
    
    esp_err_t err = send_isp_command(cmd, 3, NULL, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    
    // Poll status register until ready (Chip erase takes up to 200-500ms)
    return at89lp_isp_wait_ready(1000);
}

esp_err_t at89lp_isp_write_page(uint16_t address, const uint8_t *data, size_t length) {
    if (data == NULL || length == 0 || length > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Step 1: Load Page Buffer (0x51)
    // Addr High: xxxx xxxx (0x00)
    // Addr Low:  00bb bbbb (address & 0x3F)
    uint8_t load_cmd[5] = {
        0xAA, 0x55, 0x51,
        0x00,
        (uint8_t)(address & 0x3F)
    };
    
    esp_err_t err = send_isp_command(load_cmd, 5, data, NULL, length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load page buffer at offset 0x%02X", (unsigned int)(address & 0x3F));
        return err;
    }
    
    // Step 2: Commit Page (0x70 or 0x50)
    // Low half-page (offset < 64) is written with Auto-Erase (0x70) to erase the entire 128-byte page.
    // High half-page (offset >= 64) is written without Auto-Erase (0x50) to preserve the low half-page.
    uint8_t opcode = 0x50;
    if ((address & 0x7F) < 64) {
        opcode = 0x70;
    }
    
    uint8_t commit_cmd[5] = {
        0xAA, 0x55, opcode,
        (uint8_t)(address >> 8),
        (uint8_t)(address & 0xFF)
    };
    
    // Send commit command with NO data bytes to program the loaded buffer
    err = send_isp_command(commit_cmd, 5, NULL, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit page write at 0x%04X", address);
        return err;
    }
    
    // Poll status until programming cycle finishes
    return at89lp_isp_wait_ready(100);
}

esp_err_t at89lp_isp_read_page(uint16_t address, uint8_t *data, size_t length) {
    if (data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t cmd[5] = {
        0xAA, 0x55, 0x30, // Opcode 0x30
        (uint8_t)(address >> 8),
        (uint8_t)(address & 0xFF)
    };
    
    return send_isp_command(cmd, 5, NULL, data, length);
}
