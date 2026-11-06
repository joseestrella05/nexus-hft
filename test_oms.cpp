#include <iostream>
#include <chrono>
#include <iomanip>
#include "OMS.hpp"

int main() {
    // Claves simuladas estándar de API
    const std::string API_KEY = "vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A";
    const std::string SECRET_KEY = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjBakUDgfGEvTuyrqCRpzqVoGttf";

    OrderManagementSystem oms(API_KEY, SECRET_KEY);

    std::cout << "Inicializando Order Management System (OMS)..." << std::endl;

    // 1. Probar firmado de una orden individual
    uint64_t current_time_ms = 1704067200000ULL; // 2024-01-01 00:00:00 UTC
    Order& buy_order = oms.create_order(Side::BUY, 44250.00, 0.5, OrderType::MARKET, "BTCUSDT");

    std::string payload = oms.build_signed_payload(buy_order, current_time_ms);

    std::cout << "\n--- PAYLOAD AUTENTICADO GENERADO ---" << std::endl;
    std::cout << payload << std::endl;
    std::cout << "------------------------------------\n" << std::endl;

    // 2. Benchmark de latencia criptográfica
    constexpr size_t ITERATIONS = 100'000;
    std::cout << "Midiendo tiempo de serialización y firma HMAC-SHA256 sobre " 
              << ITERATIONS << " órdenes..." << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < ITERATIONS; ++i) {
        Order& ord = oms.create_order(Side::BUY, 44000.0 + (i % 100), 0.25, OrderType::LIMIT, "BTCUSDT");
        std::string req = oms.build_signed_payload(ord, current_time_ms + i);
        (void)req; // Evitar que el optimizador elimine el cálculo
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::nano> total_ns = end - start;

    double latency_per_order_ns = total_ns.count() / ITERATIONS;
    double orders_per_sec = (ITERATIONS / (total_ns.count() / 1e9));

    std::cout << "Tiempo total de cómputo : " << (total_ns.count() / 1e6) << " ms" << std::endl;
    std::cout << "Latencia media por orden: " << std::fixed << std::setprecision(2) 
              << latency_per_order_ns << " nanosegundos (" 
              << (latency_per_order_ns / 1000.0) << " microsegundos)" << std::endl;
    std::cout << "Throughput de firmado   : " << std::setprecision(2)
              << (orders_per_sec / 1'000.0) << " miles de órdenes/seg" << std::endl;

    return 0;
}
