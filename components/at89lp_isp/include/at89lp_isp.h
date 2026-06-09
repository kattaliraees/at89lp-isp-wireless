#ifndef AT89LP_ISP_H
#define AT89LP_ISP_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration structure for the AT89LP ISP component
 */
typedef struct {
    int pin_mosi;       /**< GPIO for Master Out Slave In */
    int pin_miso;       /**< GPIO for Master In Slave Out */
    int pin_sck;        /**< GPIO for Serial Clock */
    int pin_ss;         /**< GPIO for Slave Select (Software CS) */
    int pin_rst;        /**< GPIO for Target Reset (Active Low) */
    uint32_t spi_speed_hz; /**< SPI clock frequency, e.g., 500000 (500kHz) */
} at89lp_isp_config_t;

/**
 * @brief Initialize the SPI bus and GPIO pins for ISP
 * 
 * @param config Pointer to configuration struct
 * @return esp_err_t ESP_OK on success
 */
esp_err_t at89lp_isp_init(const at89lp_isp_config_t *config);

/**
 * @brief Deinitialize the SPI bus and free resources
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t at89lp_isp_deinit(void);

/**
 * @brief Execute the ISP entry sequence to enable programming
 * 
 * @return esp_err_t ESP_OK if entry succeeded and signature is matched
 */
esp_err_t at89lp_isp_enter(void);

/**
 * @brief Execute the ISP exit sequence to return the target to CPU execution
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t at89lp_isp_exit(void);

/**
 * @brief Read the Atmel Signature Row
 * 
 * Reads the 3 identification bytes: Manufacturer ID, Family ID, and Device ID.
 * 
 * @param sig Pointer to buffer (must be at least 3 bytes)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t at89lp_isp_read_signature(uint8_t *sig);

/**
 * @brief Read the Status Register
 * 
 * @param status Pointer to store the read status byte
 * @return esp_err_t ESP_OK on success
 */
esp_err_t at89lp_isp_read_status(uint8_t *status);

/**
 * @brief Perform a full Chip Erase on the target device
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t at89lp_isp_chip_erase(void);

/**
 * @brief Write data to a Code page on the target device
 * 
 * Uses Page Buffer Load (0x51) followed by Write Code Page with Auto-Erase (0x70).
 * 
 * @param address The 16-bit starting address (must be page-aligned, e.g., multiple of 64)
 * @param data Pointer to the page data buffer
 * @param length Length of data (typically 64 bytes)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t at89lp_isp_write_page(uint16_t address, const uint8_t *data, size_t length);

/**
 * @brief Read data from a Code page on the target device
 * 
 * @param address The 16-bit starting address
 * @param data Pointer to the destination buffer
 * @param length Length of data to read
 * @return esp_err_t ESP_OK on success
 */
esp_err_t at89lp_isp_read_page(uint16_t address, uint8_t *data, size_t length);

/**
 * @brief Wait for the target chip to finish programming (BUSY bit in Status Register)
 * 
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return esp_err_t ESP_OK if programming finished, ESP_ERR_TIMEOUT if timed out
 */
esp_err_t at89lp_isp_wait_ready(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // AT89LP_ISP_H
