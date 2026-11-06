#pragma once

#include <cstdint>
#include <array>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cmath>

struct PriceLevel {
    double price{0.0};
    double quantity{0.0};
};

template <size_t MaxLevels = 50>
class LimitOrderBook {
private:
    // Arrays contiguos en memoria caché L1 para Bids (Compras) y Asks (Ventas)
    std::array<PriceLevel, MaxLevels> bids_{};
    std::array<PriceLevel, MaxLevels> asks_{};
    size_t bid_count_{0};
    size_t ask_count_{0};

public:
    LimitOrderBook() = default;

    // Actualiza o inserta un nivel de precio en O(N) cache-friendly (N <= 50 cabe en 1-2 lineas de cache)
    void update_level(bool is_buy, double price, double quantity) noexcept {
        auto& levels = is_buy ? bids_ : asks_;
        auto& count = is_buy ? bid_count_ : ask_count_;

        // 1. Si la cantidad es 0.0, eliminar el nivel
        if (quantity <= 1e-8) {
            for (size_t i = 0; i < count; ++i) {
                if (std::abs(levels[i].price - price) < 1e-4) {
                    for (size_t j = i; j < count - 1; ++j) {
                        levels[j] = levels[j + 1];
                    }
                    count--;
                    return;
                }
            }
            return;
        }

        // 2. Buscar si el nivel ya existe para actualizar volumen
        for (size_t i = 0; i < count; ++i) {
            if (std::abs(levels[i].price - price) < 1e-4) {
                levels[i].quantity = quantity;
                return;
            }
        }

        // 3. Insertar nuevo nivel manteniendo orden estricto de precios:
        // Bids: orden descendente (mayor precio primero)
        // Asks: orden ascendente (menor precio primero)
        if (count < MaxLevels) {
            size_t insert_pos = 0;
            while (insert_pos < count) {
                bool should_insert = is_buy ? (price > levels[insert_pos].price)
                                            : (price < levels[insert_pos].price);
                if (should_insert) break;
                insert_pos++;
            }

            for (size_t j = count; j > insert_pos; --j) {
                levels[j] = levels[j - 1];
            }
            levels[insert_pos] = PriceLevel{price, quantity};
            count++;
        }
    }

    [[nodiscard]] double best_bid() const noexcept {
        return bid_count_ > 0 ? bids_[0].price : 0.0;
    }

    [[nodiscard]] double best_ask() const noexcept {
        return ask_count_ > 0 ? asks_[0].price : 0.0;
    }

    [[nodiscard]] double spread() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return 0.0;
        return asks_[0].price - bids_[0].price;
    }

    [[nodiscard]] double mid_price() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return 0.0;
        return (bids_[0].price + asks_[0].price) * 0.5;
    }

    // Señal cuantitativa de HFT: Order Book Imbalance (OBI) entre [-1.0, 1.0]
    // OBI > 0 -> Presión compradora dominante
    // OBI < 0 -> Presión vendedora dominante
    [[nodiscard]] double order_book_imbalance(size_t depth = 5) const noexcept {
        size_t d = std::min({depth, bid_count_, ask_count_});
        if (d == 0) return 0.0;

        double total_bid_qty = 0.0;
        double total_ask_qty = 0.0;

        for (size_t i = 0; i < d; ++i) {
            total_bid_qty += bids_[i].quantity;
            total_ask_qty += asks_[i].quantity;
        }

        double denominator = total_bid_qty + total_ask_qty;
        if (denominator < 1e-8) return 0.0;

        return (total_bid_qty - total_ask_qty) / denominator;
    }

    // Calcula el precio ponderado por volumen (VWAP) para simular un llenado de mercado real
    [[nodiscard]] double calculate_market_impact_fill(bool is_buy, double target_qty) const noexcept {
        const auto& levels = is_buy ? asks_ : bids_;
        const auto& count = is_buy ? ask_count_ : bid_count_;

        double remaining = target_qty;
        double total_cost = 0.0;

        for (size_t i = 0; i < count && remaining > 0.0; ++i) {
            double take = std::min(remaining, levels[i].quantity);
            total_cost += take * levels[i].price;
            remaining -= take;
        }

        if (remaining > 0.0) {
            // Liquidez insuficiente en el libro
            return 0.0;
        }
        return total_cost / target_qty;
    }

    void print_top_of_book() const {
        std::cout << "\n--- TOP OF BOOK (L2) ---" << std::endl;
        std::cout << "Best Bid : $" << std::fixed << std::setprecision(2) << best_bid() 
                  << " (Vol: " << (bid_count_ > 0 ? bids_[0].quantity : 0.0) << " BTC)" << std::endl;
        std::cout << "Best Ask : $" << best_ask() 
                  << " (Vol: " << (ask_count_ > 0 ? asks_[0].quantity : 0.0) << " BTC)" << std::endl;
        std::cout << "Spread   : $" << spread() << std::endl;
        std::cout << "Mid Price: $" << mid_price() << std::endl;
        std::cout << "OBI (5)  : " << std::setprecision(4) << order_book_imbalance(5) << std::endl;
        std::cout << "------------------------\n" << std::endl;
    }
};
