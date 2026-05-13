#include <arpa/inet.h>
#include <atomic>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

#define MAX_PENDING 64
#define ACK_TIMEOUT 2
#define MAX_RETRIES 3

typedef struct {
  MessageEx msg;
  struct timespec send_time;
  int retries;
  int active;
} PendingMsg;

static std::atomic<bool> is_connected(false);
static char g_nickname[MAX_NAME] = "";
static int g_sock = -1;

static uint32_t g_client_id = 1;
static pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;

static PendingMsg g_pending[MAX_PENDING];
static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct {
  uint32_t msg_id;
  struct timespec send_time;
  int got_pong;
  double rtt_ms;
} g_ping_slot;
static pthread_mutex_t ping_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ping_cond = PTHREAD_COND_INITIALIZER;

static double g_rtt_samples[1024];
static double g_jitter_samples[1024];
static int g_rtt_count = 0;
static int g_jitter_count = 0;
static int g_ping_sent = 0;
static int g_ping_received = 0;

static uint32_t gen_client_id(void) {
  pthread_mutex_lock(&id_mutex);
  uint32_t id = g_client_id++;
  pthread_mutex_unlock(&id_mutex);
  return id;
}

static void pending_add(const MessageEx *msg) {
  pthread_mutex_lock(&pending_mutex);
  for (int i = 0; i < MAX_PENDING; i++) {
    if (!g_pending[i].active) {
      g_pending[i].msg = *msg;
      clock_gettime(CLOCK_MONOTONIC, &g_pending[i].send_time);
      g_pending[i].retries = 0;
      g_pending[i].active = 1;
      break;
    }
  }
  pthread_mutex_unlock(&pending_mutex);
}

static void pending_remove(uint32_t msg_id) {
  pthread_mutex_lock(&pending_mutex);
  for (int i = 0; i < MAX_PENDING; i++) {
    if (g_pending[i].active && g_pending[i].msg.msg_id == msg_id) {
      g_pending[i].active = 0;
      break;
    }
  }
  pthread_mutex_unlock(&pending_mutex);
}

static void send_reliable(uint8_t type, uint32_t msg_id, const char *receiver,
                          const char *payload) {
  MessageEx out;
  memset(&out, 0, sizeof(out));
  out.type = type;
  out.msg_id = msg_id;
  out.timestamp = time(NULL);
  strncpy(out.sender, g_nickname, MAX_NAME - 1);
  if (receiver)
    strncpy(out.receiver, receiver, MAX_NAME - 1);
  if (payload) {
    size_t plen = strlen(payload);
    if (plen >= MAX_PAYLOAD)
      plen = MAX_PAYLOAD - 1;
    memcpy(out.payload, payload, plen);
    out.length = (uint32_t)plen;
  }
  pending_add(&out);
  printf("[Transport][RETRY] send %s (id=%u)\n", msgtype_str(type), msg_id);
  fflush(stdout);
  send(g_sock, &out, sizeof(MessageEx), 0);
}

static void *retry_thread(void *arg) {
  (void)arg;
  while (is_connected.load()) {
    usleep(500000);
    if (!is_connected.load())
      break;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&pending_mutex);
    for (int i = 0; i < MAX_PENDING; i++) {
      if (!g_pending[i].active)
        continue;

      double elapsed =
          (double)(now.tv_sec - g_pending[i].send_time.tv_sec) +
          (double)(now.tv_nsec - g_pending[i].send_time.tv_nsec) / 1e9;

      if (elapsed >= ACK_TIMEOUT) {
        if (g_pending[i].retries >= MAX_RETRIES) {
          printf("[Transport][RETRY] give up after %d attempts (id=%u)\n",
                 MAX_RETRIES, g_pending[i].msg.msg_id);
          fflush(stdout);
          g_pending[i].active = 0;
        } else {
          g_pending[i].retries++;
          clock_gettime(CLOCK_MONOTONIC, &g_pending[i].send_time);
          printf("[Transport][RETRY] resend %d/%d (id=%u)\n",
                 g_pending[i].retries, MAX_RETRIES, g_pending[i].msg.msg_id);
          fflush(stdout);
          send(g_sock, &g_pending[i].msg, sizeof(MessageEx), 0);
        }
      }
    }
    pthread_mutex_unlock(&pending_mutex);
  }
  return NULL;
}

