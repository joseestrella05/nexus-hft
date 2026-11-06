#pragma once

#include <cstdint>
#include <string_view>
#include <algorithm>
#include <iostream>

enum class EventType : uint8_t {
    TRADE = 1,
    QUOTE = 2
};

struct alignas(8) Tick {
    uint64_t timestamp_ns{0};
    double price{0.0};
    uint32_t volume{0};
    EventType type{EventType::TRADE};
    char symbol[7]{'\0'};

    // Constructor por defecto obligatorio para prealocación en std::array / SPSCQueue
    Tick() = default;

    Tick(uint64_t ts, double p, uint32_t v, EventType t, std::string_view sym)
        : timestamp_ns(ts), price(p), volume(v), type(t) {
        size_t len = std::min(sym.size(), sizeof(symbol) - 1);
        for (size_t i = 0; i < len; ++i) {
            symbol[i] = sym[i];
        }
        symbol[len] = '\0';
    }
};
