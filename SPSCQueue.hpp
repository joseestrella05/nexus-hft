#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <array>
#include <optional>

#if defined(__APPLE__) && defined(__aarch64__)
// Línea de caché estándar en Apple Silicon (ARM64)
constexpr size_t HARDWARE_DESTRUCTIVE_SIZE = 128;
#else
constexpr size_t HARDWARE_DESTRUCTIVE_SIZE = 64;
#endif

template <typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity debe ser una potencia de 2.");
    static_assert(std::is_trivially_copyable_v<T>, "T debe ser trivialmente copiable para evitar pausas en el ring buffer.");

private:
    static constexpr size_t MASK = Capacity - 1;

    // Buffer plano preasignado contiguo en memoria
    alignas(HARDWARE_DESTRUCTIVE_SIZE) std::array<T, Capacity> buffer_;

    // Puntero de escritura (modificado solo por el Productor)
    alignas(HARDWARE_DESTRUCTIVE_SIZE) std::atomic<size_t> tail_{0};

    // Puntero de lectura (modificado solo por el Consumidor)
    alignas(HARDWARE_DESTRUCTIVE_SIZE) std::atomic<size_t> head_{0};

public:
    SPSCQueue() = default;
    ~SPSCQueue() = default;

    // Prohibir copia y movimiento
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // Ejecutado ÚNICAMENTE por el hilo Productor
    bool push(const T& item) noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t current_head = head_.load(std::memory_order_acquire);

        // Comprobar si la cola está llena
        if ((current_tail - current_head) >= Capacity) {
            return false;
        }

        buffer_[current_tail & MASK] = item;
        // Notificar al consumidor que el dato está disponible en memoria
        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    // Ejecutado ÚNICAMENTE por el hilo Consumidor
    bool pop(T& item) noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t current_tail = tail_.load(std::memory_order_acquire);

        // Comprobar si la cola está vacía
        if (current_head == current_tail) {
            return false;
        }

        item = buffer_[current_head & MASK];
        // Notificar al productor que el espacio fue liberado
        head_.store(current_head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t size() const noexcept {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return (tail >= head) ? (tail - head) : (Capacity - (head - tail));
    }
};