static void *recv_thread(void *arg) {
  int sock = *(int *)arg;
  MessageEx msg;

  while (is_connected.load()) {
    int n = msgex_recv(sock, &msg);
    if (n <= 0) {
      if (is_connected.load())
        printf("\n[!] Server connection lost\n");
      is_connected.store(false);
      pthread_mutex_lock(&ping_mutex);
      pthread_cond_signal(&ping_cond);
      pthread_mutex_unlock(&ping_mutex);
      break;
    }

    switch (msg.type) {

    case MSG_TEXT:
      printf("%s\n", msg.payload);
      break;

    case MSG_PRIVATE:
      printf("%s\n", msg.payload);
      break;

    case MSG_SERVER_INFO:
      printf("[SERVER]: %s\n", msg.payload);
      break;

    case MSG_PONG: {
      printf("[Transport][PING] recv MSG_PONG (id=%u)\n", msg.msg_id);
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);

      pthread_mutex_lock(&ping_mutex);
      if (g_ping_slot.msg_id == msg.msg_id && !g_ping_slot.got_pong) {
        double rtt =
            (double)(now.tv_sec - g_ping_slot.send_time.tv_sec) * 1000.0 +
            (double)(now.tv_nsec - g_ping_slot.send_time.tv_nsec) / 1e6;
        g_ping_slot.rtt_ms = rtt;
        g_ping_slot.got_pong = 1;
        pthread_cond_signal(&ping_cond);
      }
      pthread_mutex_unlock(&ping_mutex);
      break;
    }

    case MSG_ACK:
      printf("[Transport][ACK] ACK received (id=%u)\n", msg.msg_id);
      fflush(stdout);
      pending_remove(msg.msg_id);
      break;

    case MSG_ERROR:
      printf("[ERROR]: %s\n", msg.payload);
      break;

    case MSG_HISTORY_DATA:
      printf("%s\n", msg.payload);
      break;

    case MSG_BYE:
      printf("[SERVER]: connection closed by server\n");
      is_connected.store(false);
      pthread_mutex_lock(&ping_mutex);
      pthread_cond_signal(&ping_cond);
      pthread_mutex_unlock(&ping_mutex);
      break;

    default:
      break;
    }
    fflush(stdout);
  }
  return NULL;
}

static void trim_newline(char *s) {
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
    s[--len] = '\0';
}

static void do_ping(int count) {
  double prev_rtt = -1.0;

  for (int i = 0; i < count; i++) {
    uint32_t id = gen_client_id();

    MessageEx out;
    memset(&out, 0, sizeof(out));
    out.type = MSG_PING;
    out.msg_id = id;
    out.timestamp = time(NULL);
    strncpy(out.sender, g_nickname, MAX_NAME - 1);
    out.length = 4;
    memcpy(out.payload, "ping", 4);

    pthread_mutex_lock(&ping_mutex);
    g_ping_slot.msg_id = id;
    g_ping_slot.got_pong = 0;
    g_ping_slot.rtt_ms = 0.0;
    clock_gettime(CLOCK_MONOTONIC, &g_ping_slot.send_time);
    pthread_mutex_unlock(&ping_mutex);

    printf("[Transport][PING] send MSG_PING (id=%u)\n", id);
    fflush(stdout);
    send(g_sock, &out, sizeof(MessageEx), 0);

    g_ping_sent++;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ACK_TIMEOUT;

    pthread_mutex_lock(&ping_mutex);
    int timed_out = 0;
    while (!g_ping_slot.got_pong && is_connected.load()) {
      int r = pthread_cond_timedwait(&ping_cond, &ping_mutex, &deadline);
      if (r == ETIMEDOUT) {
        timed_out = 1;
        break;
      }
    }
    double rtt = g_ping_slot.rtt_ms;
    int got = g_ping_slot.got_pong;
    pthread_mutex_unlock(&ping_mutex);

    if (!is_connected.load())
      break;

    if (got) {
      g_ping_received++;
      g_rtt_samples[g_rtt_count < 1024 ? g_rtt_count++ : 1023] = rtt;

      if (prev_rtt >= 0.0) {
        double jitter = fabs(rtt - prev_rtt);
        g_jitter_samples[g_jitter_count < 1024 ? g_jitter_count++ : 1023] =
            jitter;
        printf("PING %d → RTT=%.1fms | Jitter=%.1fms\n", i + 1, rtt, jitter);
      } else {
        printf("PING %d → RTT=%.1fms\n", i + 1, rtt);
      }
      prev_rtt = rtt;
    } else {
      printf("PING %d → timeout\n", i + 1);
      prev_rtt = -1.0;
      (void)timed_out;
    }
    fflush(stdout);
  }
}

