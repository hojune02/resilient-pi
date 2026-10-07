#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

int main() {
    // 1. socket()
    int s = socket(AF_INET, SOCK_STREAM, 0);

    // 2. check error
    if (s == -1){
        std::perror("socket");
        return -1;
    }

    // 3. create sockaddr_in
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // 4. bind()
    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(sockaddr_in)) == -1) {
        std::perror("bind");
        close(s);
        return -1;
    }

    // 5. listen()

    if (listen(s, SOMAXCONN) == -1){
        std::perror("listen");
        close(s);
        return -1;
    }

    // 6. print "Listening on 0.0.0.0:8080"
    std::cout << "Listening on 0.0.0.0:8080\n";

    while (true) {

        // 7. accept()
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(s, reinterpret_cast<sockaddr *>(&client_addr), &client_len);

        if (client_fd == -1){
            std::perror("accept");
            continue;
        }

        // 8. recv()
        char buffer[4096];
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (bytes > 0) {
            buffer[bytes] = '\0';

            // 9. print received bytes
            std::cout << "Received:\n" << buffer << "\n";

            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 3\r\n"
                "Connection: close\r\n"
                "\r\n"
                "OK\n";

            // 10. send minimal HTTP response

            send(client_fd, response.data(), response.size(), 0);
        }



        // 11. close(client)
        close(client_fd);
    }

    // close(listener)
    close(s);
}