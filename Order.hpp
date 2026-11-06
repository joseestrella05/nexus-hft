#pragma once

#include <cstdint>
#include <string_view>
#include <algorithm>

enum class Side : uint8_t {
    BUY = 1,
    SELL = 2
};

enum class OrderType : uint8_t {
    MARKET = 1,
    LIMIT = 2
};

enum class OrderStatus : uint8_t {
    NEW = 0,
    PENDING_SUBMIT = 1,
    FILLED = 2,
    PARTIALLY_FILLED = 3,
    CANCELLED = 4,
    REJECTED = 5
};

struct Order {
    uint64_t cl_ord_id{0};
    uint64_t timestamp_ns{0};
    double price{0.0};
    double quantity{0.0};
    double filled_qty{0.0};
    double avg_fill_price{0.0};
    Side side{Side::BUY};
    OrderType type{OrderType::MARKET};
    OrderStatus status{OrderStatus::NEW};
    char symbol[8]{"BTCUSDT"};

    Order() = default;

    Order(uint64_t id, uint64_t ts, double p, double q, Side s, OrderType t, std::string_view sym)
        : cl_ord_id(id), timestamp_ns(ts), price(p), quantity(q), side(s), type(t) {
        size_t len = std::min(sym.size(), sizeof(symbol) - 1);
        for (size_t i = 0; i < len; ++i) symbol[i] = sym[i];
        symbol[len] = '\0';
    }
};
