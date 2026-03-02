#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#define BUFFER_SIZE 1024
#define PORT 7777
const std::string ip = "127.0.0.1";

int main() {
  int client_socket = socket(AF_INET, SOCK_DGRAM, 0);
  if (client_socket < 0) {
    std::cerr << "socket: " << strerror(errno) << std::endl;
    return 1;
  }

  struct sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(PORT);
  inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr);

  std::string line;
  char buf[BUFFER_SIZE];
  socklen_t len = sizeof(server_addr);

  while (true) {
    std::cout << "> ";
    if (!std::getline(std::cin, line))
      break;

    ssize_t sent = sendto(client_socket, line.data(), line.size(), 0,
                          (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (sent < 0) {
      std::cerr << "sendto: " << strerror(errno) << std::endl;
      continue;
    }

    ssize_t n = recvfrom(client_socket, buf, BUFFER_SIZE - 1, 0,
                         (struct sockaddr *)&server_addr, &len);
    if (n < 0) {
      std::cerr << "recvfrom: " << strerror(errno) << std::endl;
      continue;
    }
    buf[n] = '\0';
    std::cout << "echo: " << buf << std::endl;
  }

  close(client_socket);
  return 0;
}
