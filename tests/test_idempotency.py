
#!/usr/bin/env python3

"""Integration tests for the idempotent HTTP server."""

import concurrent.futures
import subprocess
import urllib.error
import urllib.request
import uuid

BASE = "http://127.0.0.1:8085"


def request(
    path,
    method="GET",
    key=None,
    headers=None,
    data=None,
):
    h = dict(headers or {})

    if key is not None:
        h["Idempotency-Key"] = key

    req = urllib.request.Request(
        BASE + path,
        headers=h,
        method=method,
        data=data,
    )

    try:
        with urllib.request.urlopen(
            req,
            timeout=5,
        ) as r:
            return r.status, r.read().decode()

    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def sql(query):
    return subprocess.check_output(
        [
            "psql",
            "-X",
            "-At",
            "-v",
            "ON_ERROR_STOP=1",
            "-d",
            "resilient_pi",
            "-c",
            query,
        ],
        text=True,
    ).strip()


def check(condition, message):
    if not condition:
        raise AssertionError(message)

    print("PASS:", message)


def main():
    before = int(sql(
        "SELECT value FROM counters "
        "WHERE name = 'requests'"
    ))

    print("Initial counter:", before)

    suffix = uuid.uuid4().hex

    a = "integration-" + suffix + "-a"
    b = "integration-" + suffix + "-b"
    c = "integration-" + suffix + "-c"

    check(
        request("/health") == (200, "OK\n"),
        "health endpoint",
    )

    check(
        request("/ready") == (200, "READY\n"),
        "readiness endpoint",
    )

    check(
        request("/counter/increment", "POST")[0]
        == 400,
        "missing key rejected",
    )

    check(
        request(
            "/counter/increment",
            "POST",
            key="invalid key",
        )[0] == 400,
        "invalid key rejected",
    )

    check(
        request(
            "/counter/increment",
            "POST",
            key=a,
            data=b"payload",
        )[0] == 400,
        "request body rejected",
    )

    first = request(
        "/counter/increment",
        "POST",
        key=a,
    )

    check(
        first == (200, f"{before + 1}\n"),
        "first increment",
    )

    check(
        request(
            "/counter/increment",
            "POST",
            key=a,
        ) == first,
        "retry replays original response",
    )

    check(
        int(sql(
            "SELECT value FROM counters "
            "WHERE name = 'requests'"
        )) == before + 1,
        "retry did not increment",
    )

    # Concurrent callers reuse one logical request key.
    # The current HTTP server processes requests sequentially,
    # but PostgreSQL enforces uniqueness independently.
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=12
    ) as pool:
        futures = [
            pool.submit(
                request,
                "/counter/increment",
                "POST",
                b,
            )
            for _ in range(12)
        ]

        results = [
            f.result()
            for f in futures
        ]

    check(
        all(
            x == (200, f"{before + 2}\n")
            for x in results
        ),
        "12 concurrent same-key responses",
    )

    check(
        int(sql(
            "SELECT value FROM counters "
            "WHERE name = 'requests'"
        )) == before + 2,
        "concurrent requests increment once",
    )

    # Existing key associated with another operation.
    conflict_key = (
        "integration-" + suffix + "-conflict"
    )

    sql(
        "INSERT INTO http_idempotency "
        "(request_key, operation, response_code, response_body) "
        f"VALUES ('{conflict_key}', 'OTHER OPERATION', "
        "200, 'other')"
    )

    check(
        request(
            "/counter/increment",
            "POST",
            key=conflict_key,
        )[0] == 409,
        "conflicting operation returns 409",
    )

    new = request(
        "/counter/increment",
        "POST",
        key=c,
    )

    check(
        new == (200, f"{before + 3}\n"),
        "new key increments",
    )

    check(
        int(sql(
            "SELECT value FROM counters "
            "WHERE name = 'requests'"
        )) == before + 3,
        "final database counter",
    )

    check(
        sql(
            "SELECT response_body "
            "FROM http_idempotency "
            f"WHERE request_key = '{a}'"
        ) == str(before + 1),
        "stored response matches first result",
    )

    print(
        "ALL TESTS PASSED; counter increased by 3"
    )


if __name__ == "__main__":
    main()
