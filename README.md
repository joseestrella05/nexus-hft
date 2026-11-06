# nexus-hft

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=c%2B%2B&logoColor=white)
![macOS ARM64](https://img.shields.io/badge/macOS-ARM64-000000?logo=apple&logoColor=white)
![Clang -O3](https://img.shields.io/badge/Clang-O3-262D3A?logo=llvm&logoColor=white)
![OpenSSL](https://img.shields.io/badge/OpenSSL-TLS-721412?logo=openssl&logoColor=white)
![License MIT](https://img.shields.io/badge/License-MIT-green.svg)

**nexus-hft** es un pipeline de trading cuantitativo concurrente de ultra-baja latencia escrito en C++20 y optimizado para Apple Silicon (ARM64). Un hilo de red ingiere trades en vivo de Binance por TLS WebSocket y los inyecta en una cola SPSC lock-free. Un segundo hilo de estrategia consume esos eventos, actualiza un libro de órdenes L2 plano residente en caché L1, calcula la señal alfa (Order Book Imbalance y Momentum con Trailing Stop) y genera órdenes firmadas con HMAC-SHA256 acelerado por hardware. Ninguna ruta crítica usa locks del sistema operativo, llamadas al kernel ni asignaciones dinámicas en heap.

---

## Tabla de contenidos

1. [Benchmarks y latencias de hardware](#benchmarks-y-latencias-de-hardware)
2. [Arquitectura del pipeline](#arquitectura-del-pipeline)
3. [Ingeniería por componente](#ingeniería-por-componente)
   - [SPSC Queue y mitigación de False Sharing](#spsc-queue-y-mitigación-de-false-sharing)
   - [Libro L2 plano en caché L1](#libro-l2-plano-en-caché-l1)
   - [Criptofirmas de baja latencia con CommonCrypto](#criptofirmas-de-baja-latencia-con-commoncrypto)
   - [Ingesta TLS / WebSocket](#ingesta-tls--websocket)
   - [Pipeline concurrente en vivo](#pipeline-concurrente-en-vivo)
4. [Estructura del repositorio](#estructura-del-repositorio)
5. [Compilación y ejecución](#compilación-y-ejecución)
6. [Licencia](#licencia)

---

## Benchmarks y latencias de hardware

Todas las métricas se midieron en Apple Silicon ARM64, compilando con Apple Clang (`-std=c++20 -O3 -Wall -Wextra -pthread`).

| Componente | Archivo | Carga de prueba | Tiempo total | Throughput | Latencia media |
|---|---|---|---|---|---|
| SPSC Lock-Free Queue | `SPSCQueue.hpp` | 20,000,000 mensajes entre 2 hilos nativos | 126.74 ms | 157.80 M msg/s | **6.34 ns** / mensaje |
| Flat L2 Limit Order Book | `OrderBook.hpp` | 1,000,000 actualizaciones de profundidad | 20.55 ms | 97.33 M updates/s | **10.27 ns** / update |
| OMS + firma HMAC-SHA256 | `OMS.hpp` / `Order.hpp` | 100,000 órdenes creadas y firmadas | 104.54 ms | 956.58 K órdenes/s | **1.05 µs** / orden |
| Motor concurrente integrado | `concurrent_engine.cpp` | 1.11 M ticks históricos | 191.01 ms | 5.84 M ticks/s | n/a |

---

## Arquitectura del pipeline

```
 ┌────────────────────────────────────────────────────────────────────────────┐
 │                      stream.binance.com:9443 (TLS 1.2/1.3)                 │
 │                          /ws/btcusdt@trade (JSON)                          │
 └──────────────────────────────────────┬─────────────────────────────────────┘
                                        │  TCP (TCP_NODELAY) + TLS + WS RFC 6455
                                        ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │  HILO 1: RED (Productor)                                                   │
 │  ─ WebSocketClient.hpp                                                     │
 │  ─ Socket POSIX + OpenSSL (SSL_read)                                       │
 │  ─ Des-enmarcado de frames WebSocket                                       │
 │  ─ Parseo numérico zero-copy sobre JSON crudo                              │
 └──────────────────────────────────────┬─────────────────────────────────────┘
                                        │  push()  [memory_order_release]
                                        ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │  SPSCQueue<Tick, 65536>   (ring buffer lock-free)                          │
 │  head_ ──[alignas(128)]──   tail_ ──[alignas(128)]──   buffer[65536]       │
 └──────────────────────────────────────┬─────────────────────────────────────┘
                                        │  pop()   [memory_order_acquire]
                                        ▼
 ┌────────────────────────────────────────────────────────────────────────────┐
 │  HILO 2: ESTRATEGIA (Consumidor)                                           │
 │                                                                            │
 │   ┌───────────────┐   ┌───────────────────┐   ┌─────────────────────────┐  │
 │   │ OrderBook.hpp │──▶│ Señales alfa      │──▶│ OMS.hpp / Order.hpp     │  │
 │   │ L2 plano      │   │ ─ OBI             │   │ ─ Máquina de estados    │  │
 │   │ 50 niveles    │   │ ─ Momentum +      │   │ ─ cl_ord_id determinista│  │
 │   │ < 1.6 KB (L1) │   │   Trailing Stop   │   │ ─ HMAC-SHA256 (CCHmac)  │  │
 │   │ VWAP impacto  │   │                   │   │ ─ Hex en stack          │  │
 │   └───────────────┘   └───────────────────┘   └─────────────────────────┘  │
 │                                  │                                         │
 │                           Portfolio.hpp (PnL / posición)                   │
 └────────────────────────────────────────────────────────────────────────────┘

   Memoria: Arena.hpp (asignador bump, sin heap en ruta crítica)
   Replay histórico: FastFileReader.hpp ─▶ concurrent_engine (1.11 M ticks)
```

---

## Ingeniería por componente

### SPSC Queue y mitigación de False Sharing

`SPSCQueue.hpp` implementa un ring buffer Single-Producer Single-Consumer sin locks del sistema operativo y sin llamadas al kernel.

**Decisiones de diseño**

- **Capacidad potencia de 2 (65,536 slots).** El índice se calcula con máscara de bits, `idx & (Capacity - 1)`. Esto evita la división modular y mantiene el acceso en O(1).
- **Alineación a 128 bytes.** Los contadores `head_` (consumidor) y `tail_` (productor) se declaran con `alignas(128)`. La línea de caché efectiva en Apple Silicon es de 128 bytes, así que cada contador vive en su propia línea. Un núcleo que escribe `tail_` no invalida la línea de `head_` en el otro núcleo, lo que elimina el False Sharing.
- **Semántica Acquire/Release.** `push` publica el elemento con `std::memory_order_release` y `pop` lo observa con `std::memory_order_acquire`. Esto da el orden mínimo necesario para la corrección y evita las barreras completas de `seq_cst`. En ARM64 se traduce a instrucciones `stlr` / `ldar` en lugar de barreras `dmb ish` globales.
- **Lecturas del índice propio en relaxed.** Cada lado lee su propio índice con `memory_order_relaxed`, porque solo él lo modifica. Solo el índice del otro lado se lee con `acquire`.

```cpp
template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity debe ser potencia de 2");

    alignas(128) std::atomic<std::size_t> head_{0};   // consumidor
    alignas(128) std::atomic<std::size_t> tail_{0};   // productor
    alignas(128) T buffer_[Capacity];

public:
    bool push(const T& v) noexcept {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) == Capacity) return false;
        buffer_[t & (Capacity - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& out) noexcept {
        const auto h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false;
        out = buffer_[h & (Capacity - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
};
```

**Resultado empírico:** 20,000,000 de mensajes transferidos entre dos hilos nativos en 126.74 ms, es decir **157.80 M msg/s** con una latencia media de **6.34 ns** por mensaje.

### Libro L2 plano en caché L1

`OrderBook.hpp` mantiene el libro de órdenes L2 en arrays contiguos planos, sin árboles binarios (`std::map`) ni listas enlazadas.

**Decisiones de diseño**

- **Bids y Asks en arrays contiguos de hasta 50 niveles.** Cada nivel guarda precio y cantidad. El acceso es secuencial y predecible, lo que favorece el prefetcher de hardware.
- **Huella total menor a 1.6 KB.** El libro entero cabe con holgura en la caché L1 de datos (128 KB en Apple Silicon). Tras el calentamiento, la actualización de profundidad no sale de L1.
- **Cero fragmentación y cero asignaciones.** Toda la memoria es estática o de pila. No hay `new`, `malloc` ni rebalanceo de nodos.
- **Order Book Imbalance (OBI).** La señal alfa en tiempo real se calcula sobre los niveles superiores del libro:

  ```
  OBI = (Σ bid_qty[0..k) − Σ ask_qty[0..k)) / (Σ bid_qty[0..k) + Σ ask_qty[0..k))   ∈ [−1, +1]
  ```

  Un valor positivo indica presión compradora y uno negativo, presión vendedora.
- **Impacto de mercado con VWAP.** Para simular un barrido de liquidez realista, el algoritmo recorre los niveles opuestos acumulando cantidad hasta cubrir el tamaño de la orden, y devuelve el precio medio ponderado por volumen. La diferencia con el mejor precio da el slippage esperado.

**Resultado empírico:** 1,000,000 de actualizaciones de profundidad en 20.55 ms, es decir **97.33 M updates/s** con una latencia media de **10.27 ns** por update.

### Criptofirmas de baja latencia con CommonCrypto

`OMS.hpp` y `Order.hpp` implementan el ciclo de vida de las órdenes y su firma criptográfica.

**Máquina de estados**

```
NEW ──▶ PENDING_SUBMIT ──▶ PARTIALLY_FILLED ──▶ FILLED
                │                  │
                └──────────────────┴──────────▶ CANCELLED
```

Los estados son `NEW`, `PENDING_SUBMIT`, `PARTIALLY_FILLED`, `FILLED` y `CANCELLED`.

**Decisiones de diseño**

- **`cl_ord_id` determinista.** El identificador se deriva de un contador monotónico, lo que garantiza reproducibilidad en replay y unicidad sin consultar el reloj ni un generador aleatorio.
- **Serialización del query string.** La orden se serializa en un buffer fijo en stack con el formato requerido por el exchange (`symbol`, `side`, `type`, `quantity`, `timestamp`, `newClientOrderId`).
- **HMAC-SHA256 por la API nativa de macOS.** Se usa `CCHmac(kCCHmacAlgSHA256, ...)` de `<CommonCrypto/CommonHMAC.h>`, que aprovecha las extensiones criptográficas de ARMv8 (SHA-2) y evita el overhead de contextos dinámicos de OpenSSL.
- **Hex en stack.** El digest binario de 32 bytes se convierte a 64 caracteres hexadecimales con una tabla de búsqueda sobre un buffer en pila, sin `std::string`, `sprintf` ni heap.

**Resultado empírico:** 100,000 órdenes creadas y firmadas en 104.54 ms, es decir **956.58 K órdenes/s** con **1.05 µs** por orden.

### Ingesta TLS / WebSocket

`WebSocketClient.hpp` implementa el cliente de red completo sin dependencias externas de alto nivel.

1. **Conexión TCP con sockets POSIX** (`socket`, `connect`) y la opción `TCP_NODELAY` activa, para desactivar el algoritmo de Nagle y enviar cada segmento de inmediato.
2. **Negociación TLS con OpenSSL / LibreSSL** (`SSL_connect`) contra `stream.binance.com:9443`.
3. **Handshake WebSocket (RFC 6455)** con `Upgrade: websocket`, `Sec-WebSocket-Key` y validación de `Sec-WebSocket-Accept`.
4. **Des-enmarcado de frames.** Se decodifican opcode, bit de máscara y longitud (7, 16 y 64 bits), y se gestionan los frames de control (ping/pong/close).
5. **Parseo numérico en cero copias.** Los campos `p` (precio), `q` (cantidad) y `T` (timestamp) se localizan directamente sobre el buffer de recepción y se convierten a valores numéricos sin crear `std::string` ni copias intermedias.

### Pipeline concurrente en vivo

- **Hilo 1 (Red):** se conecta a `stream.binance.com:9443/ws/btcusdt@trade` y empuja cada tick parseado a la `SPSCQueue`.
- **Hilo 2 (Estrategia):** extrae ticks de la cola y actualiza el libro L2. Calcula la señal de Momentum con Trailing Stop y, al dispararse, genera órdenes firmadas con HMAC-SHA256 en tiempo real.
- **Motor concurrente sobre datos históricos (`concurrent_engine`):** procesó 1.11 M de ticks en 191.01 ms, es decir **5.84 M ticks/s** de punta a punta.

---

## Estructura del repositorio

```
nexus-hft/
├── SPSCQueue.hpp          # Ring buffer SPSC lock-free (alignas(128), acquire/release)
├── OrderBook.hpp          # Libro L2 plano, OBI y VWAP de impacto
├── Order.hpp              # Definición de orden, estados y cl_ord_id
├── OMS.hpp                # Gestión de órdenes y firma HMAC-SHA256 (CommonCrypto)
├── WebSocketClient.hpp    # Cliente TCP/TLS/WebSocket (RFC 6455)
├── MarketData.hpp         # Tipos de datos de mercado (ticks)
├── Arena.hpp              # Asignador de memoria tipo arena
├── FastFileReader.hpp     # Lectura rápida de ticks históricos
├── Portfolio.hpp          # Posición y PnL
├── bench_spsc.cpp         # Benchmark de la cola SPSC (binario: spsc_bench)
├── test_orderbook.cpp     # Benchmark y pruebas del libro L2
├── test_oms.cpp           # Benchmark y pruebas del OMS
├── concurrent_engine.cpp  # Motor concurrente sobre datos históricos
├── live_trader.cpp        # Pipeline en vivo (Binance TLS WebSocket)
├── LICENSE
└── README.md
```

---

## Compilación y ejecución

### Requisitos

- macOS sobre Apple Silicon (ARM64)
- Apple Clang con soporte C++20 (Xcode Command Line Tools)
- OpenSSL vía Homebrew:

```bash
brew install openssl
```

### Flags

Todos los binarios se compilan con `-std=c++20 -O3 -Wall -Wextra -pthread`. Los que usan red añaden:

```
-I$(brew --prefix openssl)/include -L$(brew --prefix openssl)/lib -lssl -lcrypto
```

### `spsc_bench`: benchmark de la cola SPSC

```bash
clang++ -std=c++20 -O3 -Wall -Wextra -pthread bench_spsc.cpp -o spsc_bench
./spsc_bench
```

Transfiere 20,000,000 de mensajes entre dos hilos y reporta tiempo total, throughput y latencia media por mensaje.

### `test_orderbook`: libro L2 plano

```bash
clang++ -std=c++20 -O3 -Wall -Wextra -pthread test_orderbook.cpp -o test_orderbook
./test_orderbook
```

Aplica 1,000,000 de actualizaciones de profundidad y reporta throughput y latencia, junto con la verificación de OBI y VWAP.

### `test_oms`: OMS y firma HMAC-SHA256

```bash
clang++ -std=c++20 -O3 -Wall -Wextra -pthread test_oms.cpp -o test_oms
./test_oms
```

Crea y firma 100,000 órdenes con `CCHmac` y reporta el tiempo medio por orden. La firma usa CommonCrypto del sistema.

### `concurrent_engine`: motor concurrente sobre datos históricos

```bash
clang++ -std=c++20 -O3 -Wall -Wextra -pthread concurrent_engine.cpp -o concurrent_engine
./concurrent_engine
```

Reproduce 1.11 M de ticks históricos a través de la `SPSCQueue` entre un hilo productor y un hilo de estrategia, y reporta el tiempo total y los ticks por segundo.

### `live_trader`: pipeline en vivo con TLS WebSocket

```bash
clang++ -std=c++20 -O3 -Wall -Wextra -pthread \
  -I$(brew --prefix openssl)/include \
  -L$(brew --prefix openssl)/lib -lssl -lcrypto \
  live_trader.cpp -o live_trader
./live_trader
```

Se conecta a `stream.binance.com:9443/ws/btcusdt@trade`, ingesta trades en tiempo real, alimenta el libro L2, evalúa la señal de Momentum con Trailing Stop y genera órdenes firmadas con HMAC-SHA256. Requiere conectividad de red saliente al puerto 9443.

---

## Licencia

Este proyecto se distribuye bajo la licencia MIT. Ver [LICENSE](LICENSE).
