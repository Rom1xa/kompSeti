#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define PORT 8080
#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define MAX_TIME_STR 32
#define MAX_CLIENTS 32

typedef enum {
  MSG_HELLO = 1,
  MSG_WELCOME = 2,
  MSG_TEXT = 3,
  MSG_PING = 4,
  MSG_PONG = 5,
  MSG_BYE = 6,
  MSG_AUTH = 7,
  MSG_PRIVATE = 8,
  MSG_ERROR = 9,
  MSG_SERVER_INFO = 10,
  MSG_LIST = 11,
  MSG_HISTORY = 12,
  MSG_HISTORY_DATA = 13,
  MSG_HELP = 14
} MessageType;

#pragma pack(push, 1)
typedef struct {
  uint32_t length;           /* длина полезной части          */
  uint8_t type;              /* тип сообщения                 */
  uint32_t msg_id;           /* уникальный идентификатор      */
  char sender[MAX_NAME];     /* ник отправителя               */
  char receiver[MAX_NAME];   /* ник получателя или ""         */
  time_t timestamp;          /* время создания                */
  char payload[MAX_PAYLOAD]; /* текст / данные команды        */
} MessageEx;
#pragma pack(pop)

#define MSGEX_HEADER_SIZE                                                      \
  (sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t) + MAX_NAME +          \
   MAX_NAME + sizeof(time_t))

static inline void fmt_time(time_t ts, char *buf, size_t sz) {
  struct tm *tm_info = localtime(&ts);
  strftime(buf, sz, "%Y-%m-%d %H:%M:%S", tm_info);
}

static inline int msgex_send(int sock, uint8_t type, uint32_t msg_id,
                             const char *sender, const char *receiver,
                             const char *payload) {
  MessageEx msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = type;
  msg.msg_id = msg_id;
  msg.timestamp = time(NULL);
  if (sender)
    strncpy(msg.sender, sender, MAX_NAME - 1);
  if (receiver)
    strncpy(msg.receiver, receiver, MAX_NAME - 1);
  if (payload) {
    size_t plen = strlen(payload);
    if (plen >= MAX_PAYLOAD)
      plen = MAX_PAYLOAD - 1;
    memcpy(msg.payload, payload, plen);
    msg.length = (uint32_t)plen;
  }
  return (int)send(sock, &msg, sizeof(MessageEx), 0);
}

static inline int msgex_recv(int sock, MessageEx *msg) {
  memset(msg, 0, sizeof(*msg));
  int n = (int)recv(sock, msg, sizeof(MessageEx), MSG_WAITALL);
  if (n <= 0)
    return n;
  if (msg->length >= MAX_PAYLOAD)
    msg->length = MAX_PAYLOAD - 1;
  msg->payload[msg->length] = '\0';
  return n;
}

static inline void tcpip_log_recv(int bytes, const char *src_ip,
                                  const char *dst_ip, int src_port,
                                  int dst_port, const char *msg_type_str,
                                  const char *sender) {
  printf("[Network Access] frame arrived from NIC\n");
  printf("[Internet]       simulated IP hdr: src=%s dst=%s proto=6\n", src_ip,
         dst_ip);
  printf("[Transport]      simulated TCP hdr: src_port=%d dst_port=%d\n",
         src_port, dst_port);
  printf("[Transport]      recv() %d bytes via TCP\n", bytes);
  printf("[Application]    deserialize MessageEx -> %s from %s\n", msg_type_str,
         sender ? sender : "?");
  fflush(stdout);
}

static inline void tcpip_log_send(const char *dst_ip,
                                  const char *msg_type_str) {
  printf("[Application]    prepare %s\n", msg_type_str);
  printf("[Transport]      send() via TCP\n");
  printf("[Internet]       destination ip = %s\n", dst_ip);
  printf("[Network Access] frame sent to network interface\n");
  fflush(stdout);
}

static inline const char *msgtype_str(uint8_t type) {
  switch (type) {
  case MSG_HELLO:
    return "MSG_HELLO";
  case MSG_WELCOME:
    return "MSG_WELCOME";
  case MSG_TEXT:
    return "MSG_TEXT";
  case MSG_PING:
    return "MSG_PING";
  case MSG_PONG:
    return "MSG_PONG";
  case MSG_BYE:
    return "MSG_BYE";
  case MSG_AUTH:
    return "MSG_AUTH";
  case MSG_PRIVATE:
    return "MSG_PRIVATE";
  case MSG_ERROR:
    return "MSG_ERROR";
  case MSG_SERVER_INFO:
    return "MSG_SERVER_INFO";
  case MSG_LIST:
    return "MSG_LIST";
  case MSG_HISTORY:
    return "MSG_HISTORY";
  case MSG_HISTORY_DATA:
    return "MSG_HISTORY_DATA";
  case MSG_HELP:
    return "MSG_HELP";
  default:
    return "MSG_UNKNOWN";
  }
}
