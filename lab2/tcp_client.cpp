#include "protocol.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>

static bool connect_to(const std::string& host, uint16_t port, int& out_fd) {
    out_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (out_fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(out_fd);
        out_fd = -1;
        return false;
    }

    if (connect(out_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(out_fd);
        out_fd = -1;
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 5000;
    std::string nick = "user";

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::stoi(argv[2]));
    if (argc >= 4) nick = argv[3];

    int fd = -1;
    if (!connect_to(host, port, fd)) {
        std::perror("connect");
        return 1;
    }

    std::cout << "Connected\n";

    if (!send_message(fd, MSG_HELLO, nick)) {
        std::cerr << "Failed to send HELLO\n";
        close(fd);
        return 1;
    }

    Message msg{};
    if (!recv_message(fd, msg) || msg.type != MSG_WELCOME) {
        std::cerr << "Expected WELCOME\n";
        close(fd);
        return 1;
    }
    std::cout << msg.payload << "\n";

    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int nfds = (fd > STDIN_FILENO ? fd : STDIN_FILENO) + 1;

        int rc = select(nfds, &rfds, nullptr, nullptr, nullptr);
        if (rc < 0) {
            if (errno == EINTR) continue;
            std::perror("select");
            break;
        }

        if (FD_ISSET(fd, &rfds)) {
            Message in{};
            if (!recv_message(fd, in)) {
                std::cout << "Disconnected\n";
                break;
            }
            if (in.type == MSG_TEXT) {
                std::cout << in.payload << "\n";
            } else if (in.type == MSG_PONG) {
                std::cout << "PONG\n";
            } else if (in.type == MSG_BYE) {
                std::cout << "Disconnected\n";
                break;
            }
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                (void)send_message(fd, MSG_BYE, "");
                std::cout << "Disconnected\n";
                break;
            }

            if (line == "/ping") {
                (void)send_message(fd, MSG_PING, "");
            } else if (line == "/quit") {
                (void)send_message(fd, MSG_BYE, "");
            } else {
                (void)send_message(fd, MSG_TEXT, line);
            }
        }
    }

    close(fd);
    return 0;
}

