#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <atomic>
#include <thread>

// Mocking the 'titan-vm-engine' interface for the microbenchmark context.
// In a real integration, this would include the actual engine headers.
namespace titan_vm_engine {
    class VirtualMachine {
    public:
        void execute_instruction_cycle() {
            // Simulate lightweight VM execution overhead (e.g., dispatch loop, state update)
            // Using a light compiler barrier / volatile operation to prevent aggressive optimization.
            volatile uint64_t counter = 0;
            counter++;
        }
    };
}

class LatencyTracker {
private:
    std::vector<uint64_t> latencies_ns;
public:
    explicit LatencyTracker(size_t capacity) {
        latencies_ns.reserve(capacity);
    }

    inline void record(uint64_t ns) {
        latencies_ns.push_back(ns);
    }

    void finalize() {
        std::sort(latencies_ns.begin(), latencies_ns.end());
    }

    uint64_t get_percentile(double p) const {
        if (latencies_ns.empty()) return 0;
        size_t index = static_cast<size_t>(p * (latencies_ns.size() - 1));
        return latencies_ns[index];
    }
};

int main() {
    // Benchmark configuration parameters
    const size_t WARMUP_ITERATIONS = 1'000'000;
    const size_t BENCHMARK_ITERATIONS = 10'000'000;

    titan_vm_engine::VirtualMachine vm;
    LatencyTracker tracker(BENCHMARK_ITERATIONS);

    // 1. Warm-up phase (stabilizes CPU frequency, populates instruction caches, warms branch predictors)
    for (size_t i = 0; i < WARMUP_ITERATIONS; ++i) {
        vm.execute_instruction_cycle();
    }

    // 2. Measurement phase (High-resolution latency tracking per operation)
    auto total_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < BENCHMARK_ITERATIONS; ++i) {
        auto op_start = std::chrono::high_resolution_clock::now();
        
        vm.execute_instruction_cycle();
        
        auto op_end = std::chrono::high_resolution_clock::now();
        auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(op_end - op_start).count();
        
        tracker.record(static_cast<uint64_t>(duration_ns));
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_duration_sec = std::chrono::duration<double>(total_end - total_start).count();

    // 3. Compute Metrics
    tracker.finalize();
    double ops_per_sec = static_cast<double>(BENCHMARK_ITERATIONS) / total_duration_sec;
    uint64_t p50_ns = tracker.get_percentile(0.50);
    uint64_t p99_ns = tracker.get_percentile(0.99);
    uint64_t p999_ns = tracker.get_percentile(0.999);

    // 4. Output Formatted Tabular Metrics to stdout
    std::cout << "\n";
    std::cout << "=================================================================\n";
    std::cout << "          titan-vm-engine Microbenchmark Results                 \n";
    std::cout << "=================================================================\n";
    std::cout << "| Metric               | Value                  | Unit          |\n";
    std::cout << "|---------------------------------------------------------------|\n";
    std::cout << "| Total Iterations     | " << std::setw(22) << BENCHMARK_ITERATIONS << " | ops           |\n";
    std::cout << "| Total Execution Time | " << std::setw(22) << std::fixed << std::setprecision(4) << total_duration_sec << " | s             |\n";
    std::cout << "| Throughput           | " << std::setw(22) << std::scientific << std::setprecision(2) << ops_per_sec << " | ops/sec       |\n";
    std::cout << "| Latency p50          | " << std::setw(22) << std::defaultfloat << p50_ns << " | ns            |\n";
    std::cout << "| Latency p99          | " << std::setw(22) << p99_ns << " | ns            |\n";
    std::cout << "| Latency p99.9        | " << std::setw(22) << p999_ns << " | ns            |\n";
    std::cout << "=================================================================\n\n";

    return 0;
}