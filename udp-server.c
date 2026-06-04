#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "ota-metadata.h"
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "sys/node-id.h"
#include "sys/log.h"

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define MSG_TYPE_DATA       0x01
#define MSG_TYPE_ACK        0x02
#define MSG_TYPE_NAK        0x03
#define MSG_TYPE_COMPLETE   0x04
#define MSG_TYPE_HASH_VERIFY 0x05

#define OTA_BLOCK_SIZE      64

typedef struct __attribute__((packed)) {
  uint8_t  msg_type;
  uint16_t block_num;
  uint16_t total_blocks;
  uint16_t data_len;
  uint16_t block_crc16;
  uint8_t  data[OTA_BLOCK_SIZE];
} ota_packet_t;

typedef struct __attribute__((packed)) {
  uint8_t  msg_type;
  uint16_t block_num;
} ota_response_t;

static struct simple_udp_connection udp_conn;

static uint16_t next_expected_block = 0;
static uint32_t total_bytes_received = 0;
static uint8_t  transfer_active = 0;

/* Harici Flash Benzetimi (Simulated External Flash) */
#define SIM_FLASH_SIZE (150 * 1024)
static uint8_t sim_flash[SIM_FLASH_SIZE];

static uint16_t crc16_calc(const uint8_t *buf, uint16_t len) {
  uint16_t crc = 0xFFFF;
  uint16_t i;
  for(i = 0; i < len; i++) {
    crc ^= buf[i];
    crc = (crc >> 8) ^ (crc << 8);
  }
  return crc;
}

static uint32_t compute_flash_crc32(uint32_t file_size) {
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t i;
  int b;
  
  if(file_size > SIM_FLASH_SIZE) return 0;

  for(i = 0; i < file_size; i++) {
    crc ^= sim_flash[i];
    for(b = 0; b < 8; b++) {
      if(crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
      else crc >>= 1;
    }
  }
  return ~crc;
}

static void send_response(uint8_t msg_type, uint16_t block_num, const uip_ipaddr_t *dest_addr) {
  ota_response_t resp;
  resp.msg_type  = msg_type;
  resp.block_num = block_num;
  simple_udp_sendto(&udp_conn, &resp, sizeof(resp), dest_addr);
}

static void udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  if(datalen < 1) return;
  uint8_t msg_type = data[0];

  if(msg_type == MSG_TYPE_DATA) {
    const ota_packet_t *pkt;
    uint16_t computed_crc;

    if(datalen < 9) return;
    pkt = (const ota_packet_t *)data;

    if(pkt->block_num == 0 && !transfer_active) {
      memset(sim_flash, 0, sizeof(sim_flash)); /* Flash'i temizle */
      next_expected_block = 0;
      total_bytes_received = 0;
      transfer_active = 1;
      LOG_INFO("=== OTA Aktarimi Basladi ===\n");
    }

    if(pkt->block_num < next_expected_block) {
      send_response(MSG_TYPE_ACK, pkt->block_num, sender_addr);
      return;
    }
    if(pkt->block_num > next_expected_block) {
      send_response(MSG_TYPE_NAK, next_expected_block, sender_addr);
      return;
    }

    computed_crc = crc16_calc(pkt->data, pkt->data_len);
    if(computed_crc != pkt->block_crc16) {
      LOG_INFO("CRC Error Block %u\n", pkt->block_num);
      send_response(MSG_TYPE_NAK, pkt->block_num, sender_addr);
      return;
    }

    /* Flash Bellege Yazma Benzetimi */
    if(total_bytes_received + pkt->data_len <= SIM_FLASH_SIZE) {
      memcpy(&sim_flash[total_bytes_received], pkt->data, pkt->data_len);
      total_bytes_received += pkt->data_len;
      next_expected_block = pkt->block_num + 1;
    } else {
      LOG_INFO("Write Error: Simulated Flash is full!\n");
    }
    
    send_response(MSG_TYPE_ACK, pkt->block_num, sender_addr);

    if(pkt->block_num % 10 == 0) {
      LOG_INFO("Ilerleme: Blok %u / %u alindi (%lu bayt)\\n",
               pkt->block_num, pkt->total_blocks,
               (unsigned long)total_bytes_received);
    }

  } else if(msg_type == MSG_TYPE_COMPLETE) {
    uint32_t received_crc32, computed_crc32;
    if(datalen < 5) return;
    memcpy(&received_crc32, data + 1, sizeof(uint32_t));
    
    LOG_INFO("=== Aktarim Tamamlandi ===\n");
    LOG_INFO("Toplam alinan bayt: %lu\n", (unsigned long)total_bytes_received);
    
    computed_crc32 = compute_flash_crc32(total_bytes_received);
    
    if(computed_crc32 == received_crc32) {
      LOG_INFO("CRC32 dogrulama BASARILI!\n");
      LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
    } else {
      LOG_INFO("HATA: CRC32 uyusmazligi! Beklenen: 0x%08lX, Hesaplanan: 0x%08lX\n",
               (unsigned long)received_crc32, (unsigned long)computed_crc32);
    }
    transfer_active = 0;
  }
}

PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);
PROCESS_THREAD(udp_server_process, ev, data) {
  PROCESS_BEGIN();
  LOG_INFO("OTA Firmware Alici baslatildi\n");
  NETSTACK_ROUTING.root_start();
  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL, UDP_CLIENT_PORT, udp_rx_callback);
  PROCESS_END();
}
