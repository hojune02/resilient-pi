#include <libpq-fe.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

struct ConnectionDeleter {
    void operator()(PGconn* conn) const {
        if (conn) PQfinish(conn);
    }
};

struct ResultDeleter {
    void operator()(PGresult* result) const {
        if (result) PQclear(result);
    }
};

using Connection = std::unique_ptr<PGconn, ConnectionDeleter>;
using Result = std::unique_ptr<PGresult, ResultDeleter>;

Result execute(PGconn* conn, const char* sql, ExecStatusType expected) {
    Result result(PQexec(conn, sql));

    if (!result || PQresultStatus(result.get()) != expected) {
        std::cerr << "SQL failed: " << PQerrorMessage(conn);
        std::exit(EXIT_FAILURE);
    }

    return result;
}

int main() {
    // No host specified: libpq uses a local Unix-domain socket.
    Connection conn(PQconnectdb("dbname=resilient_pi user=hojune"));

    if (PQstatus(conn.get()) != CONNECTION_OK) {
        std::cerr << "Connection failed: "
                  << PQerrorMessage(conn.get());
        return EXIT_FAILURE;
    }

    std::cout << "Connected to PostgreSQL\n";

    execute(conn.get(), "BEGIN", PGRES_COMMAND_OK);

    Result result = execute(
        conn.get(),
        "UPDATE counters "
        "SET value = value + 1 "
        "WHERE name = 'requests' "
        "RETURNING value",
        PGRES_TUPLES_OK
    );

    if (PQntuples(result.get()) != 1) {
        std::cerr << "Expected exactly one counter row\n";
        execute(conn.get(), "ROLLBACK", PGRES_COMMAND_OK);
        return EXIT_FAILURE;
    }

    std::string value = PQgetvalue(result.get(), 0, 0);

    execute(conn.get(), "COMMIT", PGRES_COMMAND_OK);

    std::cout << "Committed counter value: " << value << '\n';

    return EXIT_SUCCESS;
}
