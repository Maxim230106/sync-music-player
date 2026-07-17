#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
#include <optional> // Обработка необязательных значений. Чтобы recv_message() мог вернуть либо сообщение, либо std::nullopt, если соединение закрылось
#include <cstring> // std::memcpy() для чтения бинарных чисел из буфера
#include <chrono> // Текущее время now_ms()
#include <atomic>

// Возвращает текущее время в миллисекундах
// Используется для ping/pong и синхронизации воспроизведения
inline uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

// Простой логгер для консольного вывода
inline void log(const std::string& s) {
    std::cout << s << std::endl;
}

inline std::atomic_bool& protocol_logs_flag() {
    static std::atomic_bool enabled = false;
    return enabled;
}

inline void set_protocol_logs_enabled(bool enabled) {
    protocol_logs_flag().store(enabled);
}

inline bool protocol_logs_enabled() {
    return protocol_logs_flag().load();
}
