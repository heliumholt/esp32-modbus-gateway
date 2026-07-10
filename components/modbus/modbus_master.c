#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "modbus_master.h"
#include "uart_config.h"

static const char *TAG = "modbus";

#define MODBUS_FRAME_MAX  256
#define MODBUS_BROADCAST  0

/* ADU: [slave][fc][data...][crc_l][crc_h] */
/* For broadcast (slave=0), no response expected */

/* ================================================================
 * CRC16 — MODBUS polynomial 0xA001 (reversed)
 * ================================================================ */

static const uint16_t crc16_table[256] = {
    0x0000,0xC0C1,0xC181,0x0140,0xC301,0x03C0,0x0280,0xC241,0xC601,0x06C0,0x0780,0xC741,
    0x0500,0xC5C1,0xC481,0x0440,0xCC01,0x0CC0,0x0D80,0xCD41,0x0F00,0xCFC1,0xCE81,0x0E40,
    0x0A00,0xCAC1,0xCB81,0x0B40,0xC901,0x09C0,0x0880,0xC841,0xD801,0x18C0,0x1980,0xD941,
    0x1B00,0xDBC1,0xDA81,0x1A40,0x1E00,0xDEC1,0xDF81,0x1F40,0xDD01,0x1DC0,0x1C80,0xDC41,
    0x1400,0xD4C1,0xD581,0x1540,0xD701,0x17C0,0x1680,0xD641,0xD201,0x12C0,0x1380,0xD341,
    0x1100,0xD1C1,0xD081,0x1040,0xF001,0x30C0,0x3180,0xF141,0x3300,0xF3C1,0xF281,0x3240,
    0x3600,0xF6C1,0xF781,0x3740,0xF501,0x35C0,0x3480,0xF441,0x3C00,0xFCC1,0xFD81,0x3D40,
    0xFF01,0x3FC0,0x3E80,0xFE41,0xFA01,0x3AC0,0x3B80,0xFB41,0x3900,0xF9C1,0xF881,0x3840,
    0x2800,0xE8C1,0xE981,0x2940,0xEB01,0x2BC0,0x2A80,0xEA41,0xEE01,0x2EC0,0x2F80,0xEF41,
    0x2D00,0xEDC1,0xEC81,0x2C40,0xE401,0x24C0,0x2580,0xE541,0x2700,0xE7C1,0xE681,0x2640,
    0x2200,0xE2C1,0xE381,0x2340,0xE101,0x21C0,0x2080,0xE041,0xA001,0x60C0,0x6180,0xA141,
    0x6300,0xA3C1,0xA281,0x6240,0x6600,0xA6C1,0xA781,0x6740,0xA501,0x65C0,0x6480,0xA441,
    0x6C00,0xACC1,0xAD81,0x6D40,0xAF01,0x6FC0,0x6E80,0xAE41,0xAA01,0x6AC0,0x6B80,0xAB41,
    0x6900,0xA9C1,0xA881,0x6840,0x7800,0xB8C1,0xB981,0x7940,0xBB01,0x7BC0,0x7A80,0xBA41,
    0xBE01,0x7EC0,0x7F80,0xBF41,0x7D00,0xBDC1,0xBC81,0x7C40,0xB401,0x74C0,0x7580,0xB541,
    0x7700,0xB7C1,0xB681,0x7640,0x7200,0xB2C1,0xB381,0x7340,0xB101,0x71C0,0x7080,0xB041,
    0x5000,0x90C1,0x9181,0x5140,0x9301,0x53C0,0x5280,0x9241,0x9601,0x56C0,0x5780,0x9741,
    0x5500,0x95C1,0x9481,0x5440,0x9C01,0x5CC0,0x5D80,0x9D41,0x5F00,0x9FC1,0x9E81,0x5E40,
    0x5A00,0x9AC1,0x9B81,0x5B40,0x9901,0x59C0,0x5880,0x9841,0x8801,0x48C0,0x4980,0x8941,
    0x4B00,0x8BC1,0x8A81,0x4A40,0x4E00,0x8EC1,0x8F81,0x4F40,0x8D01,0x4DC0,0x4C80,0x8C41,
    0x4400,0x84C1,0x8581,0x4540,0x8701,0x47C0,0x4680,0x8641,0x8201,0x42C0,0x4380,0x8341,
    0x4100,0x81C1,0x8081,0x4040,
};

