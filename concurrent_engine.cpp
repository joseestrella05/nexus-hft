#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <deque>
#include <algorithm>
#include "SPSCQueue.hpp"
#include "Arena.hpp"
#include "MarketData.hpp"
#include "Portfolio.hpp"
#include "FastFileReader.hpp"

// Parser numérico rápido
inline uint64_t parse_uint64(const char*& ptr) {
    uint64_t val = 0;
    while (*ptr >= '0' && *ptr <= '9') {
        val = val * 10 + (*ptr - '0');
        ptr++;
    }
    return val;
}

inline double parse_double(const char*& ptr) {
    double int_part = 0.0;
    while (*ptr >= '0' && *ptr <= '9') {
        int_part = int_part * 10.0 + (*ptr - '0');
        ptr++;
    }
    if (*ptr == '.') {
        ptr++;
        double factor = 0.1;
        while (*ptr >= '0' && *ptr <= '9') {
            int_part += (*ptr - '0') * factor;
            factor *= 0.1;
            ptr++;
        }
    }
    return int_part;
}

inline void skip_column(const char*& ptr, const char* end) {
    while (ptr < end && *ptr != ',' && *ptr != '\n') ptr++;
    if (ptr < end && *ptr == ',') ptr++;
}

class LiveMomentumStrategy {
private:
    Portfolio& portfolio_;
    double entry_price_{0.0};
    double highest_price_since_entry_{0.0};
    double last_price_{0.0};

    static constexpr double POSITION_SIZE_BTC = 0.5;
    static constexpr size_t WINDOW_SIZE = 600;
    std::deque<double> price_window_;
    std::deque<uint32_t> volume_window_;
    uint64_t volume_sum_{0};

    static constexpr double INITIAL_STOP_PCT = 0.005;
    static constexpr double TRAILING_STOP_PCT = 0.004;
    
    uint64_t ticks_since_exit_{1000};
    static constexpr uint64_t COOLDOWN_TICKS = 400;

public:
    explicit LiveMomentumStrategy(Portfolio& p) : portfolio_(p) {}

    void on_tick(const Tick& tick) {
        last_price_ = tick.price;
        ticks_since_exit_++;

        price_window_.push_back(tick.price);
        volume_window_.push_back(tick.volume);
        volume_sum_ += tick.volume;

        if (price_window_.size() > WINDOW_SIZE) {
            price_window_.pop_front();
            volume_sum_ -= volume_window_.front();
            volume_window_.pop_front();
        }

        if (price_window_.size() < WINDOW_SIZE) return;

        if (portfolio_.get_position() > 0.0) {
            if (tick.price > highest_price_since_entry_) {
                highest_price_since_entry_ = tick.price;
            }

            double trailing_stop = highest_price_since_entry_ * (1.0 - TRAILING_STOP_PCT);
            double hard_stop = entry_price_ * (1.0 - INITIAL_STOP_PCT);
            double exit_trigger = std::max(trailing_stop, hard_stop);

            if (tick.price <= exit_trigger) {
                portfolio_.sell(tick.price, POSITION_SIZE_BTC);
                entry_price_ = 0.0;
                highest_price_since_entry_ = 0.0;
                ticks_since_exit_ = 0;
            }
        } else if (portfolio_.get_position() == 0.0 && ticks_since_exit_ >= COOLDOWN_TICKS) {
            double prev_high = 0.0;
            for (size_t i = 0; i < price_window_.size() - 1; ++i) {
                if (price_window_[i] > prev_high) prev_high = price_window_[i];
            }

            double avg_vol = static_cast<double>(volume_sum_) / WINDOW_SIZE;

            if (tick.price > prev_high && tick.volume > (avg_vol * 1.3)) {
                if (portfolio_.buy(tick.price, POSITION_SIZE_BTC)) {
                    entry_price_ = tick.price;
                    highest_price_since_entry_ = tick.price;
                }
            }
        }
    }

    [[nodiscard]] double get_last_price() const noexcept { return last_price_; }
};

int main() {
    constexpr size_t QUEUE_CAPACITY = 65536; // 64K slots
    constexpr size_t ARENA_SIZE = 128 * 1024 * 1024;
    const std::string datafile = "../arena_allocator/BTCUSDT-trades-2024-01-01.csv";

    std::cout << "Inicializando Pipeline de Trading Concurrente Multihilo..." << std::endl;
    FastFileReader file_reader(datafile);

    auto queue = std::make_unique<SPSCQueue<Tick, QUEUE_CAPACITY>>();
    std::atomic<bool> producer_finished{false};

    Portfolio portfolio(100'000.0);
    LiveMomentumStrategy strategy(portfolio);
    ArenaAllocator arena(ARENA_SIZE);

    const char* curr = file_reader.data();
    const char* end = curr + file_reader.size();

    auto start = std::chrono::high_resolution_clock::now();
    uint64_t processed_ticks = 0;

    // HILO 2: Consumidor (Estrategia + Gestión de Memoria Arena)
    std::thread consumer([&]() {
        Tick tick_buffer{0, 0.0, 0, EventType::TRADE, ""};
        while (true) {
            if (queue->pop(tick_buffer)) {
                // Asignar en Arena y despachar
                Tick* t = arena.create<Tick>(
                    tick_buffer.timestamp_ns,
                    tick_buffer.price,
                    tick_buffer.volume,
                    tick_buffer.type,
                    "BTC"
                );
                strategy.on_tick(*t);
                processed_ticks++;

                if (processed_ticks % 100'000 == 0) {
                    arena.reset(); // Limpieza periódica de buffer en O(1)
                }
            } else if (producer_finished.load(std::memory_order_acquire)) {
                // Drenar lo que quede pendiente antes de salir
                while (queue->pop(tick_buffer)) {
                    strategy.on_tick(tick_buffer);
                    processed_ticks++;
                }
                break;
            } else {
                #if defined(__aarch64__)
                asm volatile("yield");
                #elif defined(__x86_64__)
                asm volatile("pause");
                #endif
            }
        }
    });

    // HILO 1: Productor (Ingesta I/O zero-copy desde archivo mapeado)
    std::thread producer([&]() {
        while (curr < end) {
            skip_column(curr, end); // trade_id
            double price = parse_double(curr);
            if (curr < end && *curr == ',') curr++;

            double qty = parse_double(curr);
            if (curr < end && *curr == ',') curr++;

            skip_column(curr, end); // quote_qty
            uint64_t ts_ns = parse_uint64(curr) * 1'000'000ULL;

            while (curr < end && *curr != '\n') curr++;
            if (curr < end) curr++;

            Tick t(ts_ns, price, static_cast<uint32_t>(qty * 100), EventType::TRADE, "BTC");
            
            while (!queue->push(t)) {
                #if defined(__aarch64__)
                asm volatile("yield");
                #elif defined(__x86_64__)
                asm volatile("pause");
                #endif
            }
        }
        producer_finished.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end_time - start;

    std::cout << "\n==========================================" << std::endl;
    std::cout << " PIPELINE MULTIHILO CONCURRENTE EJECUTADO " << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "Trades procesados      : " << processed_ticks << std::endl;
    std::cout << "Tiempo total de corrida: " << elapsed.count() << " ms" << std::endl;
    std::cout << "Throughput concurrente : " 
              << std::fixed << std::setprecision(2)
              << (processed_ticks / (elapsed.count() / 1000.0)) / 1'000'000.0
              << " M ticks/seg" << std::endl;

    portfolio.print_summary(strategy.get_last_price());
    arena.reset();

    return 0;
}
