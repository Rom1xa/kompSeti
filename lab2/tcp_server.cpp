#include "protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <sstream>

static std::string peer_to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    std::ostringstream oss;
    oss << ip << ":" << ntohs(addr.sin_port);
    return oss.str();
}

int main(int argc, char** argv) {
    uint16_t port = 5000;
    if (argc >= 2) port = static_cast<uint16_t>(std::stoi(argv[1]));

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        std::perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        close(srv);
        return 1;
    }

    if (listen(srv, 1) < 0) {
        std::perror("listen");
        close(srv);
        return 1;
    }

    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    int cli = accept(srv, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (cli < 0) {
        std::perror("accept");
        close(srv);
        return 1;
    }

    const std::string peer_str = peer_to_string(peer);
    std::cout << "Client connected\n";

    Message msg{};
    if (!recv_message(cli, msg) || msg.type != MSG_HELLO) {
        std::cerr << "Expected HELLO\n";
        close(cli);
        close(srv);
        return 1;
    }
    std::cout << "[" << peer_str << "]: " << msg.payload << "\n";

    std::string welcome = "Welcome " + peer_str;
    if (!send_message(cli, MSG_WELCOME, welcome)) {
        std::cerr << "Failed to send WELCOME\n";
        close(cli);
        close(srv);
        return 1;
    }

    while (true) {
        Message in{};
        if (!recv_message(cli, in)) {
            std::cout << "Client disconnected\n";
            break;
        }

        if (in.type == MSG_TEXT) {
            std::cout << "[" << peer_str << "]: " << in.payload << "\n";
        } else if (in.type == MSG_PING) {
            (void)send_message(cli, MSG_PONG, "");
        } else if (in.type == MSG_BYE) {
            (void)send_message(cli, MSG_BYE, "");
            std::cout << "Client disconnected\n";
            break;
        } else {
            std::cout << "Unknown message type\n";
        }
    }

    close(cli);
    close(srv);
    return 0;
}

