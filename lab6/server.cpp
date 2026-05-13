#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

typedef struct {
  int delay_ms;
  double drop_rate;
  double corrupt_rate;
} SimConfig;

static SimConfig g_sim = {0, 0.0, 0.0};

typedef struct {
  int sock;
  char nickname[MAX_NAME];
  int authenticated;
  char ip[INET_ADDRSTRLEN];
  int port;
  uint32_t last_ids[32];
  int last_ids_pos;
} Client;

typedef struct {
  char sender[MAX_NAME];
  char receiver[MAX_NAME];
  char text[MAX_PAYLOAD];
  time_t timestamp;
  uint32_t msg_id;
} OfflineMsg;

static Client clients[MAX_CLIENTS];
static int client_count = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MAX_OFFLINE 256
static OfflineMsg offline_queue[MAX_OFFLINE];
static int offline_count = 0;
static pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t next_msg_id = 1;
static pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

#define HISTORY_FILE "chat_history.json"

static uint32_t gen_id(void) {
  pthread_mutex_lock(&id_mutex);
  uint32_t id = next_msg_id++;
  pthread_mutex_unlock(&id_mutex);
  return id;
}

static int sim_apply(MessageEx *msg) {
  if (g_sim.delay_ms > 0) {
    printf("[Transport][SIM] DELAY applied: %d ms\n", g_sim.delay_ms);
    fflush(stdout);
    usleep((useconds_t)g_sim.delay_ms * 1000);
  }
  if (g_sim.drop_rate > 0.0) {
    double r = (double)rand() / (double)RAND_MAX;
    if (r < g_sim.drop_rate) {
      printf("[Transport][SIM] DROP (id=%u, rate=%.1f)\n", msg->msg_id,
             g_sim.drop_rate);
      fflush(stdout);
      return 0;
    }
  }
  if (g_sim.corrupt_rate > 0.0 && msg->length > 0) {
    double r = (double)rand() / (double)RAND_MAX;
    if (r < g_sim.corrupt_rate) {
      int idx = rand() % (int)msg->length;
      msg->payload[idx] ^= 0xFF;
      printf("[Transport][SIM] CORRUPT payload (id=%u)\n", msg->msg_id);
      fflush(stdout);
    }
  }
  return 1;
}

static int client_seen_id(int sock, uint32_t msg_id) {
  pthread_mutex_lock(&clients_mutex);
  int result = 0;
  for (int i = 0; i < client_count; i++) {
    if (clients[i].sock != sock)
      continue;
    for (int j = 0; j < 32; j++) {
      if (clients[i].last_ids[j] == msg_id) {
        result = 1;
        break;
      }
    }
    if (!result) {
      clients[i].last_ids[clients[i].last_ids_pos] = msg_id;
      clients[i].last_ids_pos = (clients[i].last_ids_pos + 1) % 32;
    }
    break;
  }
  pthread_mutex_unlock(&clients_mutex);
  return result;
}

static void history_append(uint32_t msg_id, time_t ts, const char *sender,
                           const char *receiver, uint8_t type, const char *text,
                           int delivered, int is_offline) {
  pthread_mutex_lock(&history_mutex);
  FILE *f = fopen(HISTORY_FILE, "a");
  if (!f) {
    pthread_mutex_unlock(&history_mutex);
    return;
  }

  char safe_text[MAX_PAYLOAD * 2];
  int j = 0;
  for (int i = 0; text[i] && j < (int)sizeof(safe_text) - 2; i++) {
    if (text[i] == '"' || text[i] == '\\')
      safe_text[j++] = '\\';
    safe_text[j++] = text[i];
  }
  safe_text[j] = '\0';

  fprintf(f,
          "{\n"
          "  \"msg_id\": %u,\n"
          "  \"timestamp\": %ld,\n"
          "  \"sender\": \"%s\",\n"
          "  \"receiver\": \"%s\",\n"
          "  \"type\": \"%s\",\n"
          "  \"text\": \"%s\",\n"
          "  \"delivered\": %s,\n"
          "  \"is_offline\": %s\n"
          "}\n",
          msg_id, (long)ts, sender ? sender : "", receiver ? receiver : "",
          msgtype_str(type), safe_text, delivered ? "true" : "false",
          is_offline ? "true" : "false");
  fclose(f);
  pthread_mutex_unlock(&history_mutex);
}