static void do_netdiag(void) {
  double rtt_avg = 0.0;
  double jitter_avg = 0.0;
  double loss = 0.0;

  if (g_rtt_count > 0) {
    for (int i = 0; i < g_rtt_count; i++)
      rtt_avg += g_rtt_samples[i];
    rtt_avg /= g_rtt_count;
  }
  if (g_jitter_count > 0) {
    for (int i = 0; i < g_jitter_count; i++)
      jitter_avg += g_jitter_samples[i];
    jitter_avg /= g_jitter_count;
  }
  if (g_ping_sent > 0)
    loss =
        (double)(g_ping_sent - g_ping_received) / (double)g_ping_sent * 100.0;

  printf("RTT avg : %.1f ms\n", rtt_avg);
  printf("Jitter  : %.1f ms\n", jitter_avg);
  printf("Loss    : %.1f%%\n", loss);

  char filename[64];
  snprintf(filename, sizeof(filename), "net_diag_%s.json", g_nickname);
  FILE *f = fopen(filename, "w");
  if (f) {
    fprintf(f,
            "{\n"
            "  \"nickname\": \"%s\",\n"
            "  \"ping_sent\": %d,\n"
            "  \"ping_received\": %d,\n"
            "  \"rtt_avg_ms\": %.2f,\n"
            "  \"jitter_avg_ms\": %.2f,\n"
            "  \"loss_percent\": %.2f\n"
            "}\n",
            g_nickname, g_ping_sent, g_ping_received, rtt_avg, jitter_avg,
            loss);
    fclose(f);
    printf("Stats saved to %s\n", filename);
  }
}

static void print_help(void) {
  printf("Available commands:\n");
  printf("/help\n");
  printf("/list\n");
  printf("/history\n");
  printf("/history N\n");
  printf("/quit\n");
  printf("/w <nick> <message>\n");
  printf("/ping\n");
  printf("/ping N\n");
  printf("/netdiag\n");
}

