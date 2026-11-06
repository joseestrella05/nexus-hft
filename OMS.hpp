#pragma once

#include "Order.hpp"
#include <CommonCrypto/CommonHMAC.h>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>

class OrderManagementSystem {
private:
    std::string api_key_;
    std::string secret_key_;
    uint64_t next_order_id_{1};
    std::vector<Order> active_orders_;

    void hmac_sha256(const std::string& key, const std::string& data, char* output_hex) const {
        unsigned char hmac[CC_SHA256_DIGEST_LENGTH];
        CCHmac(
            kCCHmacAlgSHA256,
            key.data(),
            key.size(),
            data.data(),
            data.size(),
            hmac
        );

        static const char hex_chars[] = "0123456789abcdef";
        for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
            output_hex[i * 2]     = hex_chars[(hmac[i] >> 4) & 0x0F];
            output_hex[i * 2 + 1] = hex_chars[hmac[i] & 0x0F];
        }
        output_hex[CC_SHA256_DIGEST_LENGTH * 2] = '\0';
    }

public:
    OrderManagementSystem(std::string api_key, std::string secret_key)
        : api_key_(std::move(api_key)), secret_key_(std::move(secret_key)) {
        active_orders_.reserve(10'000);
    }

    Order& create_order(Side side, double price, double quantity, OrderType type, std::string_view symbol = "BTCUSDT") {
        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();

        active_orders_.emplace_back(next_order_id_++, now_ns, price, quantity, side, type, symbol);
        return active_orders_.back();
    }

    std::string build_signed_payload(Order& order, uint64_t timestamp_ms) {
        order.status = OrderStatus::PENDING_SUBMIT;

        std::string payload;
        payload.reserve(256);
        payload += "symbol=" + std::string(order.symbol);
        payload += "&side=" + std::string(order.side == Side::BUY ? "BUY" : "SELL");
        payload += "&type=" + std::string(order.type == OrderType::MARKET ? "MARKET" : "LIMIT");
        
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(4) << order.quantity;
        payload += "&quantity=" + ss.str();

        if (order.type == OrderType::LIMIT) {
            std::ostringstream ss_price;
            ss_price << std::fixed << std::setprecision(2) << order.price;
            payload += "&price=" + ss_price.str();
            payload += "&timeInForce=GTC";
        }

        payload += "&timestamp=" + std::to_string(timestamp_ms);

        char signature[CC_SHA256_DIGEST_LENGTH * 2 + 1];
        hmac_sha256(secret_key_, payload, signature);

        payload += "&signature=" + std::string(signature);
        return payload;
    }

    void handle_execution_report(uint64_t cl_ord_id, double executed_price, double executed_qty) {
        for (auto& ord : active_orders_) {
            if (ord.cl_ord_id == cl_ord_id) {
                ord.avg_fill_price = executed_price;
                ord.filled_qty += executed_qty;
                if (ord.filled_qty >= ord.quantity) {
                    ord.status = OrderStatus::FILLED;
                } else {
                    ord.status = OrderStatus::PARTIALLY_FILLED;
                }
                break;
            }
        }
    }

    [[nodiscard]] size_t get_total_orders() const noexcept {
        return active_orders_.size();
    }
};
