#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

struct Histogram {
    static constexpr std::array<double, 12> bounds = {
        0.001, 0.0025, 0.005, 0.01,
        0.025, 0.05, 0.1, 0.25,
        0.5, 1.0, 2.5, 5.0
    };

    std::array<std::uint64_t, bounds.size()> buckets{};
    std::uint64_t count = 0;
    double sum = 0.0;

    void observe(double seconds) {
        ++count;
        sum += seconds;

        for (std::size_t i = 0; i < bounds.size(); ++i) {
            if (seconds <= bounds[i]) {
                ++buckets[i];
            }
        }
    }

    std::string render(const std::string& name) const {
        std::ostringstream out;
        out << std::setprecision(12);

        for (std::size_t i = 0; i < bounds.size(); ++i) {
            out << name << "_bucket{le=\""
                << bounds[i] << "\"} "
                << buckets[i] << "\n";
        }

        out << name << "_bucket{le=\"+Inf\"} "
            << count << "\n";

        out << name << "_sum " << sum << "\n";
        out << name << "_count " << count << "\n";

        return out.str();
    }
};

struct Metrics {
    using Clock = std::chrono::steady_clock;

    Clock::time_point started_at = Clock::now();

    std::uint64_t requests_total = 0;
    std::uint64_t errors_total = 0;
    std::uint64_t send_failures_total = 0;

    std::uint64_t active_connections = 0;

    // Last known connection state, not a live database probe.
    bool db_connection_up = true;

    Histogram http_latency;
    Histogram db_latency;

    std::string render() const {
        std::ostringstream out;

        out << "# HELP resilient_http_requests_total "
            << "Completed HTTP request-handling attempts\n";
        out << "# TYPE resilient_http_requests_total counter\n";
        out << "resilient_http_requests_total "
            << requests_total << "\n";

        out << "# HELP resilient_http_errors_total "
            << "HTTP responses with status 4xx or 5xx\n";
        out << "# TYPE resilient_http_errors_total counter\n";
        out << "resilient_http_errors_total "
            << errors_total << "\n";

        out << "# HELP resilient_http_send_failures_total "
            << "Failed response transmissions\n";
        out << "# TYPE resilient_http_send_failures_total counter\n";
        out << "resilient_http_send_failures_total "
            << send_failures_total << "\n";

        out << "# TYPE resilient_http_active_connections gauge\n";
        out << "resilient_http_active_connections "
            << active_connections << "\n";

        out << "# TYPE resilient_process_uptime_seconds gauge\n";
        out << "resilient_process_uptime_seconds "
            << std::chrono::duration<double>(
                   Clock::now() - started_at
               ).count()
            << "\n";

        out << "# TYPE resilient_db_connection_up gauge\n";
        out << "resilient_db_connection_up "
            << (db_connection_up ? 1 : 0) << "\n";

        out << "# TYPE resilient_http_request_duration_seconds histogram\n";
        out << http_latency.render(
            "resilient_http_request_duration_seconds"
        );

        out << "# TYPE resilient_db_operation_duration_seconds histogram\n";
        out << db_latency.render(
            "resilient_db_operation_duration_seconds"
        );

        return out.str();
    }
};
