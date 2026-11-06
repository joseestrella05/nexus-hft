#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include "SPSCQueue.hpp"

struct Message {
    uint64_t seq_num;
    double price;
    uint32_t volume;
};

int main() {
    constexpr size_t TOTAL_MESSAGES = 20'000'000;
    constexpr size_t QUEUE_CAPACITY = 65536; // 64K slots (potencia de 2)

    std::cout << "Inicializando SPSC Lock-Free Queue..." << std::endl;
    std::cout << "Capacidad del Buffer: " << QUEUE_CAPACITY << " slots" << std::endl;
    std::cout << "Alineacion de cache : " << HARDWARE_DESTRUCTIVE_SIZE << " bytes (ARM64 No False-Sharing)\n" << std::endl;

    auto queue = std::make_unique<SPSCQueue<Message, QUEUE_CAPACITY>>();

    std::cout << "Lanzando 2 hilos independientes para transferir " << TOTAL_MESSAGES << " eventos...\n";

    auto start = std::chrono::high_resolution_clock::now();

    // Hilo Consumidor
    std::thread consumer([&]() {
        size_t received = 0;
        Message msg{};
        while (received < TOTAL_MESSAGES) {
            if (queue->pop(msg)) {
                received++;
            } else {
                #if defined(__aarch64__)
                asm volatile("yield"); // Pausa de baja energía para core ARM
                #elif defined(__x86_64__)
                asm volatile("pause");
                #endif
            }
        }
    });

    // Hilo Productor
    std::thread producer([&]() {
        for (uint64_t i = 0; i < TOTAL_MESSAGES; ++i) {
            Message msg{i, 45000.0 + (i % 100), static_cast<uint32_t>(i % 50)};
            while (!queue->push(msg)) {
                #if defined(__aarch64__)
                asm volatile("yield");
                #elif defined(__x86_64__)
                asm volatile("pause");
                #endif
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;

    double seconds = elapsed.count() / 1000.0;
    double ops_per_sec = TOTAL_MESSAGES / seconds;
    double latency_ns = (elapsed.count() * 1'000'000.0) / TOTAL_MESSAGES;

    std::cout << "\n==========================================" << std::endl;
    std::cout << "     BENCHMARK DE CONCURRENCIA SPSC       " << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "Mensajes transferidos : " << TOTAL_MESSAGES << std::endl;
    std::cout << "Tiempo total de cómputo: " << elapsed.count() << " ms" << std::endl;
    std::cout << "Throughput concurrente : " 
              << std::fixed << std::setprecision(2)
              << (ops_per_sec / 1'000'000.0) << " Millones de msg/seg" << std::endl;
    std::cout << "Latencia media por msg : " << std::setprecision(2) << latency_ns << " nanosegundos" << std::endl;
    std::cout << "==========================================" << std::endl;

    return 0;
}
