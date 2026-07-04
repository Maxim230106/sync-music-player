#pragma once

#include "protocol.hpp"

#ifdef _WIN32
    #define NOMINMAX
    #include <winsock2.h> // WinSock: сокеты, send, recv, bind, listen, accept
    #include <ws2tcpip.h> // Функции для IP-адресов и преобразований, (inet_pton, InetNtopA, htonl, ntohl)
    #include <iphlpapi.h> // Получение списка сетевых интерфейсов и локальных IP через GetAdaptersAddresses()
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
#else
    #error "This starter uses WinSock on Windows."
#endif

#include <optional> // Вернуть std::nullopt, если соединение закрыто
#include <mutex> // Защита списка клиентов от одновременного доступа
#include <thread> // Работа в отдельных потоках: приём клиентов и приём сообщений
#include <atomic> // Классы и функции для выполнения атомарных операций. Флаг running, который безопасно читается из разных потоков
#include <vector>
#include <string>

// RAII-обёртка для WinSock
// При создании вызывает WSAStartup, при уничтожении — WSACleanup
class SocketInit {
public:
    SocketInit() {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~SocketInit() {
        WSACleanup();
    }
};

// Отправка всех байт
// send() может вернуть меньше, чем мы передали, поэтому нужен цикл
inline bool send_all(SOCKET s, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    size_t sent_total = 0;

    while (sent_total < size) {
        int sent = ::send(s, p + sent_total, static_cast<int>(size - sent_total), 0);
        if (sent <= 0) return false;
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

// Приём всех байт
// recv() тоже может вернуть часть данных, поэтому читаем до конца
inline bool recv_all(SOCKET s, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    size_t recvd_total = 0;

    while (recvd_total < size) {
        int recvd = ::recv(s, p + recvd_total, static_cast<int>(size - recvd_total), 0);
        if (recvd <= 0) return false;
        recvd_total += static_cast<size_t>(recvd);
    }
    return true;
}

// Отправка сообщения по сети
// Заголовок переводим в network byte order
inline bool send_message(SOCKET s, const Message& msg) {
    MessageHeader h;
    h.type = static_cast<uint16_t>(msg.type);
    h.payloadSize = static_cast<uint32_t>(msg.payload.size());
    h.sequence = msg.sequence;

    MessageHeader wire{};
    wire.magic = htonl(h.magic);
    wire.version = htons(h.version);
    wire.type = htons(h.type);
    wire.payloadSize = htonl(h.payloadSize);
    wire.sequence = htonl(h.sequence);

    if (!send_all(s, &wire, sizeof(wire))) return false;
    if (!msg.payload.empty()) {
        if (!send_all(s, msg.payload.data(), msg.payload.size())) return false;
    }
    return true;
}

// Приём сообщения из сети
// Возвращает std::nullopt, если соединение закрыто
inline std::optional<Message> recv_message(SOCKET s) {
    MessageHeader wire{};
    if (!recv_all(s, &wire, sizeof(wire))) return std::nullopt;

    MessageHeader h{};
    h.magic = ntohl(wire.magic);
    h.version = ntohs(wire.version);
    h.type = ntohs(wire.type);
    h.payloadSize = ntohl(wire.payloadSize);
    h.sequence = ntohl(wire.sequence);

    // Проверка корректности заголовка.
    if (h.magic != 0x53504C59 || h.version != 1) {
        throw std::runtime_error("Bad header");
    }

    Message m;
    m.type = static_cast<MessageType>(h.type);
    m.sequence = h.sequence;
    m.payload.resize(h.payloadSize);

    if (h.payloadSize > 0) {
        if (!recv_all(s, m.payload.data(), m.payload.size())) return std::nullopt;
    }

    return m;
}

// Получить список локальных IPv4-адресов
// Это удобно печатать на host, чтобы понять, по какому адресу подключаться
inline std::vector<std::string> get_local_ipv4_addresses() {
    std::vector<std::string> result;

    // Loopback всегда добавляем вручную
    result.push_back("127.0.0.1");

    ULONG buf_len = 15000;
    std::vector<unsigned char> buffer(buf_len);

    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buf_len);

    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buf_len);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buf_len);
    }

    if (ret != NO_ERROR) {
        return result;
    }

    for (auto* a = adapters; a != nullptr; a = a->Next) {
        // Нас интересуют только активные интерфейсы
        if (a->OperStatus != IfOperStatusUp) {
            continue;
        }

        for (auto* ua = a->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
            auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            if (!sa || sa->sin_family != AF_INET) {
                continue;
            }

            char buf[INET_ADDRSTRLEN]{};
            if (InetNtopA(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
                std::string ip = buf;
                if (ip != "127.0.0.1") {
                    result.push_back(ip);
                }
            }
        }
    }

    return result;
}

// TCP-сервер
// Слушает порт и принимает клиентов
class TcpServer {
public:
    explicit TcpServer(uint16_t port) : port_(port) {}

    void start() {
        listen_sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock_ == INVALID_SOCKET) {
            throw std::runtime_error("socket() failed");
        }

        // Можно быстро перезапускать сервер на том же порте
        BOOL reuse = TRUE;
        setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0 -- слушать все интерфейсы
        addr.sin_port = htons(port_);

        if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            throw std::runtime_error("bind() failed");
        }

        if (::listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR) {
            throw std::runtime_error("listen() failed");
        }
    }

    SOCKET accept_client() {
        sockaddr_in caddr{};
        int clen = sizeof(caddr);
        SOCKET c = ::accept(listen_sock_, reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (c == INVALID_SOCKET) {
            throw std::runtime_error("accept() failed");
        }
        return c;
    }

    void stop() {
        if (listen_sock_ != INVALID_SOCKET) {
            closesocket(listen_sock_);
            listen_sock_ = INVALID_SOCKET;
        }
    }

    ~TcpServer() { stop(); }

private:
    uint16_t port_;
    SOCKET listen_sock_ = INVALID_SOCKET;
};

// TCP-клиент
// Подключается к host по IP и порту
class TcpClient {
public:
    void connect_to(const std::string& host, uint16_t port) {
        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) {
            throw std::runtime_error("socket() failed");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            throw std::runtime_error("inet_pton() failed");
        }

        if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            throw std::runtime_error("connect() failed");
        }
    }

    SOCKET socket_handle() const { return sock_; }

    void close() {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    ~TcpClient() { close(); }

private:
    SOCKET sock_ = INVALID_SOCKET;
};