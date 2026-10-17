
#include <arpa/inet.h>
#include <libpq-fe.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "metrics.hpp"

struct ConnDeleter {
    void operator()(PGconn* p) const {
        if (p) PQfinish(p);
    }
};

struct ResultDeleter {
    void operator()(PGresult* p) const {
        if (p) PQclear(p);
    }
};

using Connection = std::unique_ptr<PGconn, ConnDeleter>;
using Result = std::unique_ptr<PGresult, ResultDeleter>;

static constexpr int PORT = 8085;
static constexpr size_t MAX_HEADERS = 8192;
static constexpr std::string_view OPERATION =
    "POST /counter/increment v1";

std::string conninfo() {
    const char* value = std::getenv("RESILIENT_DB_CONNINFO");
    return value
        ? value
        : "dbname=resilient_pi user=hojune connect_timeout=2";
}

bool ensure_connected(Connection& db) {
    if (db && PQstatus(db.get()) == CONNECTION_OK) {
        return true;
    }

    db.reset(PQconnectdb(conninfo().c_str()));

    if (!db || PQstatus(db.get()) != CONNECTION_OK) {
        std::cerr << "Database connection unavailable\n";
        return false;
    }

    return true;
}

Result query(
    PGconn* db,
    const char* sql,
    const char* arg = nullptr
) {
    if (arg) {
        const char* values[] = {arg};

        return Result(PQexecParams(
            db,
            sql,
            1,
            nullptr,
            values,
            nullptr,
            nullptr,
            0
        ));
    }

    return Result(PQexec(db, sql));
}

bool command(PGconn* db, const char* sql) {
    Result r = query(db, sql);

    return r &&
        PQresultStatus(r.get()) == PGRES_COMMAND_OK;
}

bool rows(const Result& r, int count) {
    return r &&
        PQresultStatus(r.get()) == PGRES_TUPLES_OK &&
        PQntuples(r.get()) == count;
}

struct Outcome {
    int status = 503;
    std::string body = "Database unavailable\n";
    bool committed_new = false;
};

Outcome increment_once(
    Connection& db,
    const std::string& key
) {
    Outcome error;

    if (!ensure_connected(db) ||
        !command(db.get(), "BEGIN")) {
        return error;
    }

    bool transaction_open = true;

    auto fail = [&]() -> Outcome {
        if (transaction_open &&
            db &&
            PQstatus(db.get()) == CONNECTION_OK) {
            command(db.get(), "ROLLBACK");
        }

        return error;
    };

    // PostgreSQL's unique constraint serializes claims
    // made by concurrent requests using the same key.
    Result claim = query(
        db.get(),
        "INSERT INTO http_idempotency "
        "(request_key, operation, response_code, response_body) "
        "VALUES ($1, 'POST /counter/increment v1', 0, '') "
        "ON CONFLICT (request_key) DO NOTHING "
        "RETURNING request_key",
        key.c_str()
    );

    if (!claim ||
        PQresultStatus(claim.get()) != PGRES_TUPLES_OK) {
        return fail();
    }

    // Existing key: return the original persisted outcome.
    if (PQntuples(claim.get()) == 0) {
        Result prior = query(
            db.get(),
            "SELECT operation, response_code, response_body "
            "FROM http_idempotency "
            "WHERE request_key = $1",
            key.c_str()
        );

        if (!rows(prior, 1)) {
            return fail();
        }

        std::string operation =
            PQgetvalue(prior.get(), 0, 0);

        if (operation != OPERATION) {
            if (!command(db.get(), "ROLLBACK")) {
                return error;
            }

            transaction_open = false;

            return {
                409,
                "Idempotency key conflicts with "
                "an earlier operation\n",
                false
            };
        }

        int code = std::atoi(
            PQgetvalue(prior.get(), 0, 1)
        );

        std::string body =
            PQgetvalue(prior.get(), 0, 2);

        if (code != 200) {
            return fail();
        }

        if (!command(db.get(), "COMMIT")) {
            return error;
        }

        transaction_open = false;

        return {code, body, false};
    }

    if (PQntuples(claim.get()) != 1) {
        return fail();
    }

    // This request successfully claimed a new key.
    // The increment and result storage share one transaction.
    Result counter = query(
        db.get(),
        "UPDATE counters "
        "SET value = value + 1 "
        "WHERE name = 'requests' "
        "RETURNING value"
    );

    if (!rows(counter, 1)) {
        return fail();
    }

    std::string body =
        std::string(PQgetvalue(counter.get(), 0, 0))
        + "\n";

    // Separate SQL statement: avoids the data-modifying
    // CTE issue discovered during F05.
    const char* vals[] = {
        key.c_str(),
        body.c_str()
    };

    Result saved(PQexecParams(
        db.get(),
        "UPDATE http_idempotency "
        "SET response_code = 200, response_body = $2 "
        "WHERE request_key = $1 "
        "RETURNING request_key",
        2,
        nullptr,
        vals,
        nullptr,
        nullptr,
        0
    ));

    if (!rows(saved, 1)) {
        return fail();
    }

    // COMMIT may succeed even when its acknowledgement
    // is lost. Never blindly repeat this transaction.
    if (!command(db.get(), "COMMIT")) {
        return error;
    }

    transaction_open = false;

    return {200, body, true};
}