static void history_read_last(int n, char *out, size_t out_sz) {
  pthread_mutex_lock(&history_mutex);
  FILE *f = fopen(HISTORY_FILE, "r");
  if (!f) {
    pthread_mutex_unlock(&history_mutex);
    snprintf(out, out_sz, "[No history available]");
    return;
  }

#define MAX_HIST_LINES 4096
  char *lines[MAX_HIST_LINES];
  int line_count = 0;
  char line_buf[512];

  while (fgets(line_buf, sizeof(line_buf), f) && line_count < MAX_HIST_LINES) {
    lines[line_count] = strdup(line_buf);
    line_count++;
  }
  fclose(f);
  pthread_mutex_unlock(&history_mutex);

  typedef struct {
    uint32_t msg_id;
    time_t ts;
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    char type_str[32];
    char text[MAX_PAYLOAD];
    int is_offline;
  } HistEntry;

#define MAX_HIST_ENTRIES 512
  static HistEntry entries[MAX_HIST_ENTRIES];
  int entry_count = 0;

  HistEntry cur;
  memset(&cur, 0, sizeof(cur));
  int in_obj = 0;

  for (int i = 0; i < line_count; i++) {
    char *l = lines[i];
    while (*l == ' ' || *l == '\t')
      l++;

    if (l[0] == '{') {
      memset(&cur, 0, sizeof(cur));
      in_obj = 1;
    } else if (l[0] == '}' && in_obj) {
      if (entry_count < MAX_HIST_ENTRIES)
        entries[entry_count++] = cur;
      in_obj = 0;
    } else if (in_obj) {
      char key[64] = "", val[MAX_PAYLOAD] = "";
      char *q1 = strchr(l, '"');
      if (!q1) {
        free(lines[i]);
        continue;
      }
      char *q2 = strchr(q1 + 1, '"');
      if (!q2) {
        free(lines[i]);
        continue;
      }
      size_t klen = (size_t)(q2 - q1 - 1);
      if (klen >= sizeof(key))
        klen = sizeof(key) - 1;
      strncpy(key, q1 + 1, klen);
      key[klen] = '\0';

      char *colon = strchr(q2 + 1, ':');
      if (!colon) {
        free(lines[i]);
        continue;
      }
      char *vstart = colon + 1;
      while (*vstart == ' ')
        vstart++;

      if (*vstart == '"') {
        char *vq2 = strchr(vstart + 1, '"');
        if (vq2) {
          size_t vlen = (size_t)(vq2 - vstart - 1);
          if (vlen >= sizeof(val))
            vlen = sizeof(val) - 1;
          strncpy(val, vstart + 1, vlen);
          val[vlen] = '\0';
        }
      } else {
        strncpy(val, vstart, sizeof(val) - 1);
        char *end = val + strlen(val) - 1;
        while (end >= val && (*end == '\n' || *end == '\r' || *end == ','))
          *end-- = '\0';
      }

      if (strcmp(key, "msg_id") == 0)
        cur.msg_id = (uint32_t)atoi(val);
      else if (strcmp(key, "timestamp") == 0)
        cur.ts = (time_t)atol(val);
      else if (strcmp(key, "sender") == 0)
        strncpy(cur.sender, val, MAX_NAME - 1);
      else if (strcmp(key, "receiver") == 0)
        strncpy(cur.receiver, val, MAX_NAME - 1);
      else if (strcmp(key, "type") == 0)
        strncpy(cur.type_str, val, 31);
      else if (strcmp(key, "text") == 0)
        strncpy(cur.text, val, MAX_PAYLOAD - 1);
      else if (strcmp(key, "is_offline") == 0)
        cur.is_offline = (strcmp(val, "true") == 0);
    }
    free(lines[i]);
  }

  int start = 0;
  if (n > 0 && entry_count > n)
    start = entry_count - n;

  out[0] = '\0';
  size_t pos = 0;
  for (int i = start; i < entry_count && pos < out_sz - 1; i++) {
    char ts_buf[MAX_TIME_STR];
    fmt_time(entries[i].ts, ts_buf, sizeof(ts_buf));

    char line[512];
    int is_priv = (strcmp(entries[i].type_str, "MSG_PRIVATE") == 0);
    int is_offline = entries[i].is_offline;

    if (is_offline && is_priv) {
      snprintf(line, sizeof(line), "[%s][id=%u][OFFLINE][%s -> %s]: %s\n",
               ts_buf, entries[i].msg_id, entries[i].sender,
               entries[i].receiver, entries[i].text);
    } else if (is_priv) {
      snprintf(line, sizeof(line), "[%s][id=%u][%s -> %s][PRIVATE]: %s\n",
               ts_buf, entries[i].msg_id, entries[i].sender,
               entries[i].receiver, entries[i].text);
    } else {
      snprintf(line, sizeof(line), "[%s][id=%u][%s]: %s\n", ts_buf,
               entries[i].msg_id, entries[i].sender, entries[i].text);
    }

    size_t llen = strlen(line);
    if (pos + llen >= out_sz - 1)
      break;
    memcpy(out + pos, line, llen);
    pos += llen;
  }
  out[pos] = '\0';

  if (pos == 0)
    snprintf(out, out_sz, "[History is empty]");
}

