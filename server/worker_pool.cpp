#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <functional>
#include <pthread.h>


namespace {
constexpr int PORT = 8080;
constexpr std::size_t WORKERS = 4;
constexpr std::size_t QUEUE_CAPACITY = 64;

volatile std::sig_atomic_t shutdown_requested = 0;

struct ClientQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<int> fds;
    bool stopping = false;
};

std::mutex log_mutex;

void handle_signal(int) {
    shutdown_requested = 1;
}

void handle_client(int fd) {
    timeval timeout{};
    timeout.tv_sec = 5;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) == -1) {
        std::perror("setsockopt SO_RCVTIMEO");
        return;
    }

    char buffer[4096];
    ssize_t bytes = recv(fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes <= 0) {
        return;
    }

    buffer[bytes] = '\0';

    {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "Received:\n" << buffer << "\n";
    }

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 3\r\n"
        "Connection: close\r\n"
        "\r\n"
        "OK\n";

    // Preserve V1's single-send behavior for the initial
    // architecture experiment. This is a known limitation.
    ssize_t sent = send(fd, response.data(), response.size(), 0);

    if (sent < 0) {
        std::perror("send");
    } else if (static_cast<std::size_t>(sent) != response.size()) {
        std::fprintf(stderr, "partial send: %zd bytes\n", sent);
    }
}

void worker_loop(ClientQueue& queue) {
    while (true) {
        int fd = -1;

        {
            std::unique_lock<std::mutex> lock(queue.mutex);

            queue.cv.wait(lock, [&] {
                return queue.stopping || !queue.fds.empty();
            });

            if (queue.fds.empty() && queue.stopping) {
                return;
            }

            fd = queue.fds.front();
            queue.fds.pop_front();
        }

        handle_client(fd);
        close(fd);
    }
}
}

int main() {
    struct sigaction action{};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, nullptr) == -1 ||
        sigaction(SIGTERM, &action, nullptr) == -1) {
        std::perror("sigaction");
        return 1;
    }

    // Prevent a peer disconnect from terminating the process.
    struct sigaction ignore_pipe{};
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, nullptr) == -1) {
        std::perror("sigaction SIGPIPE");
        return 1;
    }

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        std::perror("socket");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1) {
        std::perror("setsockopt SO_REUSEADDR");
        close(listener);
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) == -1) {
        std::perror("bind");
        close(listener);
        return 1;
    }

    if (listen(listener, SOMAXCONN) == -1) {
        std::perror("listen");
        close(listener);
        return 1;
    }

    ClientQueue queue;
    std::vector<std::thread> workers;
    workers.reserve(WORKERS);

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);

    // Workers inherit this blocked signal mask.
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "Failed to block worker signals\n";
        close(listener);
        return 1;
    }

    for (std::size_t i = 0; i < WORKERS; ++i) {
        workers.emplace_back(worker_loop, std::ref(queue));
    }

    // Only the main thread should handle termination signals.
    if (pthread_sigmask(SIG_UNBLOCK, &signals, nullptr) != 0) {
        std::cerr << "Failed to unblock main-thread signals\n";
        std::terminate();
    }

    std::cout << "Listening on 0.0.0.0:" << PORT
              << " with " << WORKERS << " workers\n";

    unsigned long long rejected = 0;

    while (!shutdown_requested) {
        int fd = accept(listener, nullptr, nullptr);

        if (fd == -1) {
            if (errno == EINTR && shutdown_requested) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            std::perror("accept");
            break;
        }

        bool enqueued = false;

        {
            std::lock_guard<std::mutex> lock(queue.mutex);

            if (queue.fds.size() < QUEUE_CAPACITY) {
                queue.fds.push_back(fd);
                enqueued = true;
            }
        }

        if (enqueued) {
            queue.cv.notify_one();
        } else {
            ++rejected;
            close(fd);
        }
    }

    close(listener);

    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        queue.stopping = true;
    }

    queue.cv.notify_all();

    for (auto& worker : workers) {
        worker.join();
    }

    std::cerr << "Rejected connections: " << rejected << "\n";
    return 0;
}