bool send_all(
    int fd,
    const std::string& data
) {
    size_t pos = 0;

    while (pos < data.size()) {
        ssize_t n = send(
            fd,
            data.data() + pos,
            data.size() - pos,
            MSG_NOSIGNAL
        );

        if (n > 0) {
            pos += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }

    return true;
}

bool response(
    int fd,
    int status,
    const std::string& body
) {
    std::string reason =
        status == 200 ? "OK" :
        status == 400 ? "Bad Request" :
        status == 404 ? "Not Found" :
        status == 405 ? "Method Not Allowed" :
        status == 409 ? "Conflict" :
        status == 431 ? "Request Header Fields Too Large" :
        "Service Unavailable";

    return send_all(
        fd,
        "HTTP/1.1 " + std::to_string(status)
        + " " + reason + "\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: "
        + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n"
        + body
    );
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))
        );
    }

    return s;
}

std::string trim(std::string s) {
    size_t first = s.find_first_not_of(" \t");

    if (first == std::string::npos) {
        return "";
    }

    size_t last = s.find_last_not_of(" \t");

    return s.substr(first, last - first + 1);
}

struct Request {
    std::string method;
    std::string path;
    std::string key;
    int error = 0;
};

Request parse_request(int fd) {
    Request parsed;

    std::string data;
    char buf[2048];

    // Read until all headers have arrived.
    // A single recv() is not sufficient for HTTP parsing.
    while (data.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            parsed.error = 400;
            return parsed;
        }

        data.append(buf, static_cast<size_t>(n));

        if (data.size() > MAX_HEADERS) {
            parsed.error = 431;
            return parsed;
        }
    }

    size_t end = data.find("\r\n\r\n");
    size_t first_end = data.find("\r\n");

    if (first_end == std::string::npos ||
        first_end > end) {
        parsed.error = 400;
        return parsed;
    }

    std::string first = data.substr(0, first_end);

    size_t a = first.find(' ');

    size_t b = a == std::string::npos
        ? std::string::npos
        : first.find(' ', a + 1);

    if (a == std::string::npos ||
        b == std::string::npos ||
        first.substr(b + 1) != "HTTP/1.1") {
        parsed.error = 400;
        return parsed;
    }

    parsed.method = first.substr(0, a);
    parsed.path = first.substr(
        a + 1,
        b - a - 1
    );

    bool key_seen = false;
    bool len_seen = false;
    bool transfer_seen = false;

    size_t content_len = 0;
    size_t pos = first_end + 2;

    while (pos < end) {
        size_t next = data.find("\r\n", pos);

        if (next == std::string::npos ||
            next > end) {
            parsed.error = 400;
            return parsed;
        }

        std::string line = data.substr(
            pos,
            next - pos
        );

        size_t colon = line.find(':');

        if (colon == std::string::npos ||
            colon == 0 ||
            line[0] == ' ' ||
            line[0] == '\t') {
            parsed.error = 400;
            return parsed;
        }

        std::string name = lower(
            line.substr(0, colon)
        );

        std::string value = trim(
            line.substr(colon + 1)
        );

        if (name == "idempotency-key") {
            if (key_seen) {
                parsed.error = 400;
                return parsed;
            }

            key_seen = true;
            parsed.key = value;

        } else if (name == "content-length") {
            if (len_seen || value.empty()) {
                parsed.error = 400;
                return parsed;
            }

            len_seen = true;

            for (char c : value) {
                if (c < '0' ||
                    c > '9' ||
                    content_len > 100000) {
                    parsed.error = 400;
                    return parsed;
                }

                content_len =
                    content_len * 10 +
                    static_cast<size_t>(c - '0');
            }

        } else if (name == "transfer-encoding") {
            transfer_seen = true;
        }

        pos = next + 2;
    }

    // This endpoint accepts an empty request body only.
    if (transfer_seen ||
        content_len != 0 ||
        data.size() != end + 4) {
        parsed.error = 400;
        return parsed;
    }

    if (parsed.method == "POST" &&
        parsed.path == "/counter/increment") {

        if (!key_seen ||
            parsed.key.empty() ||
            parsed.key.size() > 128) {
            parsed.error = 400;
            return parsed;
        }

        for (unsigned char c : parsed.key) {
            if (!(
                (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '-' ||
                c == '_' ||
                c == '.' ||
                c == ':'
            )) {
                parsed.error = 400;
                return parsed;
            }
        }
    }

    return parsed;
}

