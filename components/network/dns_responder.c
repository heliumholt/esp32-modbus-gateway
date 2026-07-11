#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "dns_responder.h"

static const char *TAG = "dns";

/* DNS header fields (byte offsets) */
#define DNS_HDR_FLAGS_HI   2   /* flags high byte (QR bit = bit 7) */
#define DNS_HDR_QDCOUNT_HI 4   /* question count high byte */
#define DNS_HDR_QDCOUNT_LO 5   /* question count low byte */

/* DNS response flags */
#define DNS_FLAG_QR_BIT    0x80  /* QR=1 means response */
#define DNS_FLAG_QR_RESP   0x80  /* QR=1 in response */
#define DNS_FLAG_OPCODE    0x00

/* DNS record types */
#define DNS_TYPE_A         0x0001
#define DNS_CLASS_IN       0x0001

/* Captive portal IP: 192.168.4.1 */
#define CAPTIVE_IP_BYTES   { 192, 168, 4, 1 }

#define DNS_PORT            53
#define DNS_BUF_SIZE        512
/* Max safe payload to append answer without overflow: 512 - 16 bytes answer */
#define DNS_MAX_QUERY_LEN  (DNS_BUF_SIZE - 16)

static TaskHandle_t s_dns_task = NULL;
static volatile bool s_dns_running = false;

/* ================================================================
 * Byte-safe read helpers (avoid unaligned uint16_t* casts)
 * ================================================================ */

static inline uint16_t read_u16_be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline void write_u16_be(uint8_t *p, uint16_t val)
{
    p[0] = (val >> 8) & 0xFF;
    p[1] = val & 0xFF;
}

/* ================================================================
 * DNS task
 * ================================================================ */

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS responder started on port %d", DNS_PORT);

    uint8_t rx_buf[DNS_BUF_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (s_dns_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(sock + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                          (struct sockaddr *)&client_addr, &client_len);

        /* Minimum DNS header is 12 bytes */
        if (len < 12) continue;

        /* --- Validation --- */

        /* 1. Skip responses (QR bit set) — prevent DNS reflection */
        if (rx_buf[DNS_HDR_FLAGS_HI] & DNS_FLAG_QR_BIT) continue;

        /* 2. Only handle queries with exactly 1 question */
        uint16_t qdcount = read_u16_be(&rx_buf[DNS_HDR_QDCOUNT_HI]);
        if (qdcount != 1) continue;

        /* 3. Bounds check — ensure we have room to append the answer */
        if (len > DNS_MAX_QUERY_LEN) {
            ESP_LOGW(TAG, "DNS query too large (%d bytes), skipping", len);
            continue;
        }

        /* Build response in-place */
        uint8_t tx_buf[DNS_BUF_SIZE];
        memcpy(tx_buf, rx_buf, len);

        /* Set flags: QR=1 (response), OPCODE=0 (standard), keep transaction ID */
        tx_buf[DNS_HDR_FLAGS_HI] = DNS_FLAG_QR_RESP | DNS_FLAG_OPCODE;
        tx_buf[DNS_HDR_FLAGS_HI + 1] = 0x00;  /* clear remaining flags */

        /* Set answer count = 1 (byte-safe) */
        write_u16_be(&tx_buf[6], 1);
        /* Set authority count = 0, additional count = 0 */
        write_u16_be(&tx_buf[8], 0);
        write_u16_be(&tx_buf[10], 0);

        /* Append answer record */
        int ans_pos = len;
        /* NAME: pointer to question name at offset 12 */
        tx_buf[ans_pos++] = 0xC0;
        tx_buf[ans_pos++] = 0x0C;
        /* TYPE = A (1) */
        write_u16_be(&tx_buf[ans_pos], DNS_TYPE_A);
        ans_pos += 2;
        /* CLASS = IN (1) */
        write_u16_be(&tx_buf[ans_pos], DNS_CLASS_IN);
        ans_pos += 2;
        /* TTL = 300 seconds */
        tx_buf[ans_pos++] = 0x00;
        tx_buf[ans_pos++] = 0x00;
        tx_buf[ans_pos++] = 0x01;
        tx_buf[ans_pos++] = 0x2C;
        /* RDLENGTH = 4 */
        tx_buf[ans_pos++] = 0x00;
        tx_buf[ans_pos++] = 0x04;
        /* RDATA = 192.168.4.1 */
        uint8_t ip[] = CAPTIVE_IP_BYTES;
        memcpy(&tx_buf[ans_pos], ip, 4);
        ans_pos += 4;

        sendto(sock, tx_buf, ans_pos, 0,
               (struct sockaddr *)&client_addr, client_len);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS responder stopped");
    vTaskDelete(NULL);
}

/* ================================================================
 * Public
 * ================================================================ */

void dns_server_start(void)
{
    if (s_dns_running) return;
    s_dns_running = true;
    xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 3, &s_dns_task);
}

void dns_server_stop(void)
{
    if (!s_dns_running) return;
    s_dns_running = false;
    if (s_dns_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
}
