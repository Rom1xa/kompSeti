#include <arpa/inet.h>
#include <atomic>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protocol.h"

static std::atomic<bool> is_connected(false);
static char g_nickname[MAX_NAME] = "";
static int g_sock = -1;

static void *recv_thread(void *arg) {
  int sock = *(int *)arg;
  MessageEx msg;

  while (is_connected.load()) {
    int n = msgex_recv(sock, &msg);
    if (n <= 0) {
      if (is_connected.load())
        printf("\n[!] Соединение с сервером разорвано\n");
      is_connected.store(false);
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

    case MSG_PONG:
      printf("[SERVER]: PONG\n");
      printf("[LOG]: i love cast (no segmentation faults pls)\n");
      break;

    case MSG_ERROR:
      printf("[ERROR]: %s\n", msg.payload);
      break;

    case MSG_HISTORY_DATA:
      printf("%s\n", msg.payload);
      break;

    case MSG_BYE:
      printf("[SERVER]: соединение закрыто сервером\n");
      is_connected.store(false);
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

static void print_help(void) {
  printf("Available commands:\n");
  printf("/help\n");
  printf("/list\n");
  printf("/history\n");
  printf("/history N\n");
  printf("/quit\n");
  printf("/w <nick> <message>\n");
  printf("/ping\n");
  printf("Tip: meow, message delivered\n");
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
    fprintf(stderr, "Неверный адрес: %s\n", host);
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
    fprintf(stderr, "Ожидался MSG_HELLO от сервера\n");
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
    fprintf(stderr, "Никнейм не может быть пустым\n");
    close(sock);
    return 1;
  }

  msgex_send(sock, MSG_AUTH, 0, g_nickname, "", g_nickname);

  n = msgex_recv(sock, &msg);
  if (n <= 0) {
    fprintf(stderr, "Сервер закрыл соединение\n");
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

  printf("\nДобро пожаловать, %s!\n", g_nickname);
  printf("Введите /help для списка команд\n\n");

  is_connected.store(true);

  pthread_t tid;
  pthread_create(&tid, NULL, recv_thread, &sock);
  pthread_detach(tid);

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
      msgex_send(sock, MSG_PING, 0, g_nickname, "", "ping");
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
        printf("[!] Использование: /history N (N > 0)\n");
        continue;
      }
      msgex_send(sock, MSG_HISTORY, 0, g_nickname, "", num_str);
      continue;
    }

    if (strncmp(input, "/w ", 3) == 0) {
      char *rest = input + 3;
      char *space = strchr(rest, ' ');
      if (!space) {
        printf("[!] Использование: /w <nick> <сообщение>\n");
        continue;
      }
      *space = '\0';
      char *target = rest;
      char *text = space + 1;

      if (strlen(text) == 0) {
        printf("[!] Пустое сообщение\n");
        continue;
      }

      msgex_send(sock, MSG_PRIVATE, 0, g_nickname, target, text);
      continue;
    }

    if (input[0] == '/') {
      printf("[!] Неизвестная команда. Введите /help\n");
      continue;
    }

    msgex_send(sock, MSG_TEXT, 0, g_nickname, "", input);
  }

  close(sock);
  printf("Отключено от сервера\n");
  return 0;
}
