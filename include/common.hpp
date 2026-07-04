#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
#include <optional> // Обработка необязательных значений. Чтобы recv_message() мог вернуть либо сообщение, либо std::nullopt, если соединение закрылось
#include <cstring> // std::memcpy() для чтения бинарных чисел из буфера
#include <chrono> // Текущее время now_ms()

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