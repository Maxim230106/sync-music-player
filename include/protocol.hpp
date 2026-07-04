#pragma once

#include "common.hpp"

#include <string>
#include <vector>
#include <span> // Удобный вид на уже существующий массив байт без копирования
#include <stdexcept> // Исключения
#include <utility> // Типы данных std::pair, std::tuple

// Все типы сообщений протокола
enum class MessageType : uint16_t {
    Hello    = 1,  // client -> host
    Welcome  = 2,  // host -> client
    Text     = 3,  // тестовый текст
    Track    = 4,  // выбор текущего трека
    Play     = 5,  // запуск воспроизведения
    Pause    = 6,  // пауза
    Stop     = 7,  // полная остановка
    Volume   = 8,  // громкость
    Ping     = 9,  // запрос на проверку задержки
    Pong     = 10, // ответ на Ping
    Shutdown = 11, // host завершает работу
    StateSync = 12  // полная синхронизация состояния
};

// Заголовок каждого сообщения
// Сначала он идёт по сети, затем -- payload
#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic = 0x53504C59;   // 'SPLY' -- сигнатура нашего протокола
    uint16_t version = 1;          // версия протокола
    uint16_t type = 0;             // MessageType
    uint32_t payloadSize = 0;      // размер payload в байтах
    uint32_t sequence = 0;         // порядковый номер сообщения
};
#pragma pack(pop)

// Готовое сообщение, которое отправляется по сети
struct Message {
    MessageType type{};
    uint32_t sequence{};
    std::vector<uint8_t> payload;
};

// Утилита для записи бинарных данных в vector<uint8_t>
class BinaryWriter {
public:
    void write_u32(uint32_t v) { append(&v, sizeof(v)); }
    void write_u64(uint64_t v) { append(&v, sizeof(v)); }
    void write_u16(uint16_t v) { append(&v, sizeof(v)); }

    // Строка пишется в формате: длина + байты
    void write_string(const std::string& s) {
        write_u32(static_cast<uint32_t>(s.size()));
        if (!s.empty()) {
            append(s.data(), s.size());
        }
    }

    std::vector<uint8_t> take() { return std::move(buf_); }

private:
    std::vector<uint8_t> buf_;

    void append(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + size);
    }
};

// Утилита для чтения бинарных данных из payload
class BinaryReader {
public:
    explicit BinaryReader(std::span<const uint8_t> data) : data_(data) {}

    uint32_t read_u32() { return read_pod<uint32_t>(); }
    uint64_t read_u64() { return read_pod<uint64_t>(); }
    uint16_t read_u16() { return read_pod<uint16_t>(); }