static int client_add(int sock, const char *ip, int port) {
  pthread_mutex_lock(&clients_mutex);
  if (client_count >= MAX_CLIENTS) {
    pthread_mutex_unlock(&clients_mutex);
    return -1;
  }
  clients[client_count].sock = sock;
  clients[client_count].nickname[0] = '\0';
  clients[client_count].authenticated = 0;
  strncpy(clients[client_count].ip, ip, INET_ADDRSTRLEN - 1);
  clients[client_count].port = port;
  memset(clients[client_count].last_ids, 0,
         sizeof(clients[client_count].last_ids));
  clients[client_count].last_ids_pos = 0;
  client_count++;
  pthread_mutex_unlock(&clients_mutex);
  return 0;
}

static void client_remove(int sock) {
  pthread_mutex_lock(&clients_mutex);
  for (int i = 0; i < client_count; i++) {
    if (clients[i].sock == sock) {
      clients[i] = clients[client_count - 1];
      client_count--;
      break;
    }
  }
  pthread_mutex_unlock(&clients_mutex);
}

static int client_find_sock(const char *nick) {
  pthread_mutex_lock(&clients_mutex);
  int result = -1;
  for (int i = 0; i < client_count; i++) {
    if (clients[i].authenticated && strcmp(clients[i].nickname, nick) == 0) {
      result = clients[i].sock;
      break;
    }
  }
  pthread_mutex_unlock(&clients_mutex);
  return result;
}

static int nick_is_unique(const char *nick) {
  for (int i = 0; i < client_count; i++) {
    if (clients[i].authenticated && strcmp(clients[i].nickname, nick) == 0)
      return 0;
  }
  return 1;
}

static void broadcast(int sender_sock, uint8_t type, uint32_t msg_id,
                      const char *sender, const char *text) {
  pthread_mutex_lock(&clients_mutex);
  for (int i = 0; i < client_count; i++) {
    if (clients[i].authenticated && clients[i].sock != sender_sock) {
      tcpip_log_send(clients[i].ip, msgtype_str(type));
      msgex_send(clients[i].sock, type, msg_id, sender, "", text);
    }
  }
  pthread_mutex_unlock(&clients_mutex);
}

static void broadcast_server_info(const char *text) {
  pthread_mutex_lock(&clients_mutex);
  for (int i = 0; i < client_count; i++) {
    if (clients[i].authenticated) {
      tcpip_log_send(clients[i].ip, "MSG_SERVER_INFO");
      msgex_send(clients[i].sock, MSG_SERVER_INFO, 0, "SERVER", "", text);
    }
  }
  pthread_mutex_unlock(&clients_mutex);
}

