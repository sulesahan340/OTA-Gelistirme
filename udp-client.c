#include "contiki.h"
#include "net/routing/routing.h"
#include "random.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "ota-metadata.h"
#include "firmware_data.h"
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "sys/node-id.h"
#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define SEND_INTERVAL       (2 * CLOCK_SECOND)
#define ACK_TIMEOUT         (5 * CLOCK_SECOND)
#define MAX_RETRIES         3

/* OTA packet definitions */
#define OTA_BLOCK_SIZE      64
#define MSG_TYPE_DATA       0x01
#define MSG_TYPE_ACK        0x02
#define MSG_TYPE_NAK        0x03
#define MSG_TYPE_COMPLETE   0x04
#define MSG_TYPE_HASH_VERIFY 0x05

/* Packet header structure - sent as raw bytes */
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;
  uint16_t block_num;
  uint16_t total_blocks;
  uint16_t data_len;
  uint16_t block_crc16;
  uint8_t  data[OTA_BLOCK_SIZE];
} ota_packet_t;

static struct simple_udp_connection udp_conn;
static uint32_t rx_count = 0;

/* OTA transfer state */
static uint16_t current_block = 0;
static uint16_t total_blocks = 0;
static uint8_t  ack_received = 0;
static uint8_t  nak_received = 0;
static uint16_t nak_block_num = 0;
static uint8_t  transfer_complete = 0;
static uint8_t  transfer_started = 0;

static ota_boot_metadata_t boot_metadata = {
  .magic = OTA_IMAGE_MAGIC,
  .active_slot = OTA_SLOT_A,
  .candidate_slot = OTA_SLOT_NONE,
  .state_a = OTA_IMAGE_STATE_CONFIRMED,
  .state_b = OTA_IMAGE_STATE_EMPTY,
};

