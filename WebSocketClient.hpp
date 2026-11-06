#pragma once

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

class WebSocketClient {
private:
    std::string host_;
    std::string port_;
    std::string path_;
    int sock_fd_{-1};
    SSL_CTX* ssl_ctx_{nullptr};
    SSL* ssl_{nullptr};

    void init_ssl() {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        const SSL_METHOD* method = TLS_client_method();
        ssl_ctx_ = SSL_CTX_new(method);
        if (!ssl_ctx_) {
            throw std::runtime_error("Error inicializando contexto SSL");
        }
    }

    void connect_socket() {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host_.c_str(), port_.c_str(), &hints, &res) != 0) {
            throw std::runtime_error("Fallo al resolver host DNS: " + host_);
        }

        sock_fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock_fd_ < 0) {
            freeaddrinfo(res);
            throw std::runtime_error("Fallo al crear socket TCP");
        }

        // Deshabilitar algoritmo de Nagle (TCP_NODELAY para latencia mínima)
        int flag = 1;
        setsockopt(sock_fd_, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

        if (connect(sock_fd_, res->ai_addr, res->ai_addrlen) < 0) {
            close(sock_fd_);
            freeaddrinfo(res);
            throw std::runtime_error("No se pudo conectar al endpoint " + host_ + ":" + port_);
        }
        freeaddrinfo(res);

        ssl_ = SSL_new(ssl_ctx_);
        SSL_set_fd(ssl_, sock_fd_);
        SSL_set_tlsext_host_name(ssl_, host_.c_str());

        if (SSL_connect(ssl_) <= 0) {
            throw std::runtime_error("Fallo el handshake TLS/SSL con " + host_);
        }
    }

    void perform_handshake() {
        std::string request = 
            "GET " + path_ + " HTTP/1.1\r\n"
            "Host: " + host_ + ":" + port_ + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";

        if (SSL_write(ssl_, request.c_str(), static_cast<int>(request.length())) <= 0) {
            throw std::runtime_error("Error enviando el HTTP Upgrade Request");
        }

        char buffer[1024];
        int bytes = SSL_read(ssl_, buffer, sizeof(buffer) - 1);
        if (bytes <= 0) {
            throw std::runtime_error("Error recibiendo la respuesta del handshake");
        }
        buffer[bytes] = '\0';

        if (std::string_view(buffer).find("101 Switching Protocols") == std::string_view::npos) {
            throw std::runtime_error("El servidor no acepto la transicion a WebSocket:\n" + std::string(buffer));
        }
    }

public:
    WebSocketClient(std::string host, std::string port, std::string path)
        : host_(std::move(host)), port_(std::move(port)), path_(std::move(path)) {
        init_ssl();
        connect_socket();
        perform_handshake();
    }

    ~WebSocketClient() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
        if (sock_fd_ >= 0) close(sock_fd_);
        if (ssl_ctx_) SSL_CTX_free(ssl_ctx_);
    }

    // Lee un mensaje completo des-enmarcando el frame RFC 6455
    bool read_message(std::string& out_payload) {
        unsigned char header[2];
        int n = SSL_read(ssl_, header, 2);
        if (n <= 0) return false;

        uint8_t opcode = header[0] & 0x0F;
        if (opcode == 0x8) return false; // Connection Close

        bool masked = (header[1] & 0x80) != 0;
        uint64_t payload_len = header[1] & 0x7F;

        if (payload_len == 126) {
            unsigned char ext[2];
            SSL_read(ssl_, ext, 2);
            payload_len = (ext[0] << 8) | ext[1];
        } else if (payload_len == 127) {
            unsigned char ext[8];
            SSL_read(ssl_, ext, 8);
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | ext[i];
            }
        }

        uint8_t mask[4] = {0};
        if (masked) {
            SSL_read(ssl_, mask, 4);
        }

        out_payload.resize(payload_len);
        size_t total_read = 0;
        while (total_read < payload_len) {
            int r = SSL_read(ssl_, &out_payload[total_read], static_cast<int>(payload_len - total_read));
            if (r <= 0) break;
            total_read += r;
        }

        if (masked) {
            for (size_t i = 0; i < payload_len; ++i) {
                out_payload[i] ^= mask[i % 4];
            }
        }

        return total_read == payload_len;
    }
};