static void offline_store(const char *sender, const char *receiver,
                          const char *text, uint32_t msg_id) {
  pthread_mutex_lock(&offline_mutex);
  if (offline_count < MAX_OFFLINE) {
    strncpy(offline_queue[offline_count].sender, sender, MAX_NAME - 1);
    strncpy(offline_queue[offline_count].receiver, receiver, MAX_NAME - 1);
    strncpy(offline_queue[offline_count].text, text, MAX_PAYLOAD - 1);
    offline_queue[offline_count].timestamp = time(NULL);
    offline_queue[offline_count].msg_id = msg_id;
    offline_count++;
  }
  pthread_mutex_unlock(&offline_mutex);
}

static void offline_deliver(int sock, const char *nick) {
  pthread_mutex_lock(&offline_mutex);
  int delivered_any = 0;
  for (int i = 0; i < offline_count;) {
    if (strcmp(offline_queue[i].receiver, nick) == 0) {
      char ts_buf[MAX_TIME_STR];
      fmt_time(offline_queue[i].timestamp, ts_buf, sizeof(ts_buf));

      char display[MAX_PAYLOAD + 128];
      snprintf(display, sizeof(display), "[%s][id=%u][OFFLINE][%s -> %s]: %s",
               ts_buf, offline_queue[i].msg_id, offline_queue[i].sender,
               offline_queue[i].receiver, offline_queue[i].text);

      tcpip_log_send("127.0.0.1", "MSG_PRIVATE");
      msgex_send(sock, MSG_PRIVATE, offline_queue[i].msg_id,
                 offline_queue[i].sender, offline_queue[i].receiver, display);

      printf("[Application]    offline message id=%u delivered to %s\n",
             offline_queue[i].msg_id, nick);

      offline_queue[i] = offline_queue[offline_count - 1];
      offline_count--;
      delivered_any = 1;
    } else {
      i++;
    }
  }
  pthread_mutex_unlock(&offline_mutex);

  if (delivered_any)
    msgex_send(sock, MSG_SERVER_INFO, 0, "SERVER", "",
               "offline messages delivered");
  else
    printf("[Application]    no offline messages for %s\n", nick);
}

typedef struct {
  int sock;
  char ip[INET_ADDRSTRLEN];
  int port;
} ClientArg;

