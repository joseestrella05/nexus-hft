#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <deque>
#include <algorithm>
#include "WebSocketClient.hpp"
#include "SPSCQueue.hpp"
#include "MarketData.hpp"
#include "OrderBook.hpp"
#include "OMS.hpp"

inline double extract_json_double(std::string_view json, std::string_view key) {
    size_t pos = json.find(key);
    if (pos == std::string_view::npos) return 0.0;
    pos += key.length();
    while (pos < json.size() && (json[pos] == ':' || json[pos] == '\"' || json[pos] == ' ')) pos++;
    
    double val = 0.0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10.0 + (json[pos] - '0');
        pos++;
    }
    if (pos < json.size() && json[pos] == '.') {
        pos++;
        double factor = 0.1;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            val += (json[pos] - '0') * factor;
            factor *= 0.1;
            pos++;
        }
    }
    return val;
}

inline uint64_t extract_json_uint64(std::string_view json, std::string_view key) {
    size_t pos = json.find(key);
    if (pos == std::string_view::npos) return 0;
    pos += key.length();
    while (pos < json.size() && (json[pos] == ':' || json[pos] == '\"' || json[pos] == ' ')) pos++;
    
    uint64_t val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    return val;
}

int main() {
    constexpr size_t QUEUE_CAPACITY = 65536;
    auto queue = std::make_unique<SPSCQueue<Tick, QUEUE_CAPACITY>>();
    std::atomic<bool> is_running{true};

    const std::string API_KEY = "live_production_key_sample";
    const std::string SECRET_KEY = "live_production_secret_sample";
    OrderManagementSystem oms(API_KEY, SECRET_KEY);

    std::cout << "==================================================" << std::endl;
    std::cout << "   INICIANDO MOTOR QUANTITATIVO EN TIEMPO REAL   " << std::endl;
    std::cout << "==================================================" << std::endl;

    // HILO 2: Motor de Estrategia y Ejecución (Consumidor)
    std::thread trading_thread([&]() {
        Tick tick{};
        LimitOrderBook<50> book;
        std::deque<double> price_window;
        constexpr size_t WINDOW_SIZE = 30;
        double highest_since_entry = 0.0;
        double entry_price = 0.0;
        bool in_position = false;

        std::cout << "[Trading Engine] Escuchando eventos del ring buffer...\n";

        while (is_running.load(std::memory_order_relaxed)) {
            if (queue->pop(tick)) {
                book.update_level(true, tick.price - 0.5, 1.5);
                book.update_level(false, tick.price + 0.5, 1.5);

                price_window.push_back(tick.price);
                if (price_window.size() > WINDOW_SIZE) {
                    price_window.pop_front();
                }

                if (price_window.size() < WINDOW_SIZE) continue;

                // 1. Gestión de posición abierta: Trailing Stop y Hard Stop
                if (in_position) {
                    if (tick.price > highest_since_entry) {
                        highest_since_entry = tick.price;
                    }

                    double trailing_stop = highest_since_entry * 0.998; // 0.2% trailing stop
                    double hard_stop = entry_price * 0.995;            // 0.5% stop de seguridad inicial
                    double exit_trigger = std::max(trailing_stop, hard_stop);

                    if (tick.price <= exit_trigger) {
                        Order& ord = oms.create_order(Side::SELL, tick.price, 0.01, OrderType::MARKET, "BTCUSDT");
                        std::string signed_payload = oms.build_signed_payload(ord, tick.timestamp_ns / 1'000'000ULL);

                        std::cout << "\n>>> [EJECUCION SELL] Salida activada a $" << tick.price << std::endl;
                        std::cout << ">>> Payload Firmado (HMAC-SHA256): " << signed_payload << "\n" << std::endl;

                        in_position = false;
                        entry_price = 0.0;
                        highest_since_entry = 0.0;
                    }
                } 
                // 2. Búsqueda de Ruptura de Momentum
                else {
                    double prev_high = 0.0;
                    for (size_t i = 0; i < price_window.size() - 1; ++i) {
                        if (price_window[i] > prev_high) prev_high = price_window[i];
                    }

                    if (tick.price > prev_high) {
                        Order& ord = oms.create_order(Side::BUY, tick.price, 0.01, OrderType::MARKET, "BTCUSDT");
                        std::string signed_payload = oms.build_signed_payload(ord, tick.timestamp_ns / 1'000'000ULL);

                        std::cout << "\n>>> [EJECUCION BUY] Ruptura detectada a $" << tick.price << std::endl;
                        std::cout << ">>> Payload Firmado (HMAC-SHA256): " << signed_payload << "\n" << std::endl;

                        in_position = true;
                        entry_price = tick.price;
                        highest_since_entry = tick.price;
                    }
                }

                static uint64_t counter = 0;
                if (++counter % 20 == 0) {
                    std::cout << "[LIVE TICK] BTCUSDT: $" << std::fixed << std::setprecision(2) 
                              << tick.price << " | Vol: " << tick.volume 
                              << " | En posicion: " << (in_position ? "SI" : "NO") << std::endl;
                }
            } else {
                #if defined(__aarch64__)
                asm volatile("yield");
                #elif defined(__x86_64__)
                asm volatile("pause");
                #endif
            }
        }
    });

    // HILO 1: Productor de Red (WebSocket Client)
    std::thread network_thread([&]() {
        try {
            std::cout << "[Network Thread] Conectando a stream.binance.com:9443 vía TLS...\n";
            WebSocketClient ws("stream.binance.com", "9443", "/ws/btcusdt@trade");
            std::cout << "[Network Thread] Handshake WebSocket completado exitosamente.\n";

            std::string payload;
            size_t packets_received = 0;

            while (packets_received < 100) {
                if (ws.read_message(payload)) {
                    double price = extract_json_double(payload, "\"p\"");
                    double qty = extract_json_double(payload, "\"q\"");
                    uint64_t time_ms = extract_json_uint64(payload, "\"T\"");

                    if (price > 0.0) {
                        Tick t(time_ms * 1'000'000ULL, price, static_cast<uint32_t>(qty * 1000), EventType::TRADE, "BTC");
                        while (!queue->push(t)) {
                            #if defined(__aarch64__)
                            asm volatile("yield");
                            #endif
                        }
                        packets_received++;
                    }
                }
            }

            std::cout << "\n[Network Thread] Se recibieron y procesaron 100 trades en vivo desde la bolsa.\n";
            is_running.store(false);

        } catch (const std::exception& e) {
            std::cerr << "[Network Thread Error] " << e.what() << std::endl;
            is_running.store(false);
        }
    });

    network_thread.join();
    trading_thread.join();

    std::cout << "\nSesion en tiempo real finalizada con exito." << std::endl;
    return 0;
}
