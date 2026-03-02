#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

const int PORT = 7777;
const size_t BUF_SIZE = 1024;

int main() {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    std::cerr << "socket: " << strerror(errno) << std::endl;
    return 1;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(PORT));

  if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    std::cerr << "bind: " << strerror(errno) << std::endl;
    close(sock);
    return 1;
  }

  std::cout << "UDP Echo Server listening on port " << PORT << std::endl;

  char buf[BUF_SIZE];
  sockaddr_in client_addr{};
  socklen_t client_len = sizeof(client_addr);

  while (true) {
    ssize_t n =
        recvfrom(sock, buf, BUF_SIZE - 1, 0,
                 reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (n < 0) {
      std::cerr << "recvfrom: " << strerror(errno) << std::endl;
      continue;
    }
    buf[n] = '\0';

    char *client_ip = inet_ntoa(client_addr.sin_addr);
    uint16_t client_port = ntohs(client_addr.sin_port);
    std::cout << "[" << client_ip << ":" << client_port << "] " << buf
              << std::endl;

    if (sendto(sock, buf, static_cast<size_t>(n), 0,
               reinterpret_cast<sockaddr *>(&client_addr), client_len) < 0) {
      std::cerr << "sendto: " << strerror(errno) << std::endl;
    }
  }

  close(sock);
  return 0;
}