static void *handle_client(void *arg) {
  ClientArg *ca = (ClientArg *)arg;
  int sock = ca->sock;
  char ip[INET_ADDRSTRLEN];
  int port = ca->port;
  strncpy(ip, ca->ip, INET_ADDRSTRLEN - 1);
  free(ca);

  MessageEx msg;
  char sender_nick[MAX_NAME] = "";
  char buf[MAX_PAYLOAD + 256];

  tcpip_log_send(ip, "MSG_HELLO");
  msgex_send(sock, MSG_HELLO, gen_id(), "SERVER", "",
             "Welcome to the chat server");

  int n = msgex_recv(sock, &msg);
  if (n <= 0 || msg.type != MSG_WELCOME) {
    fprintf(stderr, "[SERVER] Handshake failed (sock %d)\n", sock);
    close(sock);
    client_remove(sock);
    return NULL;
  }
  tcpip_log_recv(n, ip, "127.0.0.1", port, PORT, "MSG_WELCOME", msg.sender);
  printf("[Application]    SYN -> ACK -> READY\n");
  printf("[Application]    turk powered TCP/IP stack initialized\n");

  n = msgex_recv(sock, &msg);
  if (n <= 0) {
    close(sock);
    client_remove(sock);
    return NULL;
  }
  tcpip_log_recv(n, ip, "127.0.0.1", port, PORT, msgtype_str(msg.type),
                 msg.sender);

  if (msg.type != MSG_AUTH || strlen(msg.payload) == 0) {
    printf("[Application]    authentication FAILED (missing nick)\n");
    tcpip_log_send(ip, "MSG_ERROR");
    msgex_send(sock, MSG_ERROR, 0, "SERVER", "",
               "Authentication required: send MSG_AUTH with nickname");
    close(sock);
    client_remove(sock);
    return NULL;
  }

  strncpy(sender_nick, msg.payload, MAX_NAME - 1);
  sender_nick[MAX_NAME - 1] = '\0';

  pthread_mutex_lock(&clients_mutex);
  int unique = nick_is_unique(sender_nick);
  if (unique) {
    for (int i = 0; i < client_count; i++) {
      if (clients[i].sock == sock) {
        strncpy(clients[i].nickname, sender_nick, MAX_NAME - 1);
        clients[i].authenticated = 1;
        break;
      }
    }
  }
  pthread_mutex_unlock(&clients_mutex);

  if (!unique) {
    printf("[Application]    authentication FAILED (nick taken)\n");
    tcpip_log_send(ip, "MSG_ERROR");
    msgex_send(sock, MSG_ERROR, 0, "SERVER", "", "Nickname already taken");
    close(sock);
    client_remove(sock);
    return NULL;
  }

  printf("[Application]    authentication success: %s\n", sender_nick);
  snprintf(buf, sizeof(buf), "User [%s] connected", sender_nick);
  printf("%s\n", buf);

  tcpip_log_send(ip, "MSG_SERVER_INFO");
  msgex_send(sock, MSG_SERVER_INFO, 0, "SERVER", sender_nick,
             "Authentication successful");
  broadcast(sock, MSG_SERVER_INFO, 0, "SERVER", buf);

  offline_deliver(sock, sender_nick);

  while (1) {
    n = msgex_recv(sock, &msg);
    if (n <= 0) {
      snprintf(buf, sizeof(buf), "User [%s] disconnected", sender_nick);
      printf("%s\n", buf);
      broadcast_server_info(buf);
      break;
    }

    tcpip_log_recv(n, ip, "127.0.0.1", port, PORT, msgtype_str(msg.type),
                   sender_nick);

    if (!sim_apply(&msg))
      continue;

    if (msg.type == MSG_TEXT || msg.type == MSG_PRIVATE ||
        msg.type == MSG_PING) {
      if (client_seen_id(sock, msg.msg_id)) {
        printf("[Application][DEDUP] duplicate ignored (id=%u)\n", msg.msg_id);
        fflush(stdout);
        continue;
      }
    }

    switch (msg.type) {

    case MSG_TEXT: {
      printf("[Application][ACK] process MSG_TEXT (id=%u)\n", msg.msg_id);
      uint32_t mid = gen_id();
      char ts_buf[MAX_TIME_STR];
      fmt_time(time(NULL), ts_buf, sizeof(ts_buf));

      snprintf(buf, sizeof(buf), "[%s][id=%u][%s]: %s", ts_buf, mid,
               sender_nick, msg.payload);
      printf("%s\n", buf);

      broadcast(sock, MSG_TEXT, mid, sender_nick, buf);
      tcpip_log_send(ip, "MSG_TEXT");
      msgex_send(sock, MSG_TEXT, mid, sender_nick, "", buf);

      printf("[Transport][ACK] send MSG_ACK (id=%u)\n", msg.msg_id);
      msgex_send(sock, MSG_ACK, msg.msg_id, "SERVER", sender_nick, "");

      history_append(mid, time(NULL), sender_nick, "", MSG_TEXT, msg.payload, 1,
                     0);
      break;
    }

    case MSG_PRIVATE: {
      printf("[Application][ACK] process MSG_PRIVATE (id=%u)\n", msg.msg_id);
      uint32_t mid = gen_id();

      char target_nick[MAX_NAME] = "";
      char private_text[MAX_PAYLOAD] = "";

      if (strlen(msg.receiver) > 0) {
        strncpy(target_nick, msg.receiver, MAX_NAME - 1);
        strncpy(private_text, msg.payload, MAX_PAYLOAD - 1);
      } else {
        char *colon = strchr(msg.payload, ':');
        if (!colon) {
          tcpip_log_send(ip, "MSG_ERROR");
          msgex_send(sock, MSG_ERROR, 0, "SERVER", sender_nick,
                     "Invalid format. Use: /w <nick> <message>");
          break;
        }
        size_t nlen = (size_t)(colon - msg.payload);
        if (nlen == 0 || nlen >= MAX_NAME) {
          tcpip_log_send(ip, "MSG_ERROR");
          msgex_send(sock, MSG_ERROR, 0, "SERVER", sender_nick,
                     "Invalid nickname in private message");
          break;
        }
        strncpy(target_nick, msg.payload, nlen);
        target_nick[nlen] = '\0';
        strncpy(private_text, colon + 1, MAX_PAYLOAD - 1);
      }

      int target_sock = client_find_sock(target_nick);

      char ts_buf[MAX_TIME_STR];
      fmt_time(time(NULL), ts_buf, sizeof(ts_buf));

      if (target_sock == -1) {
        printf("[Application]    receiver %s is offline — store in queue\n",
               target_nick);
        offline_store(sender_nick, target_nick, private_text, mid);
        history_append(mid, time(NULL), sender_nick, target_nick, MSG_PRIVATE,
                       private_text, 0, 1);

        snprintf(buf, sizeof(buf), "Message to '%s' stored (offline delivery)",
                 target_nick);
        tcpip_log_send(ip, "MSG_SERVER_INFO");
        msgex_send(sock, MSG_SERVER_INFO, 0, "SERVER", sender_nick, buf);
      } else {
        snprintf(buf, sizeof(buf), "[%s][id=%u][PRIVATE][%s -> %s]: %s", ts_buf,
                 mid, sender_nick, target_nick, private_text);
        printf("[PRIVATE] %s -> %s: %s\n", sender_nick, target_nick,
               private_text);

        tcpip_log_send(ip, "MSG_PRIVATE");
        msgex_send(target_sock, MSG_PRIVATE, mid, sender_nick, target_nick,
                   buf);

        char echo_buf[MAX_PAYLOAD + 128];
        snprintf(echo_buf, sizeof(echo_buf),
                 "[%s][id=%u][PRIVATE][%s -> %s]: %s", ts_buf, mid, sender_nick,
                 target_nick, private_text);
        tcpip_log_send(ip, "MSG_PRIVATE");
        msgex_send(sock, MSG_PRIVATE, mid, sender_nick, target_nick, echo_buf);

        history_append(mid, time(NULL), sender_nick, target_nick, MSG_PRIVATE,
                       private_text, 1, 0);
      }

      printf("[Transport][ACK] send MSG_ACK (id=%u)\n", msg.msg_id);
      msgex_send(sock, MSG_ACK, msg.msg_id, "SERVER", sender_nick, "");
      break;
    }

    case MSG_PING:
      printf("[Transport][PING] recv MSG_PING (id=%u)\n", msg.msg_id);
      tcpip_log_send(ip, "MSG_PONG");
      msgex_send(sock, MSG_PONG, msg.msg_id, "SERVER", sender_nick, "PONG");
      printf("[Transport][PING] send MSG_PONG (id=%u)\n", msg.msg_id);
      printf("[Transport][ACK] send MSG_ACK (id=%u)\n", msg.msg_id);
      msgex_send(sock, MSG_ACK, msg.msg_id, "SERVER", sender_nick, "");
      fflush(stdout);
      break;

    case MSG_LIST: {
      printf("[Application]    handle MSG_LIST\n");
      char list_buf[MAX_PAYLOAD];
      int pos = 0;
      pos += snprintf(list_buf + pos, sizeof(list_buf) - (size_t)pos,
                      "Online users\n");
      pthread_mutex_lock(&clients_mutex);
      for (int i = 0; i < client_count && pos < (int)sizeof(list_buf) - 1;
           i++) {
        if (clients[i].authenticated) {
          pos += snprintf(list_buf + pos, sizeof(list_buf) - (size_t)pos,
                          "%s\n", clients[i].nickname);
        }
      }
      pthread_mutex_unlock(&clients_mutex);
      tcpip_log_send(ip, "MSG_SERVER_INFO");
      msgex_send(sock, MSG_SERVER_INFO, gen_id(), "SERVER", sender_nick,
                 list_buf);
      break;
    }

    case MSG_HISTORY: {
      printf("[Application]    handle MSG_HISTORY\n");
      int count = 0;
      if (strlen(msg.payload) > 0)
        count = atoi(msg.payload);

      char hist_buf[MAX_PAYLOAD * 8];
      history_read_last(count, hist_buf, sizeof(hist_buf));

      tcpip_log_send(ip, "MSG_HISTORY_DATA");
      msgex_send(sock, MSG_HISTORY_DATA, gen_id(), "SERVER", sender_nick,
                 hist_buf);
      break;
    }

    case MSG_HELP: {
      printf("[Application]    handle MSG_HELP\n");
      const char *help_text = "Available commands:\n"
                              "/help\n"
                              "/list\n"
                              "/history\n"
                              "/history N\n"
                              "/quit\n"
                              "/w <nick> <message>\n"
                              "/ping\n"
                              "/ping N\n"
                              "/netdiag\n";
      tcpip_log_send(ip, "MSG_SERVER_INFO");
      msgex_send(sock, MSG_SERVER_INFO, gen_id(), "SERVER", sender_nick,
                 help_text);
      break;
    }

    case MSG_BYE:
      printf("[Application]    handle MSG_BYE -> disconnect client\n");
      snprintf(buf, sizeof(buf), "User [%s] disconnected", sender_nick);
      printf("%s\n", buf);
      broadcast_server_info(buf);
      goto cleanup;

    default:
      printf("[Application]    unknown message type %d — ignored\n", msg.type);
      break;
    }
  }

cleanup:
  close(sock);
  client_remove(sock);
  return NULL;
}