static uint16_t modbus_crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc16_table[(crc ^ buf[i]) & 0xFF];
    }
    return crc;
}

/* ================================================================
 * Frame send/recv helpers
 * ================================================================ */

/**
 * Send a MODBUS frame over RS485.
 * Toggles DE/RE to TX mode, sends, waits for TX complete, toggles back.
 */
static esp_err_t modbus_send_frame(const uint8_t *frame, int len)
{
    modbus_uart_set_tx_mode();
    /* RS485: after DE asserts, wait before sending */
    esp_rom_delay_us(50);
    int sent = uart_write_bytes(modbus_uart_get_port(), frame, len);
    uart_wait_tx_done(modbus_uart_get_port(), pdMS_TO_TICKS(100));
    modbus_uart_set_rx_mode();

    if (sent != len) {
        ESP_LOGE(TAG, "UART write failed: sent %d/%d bytes", sent, len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * Receive a MODBUS response frame with timeout.
 * Returns frame length on success, or negative on error/timeout.
 */
static int modbus_recv_frame(uint8_t *buf, int max_len, int timeout_ms)
{
    int total = 0;
    int64_t start = esp_timer_get_time() / 1000;

    /* Wait for at least slave address + FC */
    while (total < 2) {
        int64_t elapsed = (esp_timer_get_time() / 1000) - start;
        if (elapsed > timeout_ms) {
            if (total == 0)
                ESP_LOGW(TAG, "Recv timeout (no data within %d ms)", timeout_ms);
            return -1;
        }
        int n = uart_read_bytes(modbus_uart_get_port(), buf + total, max_len - total,
                                pdMS_TO_TICKS(50));
        if (n > 0) total += n;
        if (n < 0) return -1;
    }

    /* Determine expected frame length based on function code */
    uint8_t fc = buf[1];
    int expected_len = 0;

    if (fc == MODBUS_FC_READ_HOLDING || fc == MODBUS_FC_READ_INPUT) {
        /* Response: [slave][fc][byte_count][data...][crc][crc] */
        /* We need byte_count byte at offset 2 to determine length */
        while (total < 3) {
            int n = uart_read_bytes(modbus_uart_get_port(), buf + total, 1, pdMS_TO_TICKS(50));
            if (n > 0) total += n;
            if ((esp_timer_get_time() / 1000) - start > timeout_ms) return -1;
        }
        expected_len = 3 + buf[2] + 2;  /* slave+fc+bc+data+crc */
        if (expected_len > MODBUS_FRAME_MAX) {
            ESP_LOGW(TAG, "Bad byte_count 0x%02X → expected_len %d, clamping to %d",
                     buf[2], expected_len, MODBUS_FRAME_MAX);
            expected_len = MODBUS_FRAME_MAX;
        }
    } else if (fc == MODBUS_FC_WRITE_SINGLE) {
        expected_len = 8;  /* echo of request */
    } else if (fc == MODBUS_FC_WRITE_MULTI) {
        expected_len = 8;  /* [slave][fc][start_hi][start_lo][count_hi][count_lo][crc][crc] */
    } else if (fc & 0x80) {
        expected_len = 5;  /* exception: [slave][fc|0x80][exc_code][crc][crc] */
    } else {
        ESP_LOGW(TAG, "Unknown FC in response: 0x%02X", fc);
        return -1;
    }

    /* Read remaining bytes */
    while (total < expected_len) {
        int64_t elapsed = (esp_timer_get_time() / 1000) - start;
        if (elapsed > timeout_ms) {
            ESP_LOGW(TAG, "Recv incomplete: got %d/%d bytes", total, expected_len);
            return -1;
        }
        int n = uart_read_bytes(modbus_uart_get_port(), buf + total, expected_len - total,
                                pdMS_TO_TICKS(50));
        if (n > 0) total += n;
    }

    /* Validate CRC */
    if (total >= 2) {
        uint16_t received_crc = buf[total - 2] | (buf[total - 1] << 8);
        uint16_t computed_crc = modbus_crc16(buf, total - 2);
        if (received_crc != computed_crc) {
            ESP_LOGE(TAG, "CRC mismatch: got 0x%04X, expected 0x%04X",
                     received_crc, computed_crc);
            return -1;
        }
    }

    return total;
}

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t modbus_master_init(void)
{
    return modbus_uart_init();
}

esp_err_t modbus_read_holding_registers(uint8_t slave_addr, uint16_t start_reg,
                                         uint16_t count, uint16_t *results_out)
{
    if (count < 1 || count > 125) return ESP_ERR_INVALID_ARG;
    if (!results_out) return ESP_ERR_INVALID_ARG;

    uint8_t frame[8];
    frame[0] = slave_addr;
    frame[1] = MODBUS_FC_READ_HOLDING;
    frame[2] = (start_reg >> 8) & 0xFF;
    frame[3] = start_reg & 0xFF;
    frame[4] = (count >> 8) & 0xFF;
    frame[5] = count & 0xFF;

    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = crc >> 8;

    esp_err_t ret = modbus_send_frame(frame, 8);
    if (ret != ESP_OK) return ret;

    /* Broadcast: no response expected */
    if (slave_addr == MODBUS_BROADCAST) return ESP_OK;

    uint8_t resp[MODBUS_FRAME_MAX];
    int rlen = modbus_recv_frame(resp, sizeof(resp), MODBUS_DEFAULT_TIMEOUT_MS);
    if (rlen < 0) return ESP_ERR_TIMEOUT;

    /* Check slave address and function code */
    if (resp[0] != slave_addr) return ESP_ERR_INVALID_RESPONSE;

    /* Exception? */
    if (resp[1] & 0x80) {
        ESP_LOGW(TAG, "MODBUS exception: slave=%d, fc=%d, code=%d",
                 slave_addr, MODBUS_FC_READ_HOLDING, resp[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t byte_count = resp[2];
    if (byte_count != count * 2) {
        ESP_LOGE(TAG, "Byte count mismatch: expected %d, got %d", count * 2, byte_count);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Extract register values (big-endian → host) */
    for (int i = 0; i < count; i++) {
        results_out[i] = (resp[3 + i * 2] << 8) | resp[4 + i * 2];
    }

    return ESP_OK;
}

esp_err_t modbus_read_input_registers(uint8_t slave_addr, uint16_t start_reg,
                                       uint16_t count, uint16_t *results_out)
{
    if (count < 1 || count > 125) return ESP_ERR_INVALID_ARG;
    if (!results_out) return ESP_ERR_INVALID_ARG;

    uint8_t frame[8];
    frame[0] = slave_addr;
    frame[1] = MODBUS_FC_READ_INPUT;
    frame[2] = (start_reg >> 8) & 0xFF;
    frame[3] = start_reg & 0xFF;
    frame[4] = (count >> 8) & 0xFF;
    frame[5] = count & 0xFF;

    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = crc >> 8;

    esp_err_t ret = modbus_send_frame(frame, 8);
    if (ret != ESP_OK) return ret;

    if (slave_addr == MODBUS_BROADCAST) return ESP_OK;

    uint8_t resp[MODBUS_FRAME_MAX];
    int rlen = modbus_recv_frame(resp, sizeof(resp), MODBUS_DEFAULT_TIMEOUT_MS);
    if (rlen < 0) return ESP_ERR_TIMEOUT;

    if (resp[0] != slave_addr) return ESP_ERR_INVALID_RESPONSE;

    if (resp[1] & 0x80) {
        ESP_LOGW(TAG, "MODBUS exception: slave=%d, fc=%d, code=%d",
                 slave_addr, MODBUS_FC_READ_INPUT, resp[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t byte_count = resp[2];
    if (byte_count != count * 2) return ESP_ERR_INVALID_SIZE;

    for (int i = 0; i < count; i++) {
        results_out[i] = (resp[3 + i * 2] << 8) | resp[4 + i * 2];
    }

    return ESP_OK;
}

esp_err_t modbus_write_single_register(uint8_t slave_addr, uint16_t reg_addr,
                                        uint16_t value)
{
    uint8_t frame[8];
    frame[0] = slave_addr;
    frame[1] = MODBUS_FC_WRITE_SINGLE;
    frame[2] = (reg_addr >> 8) & 0xFF;
    frame[3] = reg_addr & 0xFF;
    frame[4] = (value >> 8) & 0xFF;
    frame[5] = value & 0xFF;

    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = crc >> 8;

    esp_err_t ret = modbus_send_frame(frame, 8);
    if (ret != ESP_OK) return ret;

    if (slave_addr == MODBUS_BROADCAST) return ESP_OK;

    uint8_t resp[MODBUS_FRAME_MAX];
    int rlen = modbus_recv_frame(resp, sizeof(resp), MODBUS_DEFAULT_TIMEOUT_MS);
    if (rlen < 0) return ESP_ERR_TIMEOUT;

    if (resp[0] != slave_addr) return ESP_ERR_INVALID_RESPONSE;

    if (resp[1] & 0x80) {
        ESP_LOGW(TAG, "MODBUS exception: slave=%d, fc=%d, code=%d",
                 slave_addr, MODBUS_FC_WRITE_SINGLE, resp[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* FC06 echo: should match exactly */
    if (memcmp(frame, resp, 6) != 0) {
        ESP_LOGW(TAG, "FC06 echo mismatch");
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t modbus_write_multiple_registers(uint8_t slave_addr, uint16_t start_reg,
                                           const uint16_t *values, uint16_t count)
{
    if (count < 1 || count > 123) return ESP_ERR_INVALID_ARG;
    if (!values) return ESP_ERR_INVALID_ARG;

    int data_len = count * 2;
    int frame_len = 7 + data_len;  /* slave+fc+addr2+count2+bytecnt+data+crc2 */

    uint8_t frame[MODBUS_FRAME_MAX];
    frame[0] = slave_addr;
    frame[1] = MODBUS_FC_WRITE_MULTI;
    frame[2] = (start_reg >> 8) & 0xFF;
    frame[3] = start_reg & 0xFF;
    frame[4] = (count >> 8) & 0xFF;
    frame[5] = count & 0xFF;
    frame[6] = (uint8_t)data_len;

    for (int i = 0; i < count; i++) {
        frame[7 + i * 2]     = (values[i] >> 8) & 0xFF;
        frame[7 + i * 2 + 1] = values[i] & 0xFF;
    }

    uint16_t crc = modbus_crc16(frame, frame_len - 2);
    frame[frame_len - 2] = crc & 0xFF;
    frame[frame_len - 1] = crc >> 8;

    esp_err_t ret = modbus_send_frame(frame, frame_len);
    if (ret != ESP_OK) return ret;

    if (slave_addr == MODBUS_BROADCAST) return ESP_OK;

    uint8_t resp[MODBUS_FRAME_MAX];
    int rlen = modbus_recv_frame(resp, sizeof(resp), MODBUS_DEFAULT_TIMEOUT_MS);
    if (rlen < 0) return ESP_ERR_TIMEOUT;

    if (resp[0] != slave_addr) return ESP_ERR_INVALID_RESPONSE;

    if (resp[1] & 0x80) {
        ESP_LOGW(TAG, "MODBUS exception: slave=%d, fc=%d, code=%d",
                 slave_addr, MODBUS_FC_WRITE_MULTI, resp[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}