/*---------------------------------------------------------------------------*/
PROCESS(udp_client_process, "UDP client");
AUTOSTART_PROCESSES(&udp_client_process);
/*---------------------------------------------------------------------------*/
static uint16_t
compute_block_crc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0xFFFF;
  uint16_t i;
  for(i = 0; i < len; i++) {
    crc ^= data[i];
    crc = (crc >> 8) ^ (crc << 8);
  }
  return crc;
}
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  (void)c;
  (void)sender_port;
  (void)receiver_addr;
  (void)receiver_port;

  if(datalen < 1) {
    return;
  }

  uint8_t msg_type = data[0];

  if(msg_type == MSG_TYPE_ACK && datalen >= 3) {
    uint16_t acked_block;
    memcpy(&acked_block, &data[1], sizeof(uint16_t));
    LOG_INFO("ACK received for block %u\n", acked_block);
    ack_received = 1;
    rx_count++;
  } else if(msg_type == MSG_TYPE_NAK && datalen >= 3) {
    uint16_t req_block;
    memcpy(&req_block, &data[1], sizeof(uint16_t));
    LOG_INFO("NAK received, resend block %u\n", req_block);
    nak_received = 1;
    nak_block_num = req_block;
  }
}
/*---------------------------------------------------------------------------*/
static void
send_data_block(uint16_t block_num, const uip_ipaddr_t *dest)
{
  ota_packet_t pkt;
  uint32_t offset;
  uint16_t remaining;
  uint16_t copy_len;

  memset(&pkt, 0, sizeof(pkt));

  offset = (uint32_t)block_num * OTA_BLOCK_SIZE;
  remaining = FIRMWARE_IMAGE_SIZE - offset;
  copy_len = (remaining < OTA_BLOCK_SIZE) ? remaining : OTA_BLOCK_SIZE;

  pkt.msg_type = MSG_TYPE_DATA;
  pkt.block_num = block_num;
  pkt.total_blocks = total_blocks;
  pkt.data_len = copy_len;
  memcpy(pkt.data, &firmware_image[offset], copy_len);
  pkt.block_crc16 = compute_block_crc16(pkt.data, copy_len);

  /* Send header + actual data only (not full 64 bytes if last block is smaller) */
  uint16_t send_len = 9 + copy_len; /* 9 byte header + data */
  simple_udp_sendto(&udp_conn, &pkt, send_len, dest);

  if(block_num % 100 == 0) {
    LOG_INFO("Sent block %u/%u (%lu/%lu bytes)\n",
             block_num, total_blocks,
             (unsigned long)(offset + copy_len),
             (unsigned long)FIRMWARE_IMAGE_SIZE);
  }
}
/*---------------------------------------------------------------------------*/
static void
send_complete_message(const uip_ipaddr_t *dest)
{
  uint8_t buf[5];
  uint32_t full_crc;

  full_crc = ota_crc32_buffer(firmware_image, FIRMWARE_IMAGE_SIZE);

  buf[0] = MSG_TYPE_COMPLETE;
  memcpy(&buf[1], &full_crc, sizeof(uint32_t));

  simple_udp_sendto(&udp_conn, buf, sizeof(buf), dest);

  LOG_INFO("COMPLETE message sent. Full CRC32: 0x%08lx, size: %lu bytes\n",
           (unsigned long)full_crc, (unsigned long)FIRMWARE_IMAGE_SIZE);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;
  static struct etimer timeout_timer;
  static uint8_t retries;
  static uip_ipaddr_t dest_ipaddr;

  PROCESS_BEGIN();

  /* Calculate total blocks */
  total_blocks = (FIRMWARE_IMAGE_SIZE + OTA_BLOCK_SIZE - 1) / OTA_BLOCK_SIZE;

  LOG_INFO("OTA Client started. Firmware size: %lu bytes, blocks: %u, block size: %u\n",
           (unsigned long)FIRMWARE_IMAGE_SIZE, total_blocks, OTA_BLOCK_SIZE);

  /* Initialize UDP connection */
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);

  /* Wait for routing to establish */
  etimer_set(&periodic_timer, SEND_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    if(NETSTACK_ROUTING.node_is_reachable() &&
        NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
      break;
    }
    LOG_INFO("Waiting for routing...\n");
    etimer_set(&periodic_timer, SEND_INTERVAL);
  }

  /* Only node 2 performs the firmware transfer */
  if(node_id == 2) {

    LOG_INFO("Routing established. Starting OTA transfer...\n");
    transfer_started = 1;
    current_block = 0;

    /* Main transfer loop */
    while(!transfer_complete) {
      retries = 0;
      ack_received = 0;
      nak_received = 0;

      /* Send current block */
      send_data_block(current_block, &dest_ipaddr);

      /* Wait for ACK with timeout and retries */
      while(!ack_received && retries < MAX_RETRIES) {
        etimer_set(&timeout_timer, ACK_TIMEOUT);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timeout_timer) ||
                                  ack_received || nak_received);

        if(nak_received) {
          /* Resend requested block */
          LOG_INFO("Resending block %u (NAK)\n", nak_block_num);
          current_block = nak_block_num;
          nak_received = 0;
          send_data_block(current_block, &dest_ipaddr);
          retries++;
        } else if(!ack_received) {
          /* Timeout - resend */
          retries++;
          LOG_INFO("Timeout block %u, retry %u/%u\n",
                   current_block, retries, MAX_RETRIES);
          if(retries < MAX_RETRIES) {
            send_data_block(current_block, &dest_ipaddr);
          }
        }
      }

      if(!ack_received) {
        LOG_INFO("ERROR: Block %u failed after %u retries\n",
                 current_block, MAX_RETRIES);
        /* Try to continue anyway */
      }

      /* Move to next block */
      current_block++;

      if(current_block >= total_blocks) {
        /* All blocks sent - send completion message */
        LOG_INFO("All %u blocks sent. Sending COMPLETE...\n", total_blocks);

        /* Small delay before complete message */
        etimer_set(&periodic_timer, CLOCK_SECOND);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

        send_complete_message(&dest_ipaddr);
        transfer_complete = 1;

        LOG_INFO("Firmware aktarimi tamamlandi.\n");

        /* Update OTA metadata */
        {
          uint32_t fw_crc = ota_crc32_buffer(firmware_image, FIRMWARE_IMAGE_SIZE);
          if(ota_metadata_mark_verified(&boot_metadata, OTA_SLOT_B,
                                        2, FIRMWARE_IMAGE_SIZE, fw_crc) &&
             ota_metadata_stage_verified_image(&boot_metadata, OTA_SLOT_B)) {
            LOG_INFO("OTA metadata: Slot B staged for activation\n");
          }
        }
      }

      /* Small delay between blocks for network breathing room */
      if(!transfer_complete) {
        etimer_set(&periodic_timer, CLOCK_SECOND / 4);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
      }
    }

  } else {
    /* Non-sender nodes just idle */
    LOG_INFO("Node %u: not the sender, idling\n", node_id);
  }

  /* Keep process alive */
  while(1) {
    etimer_set(&periodic_timer, 60 * CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