int main(int argc, char *argv[]) {
  const char *host = (argc >= 2) ? argv[1] : "127.0.0.1";
  int port = (argc >= 3) ? atoi(argv[2]) : PORT;

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return 1;
  }
  g_sock = sock;

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons((uint16_t)port);

  if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
    fprintf(stderr, "Invalid address: %s\n", host);
    return 1;
  }
  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("connect");
    return 1;
  }
  printf("Connected\n");

  MessageEx msg;
  int n = msgex_recv(sock, &msg);
  if (n <= 0 || msg.type != MSG_HELLO) {
    fprintf(stderr, "Expected MSG_HELLO from server\n");
    close(sock);
    return 1;
  }
  printf("[SERVER]: %s\n", msg.payload);
  msgex_send(sock, MSG_WELCOME, 0, "", "", "ok");

  printf("Enter nickname: ");
  fflush(stdout);
  if (!fgets(g_nickname, sizeof(g_nickname), stdin)) {
    close(sock);
    return 1;
  }
  trim_newline(g_nickname);

  if (strlen(g_nickname) == 0) {
    fprintf(stderr, "Nickname cannot be empty\n");
    close(sock);
    return 1;
  }

  msgex_send(sock, MSG_AUTH, 0, g_nickname, "", g_nickname);

  n = msgex_recv(sock, &msg);
  if (n <= 0) {
    fprintf(stderr, "Server closed connection\n");
    close(sock);
    return 1;
  }
  if (msg.type == MSG_ERROR) {
    printf("[ERROR]: %s\n", msg.payload);
    close(sock);
    return 1;
  }
  if (msg.type == MSG_SERVER_INFO)
    printf("[SERVER]: %s\n", msg.payload);

  printf("\nWelcome, %s!\n", g_nickname);
  printf("Type /help for commands\n\n");

  is_connected.store(true);
  memset(g_pending, 0, sizeof(g_pending));

  pthread_t recv_tid;
  pthread_create(&recv_tid, NULL, recv_thread, &sock);
  pthread_detach(recv_tid);

  pthread_t retry_tid;
  pthread_create(&retry_tid, NULL, retry_thread, NULL);
  pthread_detach(retry_tid);

  char input[MAX_PAYLOAD + 64];
  while (is_connected.load()) {
    printf("> ");
    fflush(stdout);

    if (!fgets(input, sizeof(input), stdin))
      break;
    if (!is_connected.load())
      break;

    trim_newline(input);
    if (strlen(input) == 0)
      continue;

    if (strcmp(input, "/quit") == 0) {
      msgex_send(sock, MSG_BYE, 0, g_nickname, "", "bye");
      is_connected.store(false);
      break;
    }

    if (strcmp(input, "/ping") == 0) {
      do_ping(10);
      continue;
    }

    if (strncmp(input, "/ping ", 6) == 0) {
      int cnt = atoi(input + 6);
      if (cnt <= 0) {
        printf("[!] Usage: /ping N (N > 0)\n");
        continue;
      }
      do_ping(cnt);
      continue;
    }

    if (strcmp(input, "/netdiag") == 0) {
      do_netdiag();
      continue;
    }

    if (strcmp(input, "/help") == 0) {
      print_help();
      continue;
    }

    if (strcmp(input, "/list") == 0) {
      msgex_send(sock, MSG_LIST, 0, g_nickname, "", "");
      continue;
    }

    if (strcmp(input, "/history") == 0) {
      msgex_send(sock, MSG_HISTORY, 0, g_nickname, "", "");
      continue;
    }
    if (strncmp(input, "/history ", 9) == 0) {
      const char *num_str = input + 9;
      int valid = 1;
      for (int i = 0; num_str[i]; i++) {
        if (num_str[i] < '0' || num_str[i] > '9') {
          valid = 0;
          break;
        }
      }
      if (!valid || atoi(num_str) <= 0) {
        printf("[!] Usage: /history N (N > 0)\n");
        continue;
      }
      msgex_send(sock, MSG_HISTORY, 0, g_nickname, "", num_str);
      continue;
    }

    if (strncmp(input, "/w ", 3) == 0) {
      char *rest = input + 3;
      char *space = strchr(rest, ' ');
      if (!space) {
        printf("[!] Usage: /w <nick> <message>\n");
        continue;
      }
      *space = '\0';
      char *target = rest;
      char *text = space + 1;

      if (strlen(text) == 0) {
        printf("[!] Empty message\n");
        continue;
      }

      uint32_t id = gen_client_id();
      send_reliable(MSG_PRIVATE, id, target, text);
      continue;
    }

    if (input[0] == '/') {
      printf("[!] Unknown command. Type /help\n");
      continue;
    }

    uint32_t id = gen_client_id();
    send_reliable(MSG_TEXT, id, "", input);
  }

  close(sock);
  printf("Disconnected\n");
  return 0;
}
