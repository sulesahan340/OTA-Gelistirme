#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* Increase UDP buffer size for firmware blocks */
#define UIP_CONF_BUFFER_SIZE 240

/* Increase queuebuf for reliability */
#define QUEUEBUF_CONF_NUM 8

/* Logging - reduce noise from network stack */
#define LOG_CONF_LEVEL_RPL LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_TCPIP LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_IPV6 LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_6LOWPAN LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_MAC LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_FRAMER LOG_LEVEL_WARN

#endif /* PROJECT_CONF_H_ */
