#include "../../server/metrics.hpp"

#include <iostream>

int main() {
    Metrics metrics;

    metrics.requests_total = 3;
    metrics.errors_total = 1;

    metrics.http_latency.observe(0.003);
    metrics.http_latency.observe(0.012);
    metrics.http_latency.observe(0.150);

    metrics.db_latency.observe(0.002);

    std::cout << metrics.render();
}
