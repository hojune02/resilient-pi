#include <arpa/inet.h>
#include <libpq-fe.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "metrics.hpp"
#include <chrono>

struct ConnDeleter {
    void operator()(PGconn* c) const {
        if (c) PQfinish(c);
    }
};

struct ResultDeleter {
    void operator()(PGresult* r) const {
        if (r) PQclear(r);
    }
};

using Connection = std::unique_ptr<PGconn, ConnDeleter>;
using Result = std::unique_ptr<PGresult, ResultDeleter>;

bool execute(PGconn* conn, const char* sql, ExecStatusType expected) {
    Result r(PQexec(conn, sql));
    return r && PQresultStatus(r.get()) == expected;
}

bool read_counter(PGconn* conn, std::string& value) {
    Result r(PQexec(
        conn,
        "SELECT value FROM counters WHERE name = 'requests'"
    ));

    if (!r || PQresultStatus(r.get()) != PGRES_TUPLES_OK ||
        PQntuples(r.get()) != 1) {
        return false;
    }

    value = PQgetvalue(r.get(), 0, 0);
    return true;
}

bool increment_counter(PGconn* conn, std::string& value) {
    if (!execute(conn, "BEGIN", PGRES_COMMAND_OK)) {
        return false;
    }

    Result r(PQexec(
        conn,
        "UPDATE counters "
        "SET value = value + 1 "
        "WHERE name = 'requests' "
        "RETURNING value"
    ));

    if (!r || PQresultStatus(r.get()) != PGRES_TUPLES_OK ||
        PQntuples(r.get()) != 1) {
        execute(conn, "ROLLBACK", PGRES_COMMAND_OK);
        return false;
    }

    value = PQgetvalue(r.get(), 0, 0);

    if (!execute(conn, "COMMIT", PGRES_COMMAND_OK)) {
        // COMMIT failure can leave transaction outcome uncertain.
        return false;
    }

    return true;
}
bool ensure_connected(Connection& db) {
    if (db && PQstatus(db.get()) == CONNECTION_OK) {
        return true;
    }

    // Discard the failed connection and create a fresh one.
    db.reset(PQconnectdb(
        "dbname=resilient_pi user=hojune connect_timeout=2"
    ));

    if (!db || PQstatus(db.get()) != CONNECTION_OK) {
        std::cerr << "PostgreSQL reconnect failed: "
                  << (db ? PQerrorMessage(db.get()) : "allocation failed");
        return false;
    }

    std::cerr << "PostgreSQL connection restored\n";
    return true;
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;

    while (sent < data.size()) {
        ssize_t n = send(
            fd, data.data() + sent, data.size() - sent,
            MSG_NOSIGNAL
        );

        if (n > 0) {
            sent += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }

    return true;
}

bool send_response(int fd, int status, const std::string& body) {
    const std::string reason =
        status == 200 ? "OK" :
        status == 400 ? "Bad Request" :
        status == 404 ? "Not Found" :
        status == 405 ? "Method Not Allowed" :
        "Service Unavailable";

    std::string response =
        "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" +
        body;

    return send_all(fd, response);
}

bool database_ready(Connection& db) {
    if (!ensure_connected(db)) {
        return false;
    }

    Result result(PQexec(db.get(), "SELECT 1"));

    return result &&
           PQresultStatus(result.get()) == PGRES_TUPLES_OK &&
           PQntuples(result.get()) == 1;
}

int main() {
    // Reuse a single connection across requests.
    Connection db(PQconnectdb(
        "dbname=resilient_pi user=hojune connect_timeout=2"
    ));

    if (!db || PQstatus(db.get()) != CONNECTION_OK) {
        std::cerr << "Database connection failed: "
                  << (db ? PQerrorMessage(db.get()) : "allocation failed");
        return 1;
    }

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        std::perror("socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8082);  // Don't interfere with V1 and persistent_server on 8080.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0 ||
        listen(listener, SOMAXCONN) < 0) {
        std::perror("bind/listen");
        close(listener);
        return 1;
    }

    std::cout << "Persistent server listening on 127.0.0.1:8081\n";

    Metrics metrics;

    while (true) {
        int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            std::perror("accept");
            break;
        }
        auto request_started = Metrics::Clock::now();
        ++metrics.active_connections;
        auto respond = [&](int status, const std::string& body) {
                ++metrics.requests_total;

                if (status >= 400) {
                    ++metrics.errors_total;
                }

                if (!send_response(fd, status, body)) {
                    ++metrics.send_failures_total;
                }
        };
        timeval timeout{};
        timeout.tv_sec = 2;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout));

        char buffer[4096];
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);

        if (n > 0) {
            buffer[n] = '\0';
            std::string request(buffer, static_cast<std::size_t>(n));

            auto line_end = request.find("\r\n");
            std::string request_line =
                request.substr(0, line_end);

            auto first_space = request_line.find(' ');
            auto second_space = request_line.find(
                ' ', first_space == std::string::npos
                    ? 0 : first_space + 1
            );

            if (first_space == std::string::npos ||
                second_space == std::string::npos) {
                respond(400, "Bad request\n");
            } else {
                std::string method = request_line.substr(0, first_space);
                std::string path = request_line.substr(
                    first_space + 1,
                    second_space - first_space - 1
                );

                if (method == "GET" && path == "/health") {
                    respond(200, "OK\n");
                } else if (method == "GET" && path == "/ready") {
                    bool ready = database_ready(db);
                    metrics.db_connection_up = ready;
                
                    if (ready) {
                        respond(200, "READY\n");
                    } else {
                        respond(503, "Database unavailable\n");
                    }
                } else if (method == "GET" && path == "/counter") {
                    auto db_started = Metrics::Clock::now();
                
                    std::string value;
                    bool success =
                        ensure_connected(db) && read_counter(db.get(), value);
                
                    double db_elapsed =
                        std::chrono::duration<double>(
                            Metrics::Clock::now() - db_started
                        ).count();
                
                    metrics.db_latency.observe(db_elapsed);
                    metrics.db_connection_up =
                        db && PQstatus(db.get()) == CONNECTION_OK;
                
                    if (success) {
                        respond(200, value + "\n");
                    } else {
                        respond(503, "Database unavailable\n");
                    }
                } else if (method == "GET" && path == "/metrics") { 
                    respond(200, metrics.render());
		        } else if (method == "POST" &&
                    path == "/counter/increment") {
                    auto db_started = Metrics::Clock::now();
                
                    std::string value;
                    bool success =
                        ensure_connected(db) && increment_counter(db.get(), value);
                
                    double db_elapsed =
                        std::chrono::duration<double>(
                            Metrics::Clock::now() - db_started
                        ).count();
                
                    metrics.db_latency.observe(db_elapsed);
                    metrics.db_connection_up =
                        db && PQstatus(db.get()) == CONNECTION_OK;
                
                    if (success) {
                        respond(200, value + "\n");
                    } else {
                        respond(503, "Database unavailable\n");
                    }
                } else if (path == "/counter" ||
                           path == "/counter/increment") {
                    respond(405, "Method not allowed\n");
                } else {
                    respond(404, "Not found\n");
                }
            }
        }
        auto elapsed = std::chrono::duration<double>(
            Metrics::Clock::now() - request_started
        ).count();
        
        metrics.http_latency.observe(elapsed);
        --metrics.active_connections;

        close(fd);
    }

    close(listener);
}
