#include <iostream>
#include <chrono>
#include <iomanip>
#include "OrderBook.hpp"

int main() {
    LimitOrderBook<50> book;

    std::cout << "Inicializando Limit Order Book en memoria contigua..." << std::endl;

    // 1. Cargar niveles iniciales del libro de órdenes
    book.update_level(true,  44000.00, 1.50); // Bid 1
    book.update_level(true,  43999.50, 3.20); // Bid 2
    book.update_level(true,  43998.00, 5.00); // Bid 3
    book.update_level(true,  43995.00, 12.00); // Bid 4

    book.update_level(false, 44000.50, 0.80); // Ask 1
    book.update_level(false, 44001.00, 1.20); // Ask 2
    book.update_level(false, 44002.50, 4.00); // Ask 3
    book.update_level(false, 44005.00, 10.00); // Ask 4

    book.print_top_of_book();

    // 2. Simular llenado de orden de 2.0 BTC a mercado para ver el barrido del libro
    double target_btc = 2.0;
    double fill_price = book.calculate_market_impact_fill(true, target_btc);
    double theoretical_best = book.best_ask();
    double slippage_dollars = (fill_price - theoretical_best) * target_btc;

    std::cout << "Simulación de compra de " << target_btc << " BTC a mercado:" << std::endl;
    std::cout << "Precio Best Ask inicial   : $" << std::fixed << std::setprecision(2) << theoretical_best << std::endl;
    std::cout << "Precio real promedio VWAP : $" << fill_price << std::endl;
    std::cout << "Slippage total por impacto: $" << slippage_dollars << " USD\n" << std::endl;

    // 3. Benchmark de latencia de inserción y cálculo de OBI
    constexpr size_t ITERATIONS = 1'000'000;
    std::cout << "Ejecutando " << ITERATIONS << " actualizaciones del L2 Order Book..." << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < ITERATIONS; ++i) {
        double offset = (i % 20) * 0.1;
        book.update_level(true, 44000.0 - offset, 2.0 + (i % 5));
        book.update_level(false, 44000.5 + offset, 1.5 + (i % 4));
        volatile double obi = book.order_book_imbalance(5);
        (void)obi;
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::nano> total_ns = end - start;

    double latency_per_update_ns = total_ns.count() / (ITERATIONS * 2);

    std::cout << "Tiempo total de cómputo   : " << (total_ns.count() / 1e6) << " ms" << std::endl;
    std::cout << "Latencia media por update : " << std::fixed << std::setprecision(2) 
              << latency_per_update_ns << " nanosegundos" << std::endl;
    std::cout << "Throughput del Order Book : " 
              << ((ITERATIONS * 2) / (total_ns.count() / 1e9)) / 1'000'000.0 
              << " Millones de updates/seg" << std::endl;

    return 0;
}