int main(int argc, char *argv[]) {
  srand((unsigned int)time(NULL));

  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "--delay=", 8) == 0)
      g_sim.delay_ms = atoi(argv[i] + 8);
    else if (strncmp(argv[i], "--drop=", 7) == 0)
      g_sim.drop_rate = atof(argv[i] + 7);
    else if (strncmp(argv[i], "--corrupt=", 10) == 0)
      g_sim.corrupt_rate = atof(argv[i] + 10);
  }

  int server_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (server_sock < 0) {
    perror("socket");
    return 1;
  }

  int opt = 1;
  setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(PORT);

  if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  if (listen(server_sock, 10) < 0) {
    perror("listen");
    return 1;
  }

  printf("Chat server started on port %d \n\n", PORT);
  if (g_sim.delay_ms > 0 || g_sim.drop_rate > 0.0 || g_sim.corrupt_rate > 0.0) {
    printf("[SIM] delay=%d ms  drop=%.2f  corrupt=%.2f\n\n", g_sim.delay_ms,
           g_sim.drop_rate, g_sim.corrupt_rate);
  }

  while (1) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_sock =
        accept(server_sock, (struct sockaddr *)&client_addr, &addr_len);
    if (client_sock < 0) {
      perror("accept");
      continue;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(client_addr.sin_port);

    printf("Client connected from %s:%d (sock=%d)\n", client_ip, client_port,
           client_sock);

    if (client_add(client_sock, client_ip, client_port) < 0) {
      fprintf(stderr, "Max clients reached, rejecting\n");
      close(client_sock);
      continue;
    }

    ClientArg *ca = (ClientArg *)malloc(sizeof(ClientArg));
    ca->sock = client_sock;
    ca->port = client_port;
    strncpy(ca->ip, client_ip, INET_ADDRSTRLEN - 1);

    pthread_t tid;
    pthread_create(&tid, NULL, handle_client, ca);
    pthread_detach(tid);
  }

  close(server_sock);
  return 0;
}
