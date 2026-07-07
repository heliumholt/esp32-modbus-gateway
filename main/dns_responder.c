#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "dns_responder.h"

static const char *TAG = "dns";

/* DNS header flags */
#define DNS_FLAG_QR_RESPONSE  0x8000
#define DNS_FLAG_OPCODE_STD   0x0000
#define DNS_TYPE_A            0x0001
#define DNS_CLASS_IN          0x0001

/* Captive portal IP */
#define CAPTIVE_IP_BYTE_1  192
#define CAPTIVE_IP_BYTE_2  168
#define CAPTIVE_IP_BYTE_3  4
#define CAPTIVE_IP_BYTE_4  1

#define DNS_PORT            53
#define DNS_BUF_SIZE        512

static TaskHandle_t s_dns_task = NULL;
static volatile bool s_dns_running = false;

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
        if (len < 12) continue; /* DNS header is 12 bytes minimum */

        /* Build DNS response from the request */
        uint8_t tx_buf[DNS_BUF_SIZE];
        memcpy(tx_buf, rx_buf, len);

        /* Modify flags: set QR=1 (response), keep query ID */
        uint16_t *flags = (uint16_t *)(tx_buf + 2);
        *flags = htons(DNS_FLAG_QR_RESPONSE | DNS_FLAG_OPCODE_STD);

        /* Set answer count = 1 (we always answer with one A record) */
        uint16_t *ancount = (uint16_t *)(tx_buf + 6);
        *ancount = htons(1);

        /* Append answer: NAME (0xC00C = pointer to question name),
           TYPE=A, CLASS=IN, TTL=300, RDLENGTH=4, IP=192.168.4.1 */
        int ans_pos = len;
        /* Pointer to question name */
        tx_buf[ans_pos++] = 0xC0;
        tx_buf[ans_pos++] = 0x0C;
        /* TYPE = A */
        tx_buf[ans_pos++] = 0x00;
        tx_buf[ans_pos++] = DNS_TYPE_A;
        /* CLASS = IN */
        tx_buf[ans_pos++] = 0x00;
        tx_buf[ans_pos++] = DNS_CLASS_IN;
        /* TTL = 300 seconds */
        tx_buf[ans_pos++] = 0x00;
        tx_buf[ans_pos++] = 0x00;
        tx_buf[ans_pos++] = 0x01;
        tx_buf[ans_pos++] = 0x2C;
        /* RDLENGTH = 4 */
        tx_buf[ans_pos++] = 0x00;
        tx_buf[ans_pos++] = 0x04;
        /* IP = 192.168.4.1 */
        tx_buf[ans_pos++] = CAPTIVE_IP_BYTE_1;
        tx_buf[ans_pos++] = CAPTIVE_IP_BYTE_2;
        tx_buf[ans_pos++] = CAPTIVE_IP_BYTE_3;
        tx_buf[ans_pos++] = CAPTIVE_IP_BYTE_4;

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