    std::string read_string() {
        uint32_t len = read_u32();
        if (pos_ + len > data_.size()) {
            throw std::runtime_error("Bad string length");
        }
        std::string s(reinterpret_cast<const char*>(data_.data() + pos_), len);
        pos_ += len;
        return s;
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_ = 0;

    template <typename T>
    T read_pod() {
        if (pos_ + sizeof(T) > data_.size()) {
            throw std::runtime_error("Unexpected end of payload");
        }
        T v{};
        std::memcpy(&v, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
};

// ---------- builders ----------

// Приветствие клиента: ник + локальное время клиента
inline std::vector<uint8_t> build_hello_payload(const std::string& nickname) {
    BinaryWriter w;
    w.write_string(nickname);
    w.write_u64(now_ms());
    return w.take();
}

// Приветствие от сервера: id клиента + имя сервера
inline std::vector<uint8_t> build_welcome_payload(uint32_t client_id, const std::string& server_name) {
    BinaryWriter w;
    w.write_u32(client_id);
    w.write_string(server_name);
    return w.take();
}

// Просто текстовая заглушка
inline std::vector<uint8_t> build_text_payload(const std::string& text) {
    BinaryWriter w;
    w.write_string(text);
    return w.take();
}

// Название трека
inline std::vector<uint8_t> build_track_payload(const std::string& track_name) {
    BinaryWriter w;
    w.write_string(track_name);
    return w.take();
}

// Состояние плеера:
// track + position + server_time + play_at
// Для Pause/Stop часть полей может быть не очень нужна, но формат пока общий
inline std::vector<uint8_t> build_play_payload(
    const std::string& track_name,
    uint64_t position_ms,
    uint64_t server_time_ms,
    uint64_t play_at_ms,
    uint32_t volume_percent
) {
    BinaryWriter w;
    w.write_string(track_name);
    w.write_u64(position_ms);
    w.write_u64(server_time_ms);
    w.write_u64(play_at_ms);
    w.write_u32(volume_percent);
    return w.take();
}

// Для Pause: track + position + server_time
inline std::vector<uint8_t> build_pause_payload(
    const std::string& track_name,
    uint64_t position_ms,
    uint64_t server_time_ms
) {
    BinaryWriter w;
    w.write_string(track_name);
    w.write_u64(position_ms);
    w.write_u64(server_time_ms);
    return w.take();
}

// Для Stop: track + server_time
inline std::vector<uint8_t> build_stop_payload(
    const std::string& track_name,
    uint64_t server_time_ms
) {
    BinaryWriter w;
    w.write_string(track_name);
    w.write_u64(server_time_ms);
    return w.take();
}

// Громкость в процентах
inline std::vector<uint8_t> build_volume_payload(uint32_t volume_percent) {
    BinaryWriter w;
    w.write_u32(volume_percent);
    return w.take();
}

// Ping/Pong — timestamp для вычисления задержки
inline std::vector<uint8_t> build_ping_payload(uint64_t t) {
    BinaryWriter w;
    w.write_u64(t);
    return w.take();
}

// Полная синхронизация состояния для нового клиента
inline std::vector<uint8_t> build_state_sync_payload(
    const std::string& track_name,
    const std::string& state,
    uint64_t position_ms,
    uint64_t server_time_ms,
    uint64_t play_at_ms,
    uint32_t volume_percent
) {
    BinaryWriter w;
    w.write_string(track_name);
    w.write_string(state);
    w.write_u64(position_ms);
    w.write_u64(server_time_ms);
    w.write_u64(play_at_ms);
    w.write_u32(volume_percent);
    return w.take();
}

// ---------- parsers ----------

inline std::string parse_string_payload(const std::vector<uint8_t>& payload) {
    BinaryReader r(payload);
    return r.read_string();
}

inline std::pair<uint32_t, std::string> parse_welcome_payload(const std::vector<uint8_t>& payload) {
    BinaryReader r(payload);
    uint32_t client_id = r.read_u32();
    std::string server_name = r.read_string();
    return {client_id, server_name};
}

inline std::pair<std::string, uint64_t> parse_track_position_payload(const std::vector<uint8_t>& payload) {
    BinaryReader r(payload);
    std::string track = r.read_string();
    uint64_t pos = r.read_u64();
    return {track, pos};
}

inline std::tuple<std::string, uint64_t, uint64_t> parse_pause_payload(const std::vector<uint8_t>& payload) {
    BinaryReader r(payload);
    std::string track = r.read_string();
    uint64_t pos = r.read_u64();
    uint64_t server_time = r.read_u64();
    return {track, pos, server_time};
}

inline std::tuple<std::string, uint64_t, uint64_t, uint64_t, uint32_t> parse_play_payload(const std::vector<uint8_t>& payload) {
    BinaryReader r(payload);
    std::string track = r.read_string();
    uint64_t pos = r.read_u64();
    uint64_t server_time = r.read_u64();
    uint64_t play_at = r.read_u64();
    uint32_t volume = r.read_u32();
    return {track, pos, server_time, play_at, volume};
}

inline std::tuple<std::string, uint64_t> parse_stop_payload(const std::vector<uint8_t>& payload) {
    BinaryReader r(payload);
    std::string track = r.read_string();
    uint64_t server_time = r.read_u64();
    return {track, server_time};
}

inline uint32_t parse_volume_payload(const std::vector<uint8_t>& payload) {
    BinaryReader r(payload);
    return r.read_u32();
}

inline uint64_t parse_ping_payload(const std::vector<uint8_t>& payload) {
    BinaryReader r(payload);
    return r.read_u64();
}

inline std::tuple<std::string, std::string, uint64_t, uint64_t, uint64_t, uint32_t>
parse_state_sync_payload(const std::vector<uint8_t>& payload) {
    BinaryReader r(payload);
    std::string track = r.read_string();
    std::string state = r.read_string();
    uint64_t position_ms = r.read_u64();
    uint64_t server_time_ms = r.read_u64();
    uint64_t play_at_ms = r.read_u64();
    uint32_t volume = r.read_u32();
    return {track, state, position_ms, server_time_ms, play_at_ms, volume};
}