int main() {
    Connection db(
        PQconnectdb(conninfo().c_str())
    );

    if (!db ||
        PQstatus(db.get()) != CONNECTION_OK) {
        std::cerr
            << "Initial PostgreSQL connection failed\n";
        return 1;
    }

    int server = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (server < 0) {
        std::perror("socket");
        return 1;
    }

    int reuse = 1;

    setsockopt(
        server,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(
            server,
            reinterpret_cast<sockaddr*>(&addr),
            sizeof(addr)
        ) < 0 ||
        listen(server, 128) < 0) {

        std::perror("bind/listen");
        close(server);
        return 1;
    }

    std::cerr
        << "Idempotent server listening on 127.0.0.1:"
        << PORT << "\n";

    Metrics metrics;

    // Test-only failure injection.
    // When enabled, suppress the first response after a
    // successful new COMMIT.
    bool fail_once =
        std::getenv(
            "RESILIENT_TEST_DROP_AFTER_COMMIT_ONCE"
        ) != nullptr;

    while (true) {
        int fd = accept(server, nullptr, nullptr);

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            std::perror("accept");
            break;
        }

        const auto started = Metrics::Clock::now();

        ++metrics.active_connections;

        timeval tv{};
        tv.tv_sec = 2;

        setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &tv,
            sizeof(tv)
        );

        setsockopt(
            fd,
            SOL_SOCKET,
            SO_SNDTIMEO,
            &tv,
            sizeof(tv)
        );

        auto send_result = [&](
            int code,
            const std::string& body
        ) {
            ++metrics.requests_total;

            if (code >= 400) {
                ++metrics.errors_total;
            }

            if (!response(fd, code, body)) {
                ++metrics.send_failures_total;
            }
        };

        Request req = parse_request(fd);

        if (req.error) {
            send_result(
                req.error,
                "Invalid HTTP request\n"
            );

        } else if (
            req.method == "GET" &&
            req.path == "/health"
        ) {
            send_result(200, "OK\n");

        } else if (
            req.method == "GET" &&
            req.path == "/ready"
        ) {
            bool healthy = ensure_connected(db);

            if (healthy) {
                Result r = query(
                    db.get(),
                    "SELECT 1"
                );

                healthy = rows(r, 1);
            }

            metrics.db_connection_up = healthy;

            send_result(
                healthy ? 200 : 503,
                healthy
                    ? "READY\n"
                    : "Database unavailable\n"
            );

        } else if (
            req.method == "GET" &&
            req.path == "/counter"
        ) {
            const auto t = Metrics::Clock::now();

            bool ok = ensure_connected(db);
            Result r;

            if (ok) {
                r = query(
                    db.get(),
                    "SELECT value FROM counters "
                    "WHERE name = 'requests'"
                );

                ok = rows(r, 1);
            }

            metrics.db_latency.observe(
                std::chrono::duration<double>(
                    Metrics::Clock::now() - t
                ).count()
            );

            metrics.db_connection_up =
                db &&
                PQstatus(db.get()) == CONNECTION_OK;

            send_result(
                ok ? 200 : 503,
                ok
                    ? std::string(
                        PQgetvalue(r.get(), 0, 0)
                    ) + "\n"
                    : "Database unavailable\n"
            );

        } else if (
            req.method == "GET" &&
            req.path == "/metrics"
        ) {
            send_result(200, metrics.render());

        } else if (
            req.method == "POST" &&
            req.path == "/counter/increment"
        ) {
            const auto t = Metrics::Clock::now();

            Outcome outcome = increment_once(
                db,
                req.key
            );

            metrics.db_latency.observe(
                std::chrono::duration<double>(
                    Metrics::Clock::now() - t
                ).count()
            );

            metrics.db_connection_up =
                db &&
                PQstatus(db.get()) == CONNECTION_OK;

            if (fail_once && outcome.committed_new) {
                // Simulate lost acknowledgement AFTER COMMIT.
                fail_once = false;

                ++metrics.requests_total;
                ++metrics.send_failures_total;

                close(fd);
                fd = -1;
            } else {
                send_result(
                    outcome.status,
                    outcome.body
                );
            }

        } else if (
            req.path == "/counter" ||
            req.path == "/counter/increment"
        ) {
            send_result(
                405,
                "Method not allowed\n"
            );

        } else {
            send_result(
                404,
                "Not found\n"
            );
        }

        metrics.http_latency.observe(
            std::chrono::duration<double>(
                Metrics::Clock::now() - started
            ).count()
        );

        --metrics.active_connections;

        if (fd >= 0) {
            close(fd);
        }
    }

    close(server);
    return 0;
}
