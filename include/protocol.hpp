#pragma once

#include "common.hpp"

#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

inline constexpr uint32_t kProtocolMagic = 0x53504C59;
inline constexpr uint16_t kProtocolVersion = 2;

enum class MessageType : uint16_t {
    Hello = 1,
    Welcome = 2,
    PlaylistSync = 3,
    ClientsSync = 4,
    TrackInfo = 5,
    TrackChunk = 6,
    Ready = 7,
    StateSync = 8,
    Ping = 9,
    Pong = 10,
    Quit = 11,
    Kick = 12,
    Shutdown = 13
};

enum class PlaybackState : uint16_t {
    Stopped = 0,
    Playing = 1,
    Paused = 2
};

enum class RepeatMode : uint16_t {
    Queue = 0,
    Track = 1,
    Playlist = 2
};

enum class ClientRole : uint16_t {
    Listener = 0,
    Host = 1
};

enum class ClientStatus : uint16_t {
    Connected = 0,
    Ready = 1,
    Disconnected = 2
};

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic = kProtocolMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = 0;
    uint32_t payloadSize = 0;
    uint32_t sequence = 0;
};
#pragma pack(pop)

struct Message {
    MessageType type{};
    uint32_t sequence{};
    std::vector<uint8_t> payload;
};

struct HelloPayload {
    std::string nickname;
    uint64_t clientTimeMs = 0;
};

struct WelcomePayload {
    uint32_t clientId = 0;
    std::string serverName;
    uint32_t maxClients = 0;
    uint32_t protocolVersion = kProtocolVersion;
};

struct TrackDescriptor {
    uint32_t trackId = 0;
    std::string title;
    std::string fileName;
    uint64_t fileSize = 0;
    uint64_t durationMs = 0;
};

struct PlaylistSyncPayload {
    uint32_t playlistVersion = 0;
    uint32_t currentTrackId = 0;
    std::vector<TrackDescriptor> tracks;
};

struct ClientDescriptor {
    uint32_t clientId = 0;
    std::string nickname;
    ClientRole role = ClientRole::Listener;
    ClientStatus status = ClientStatus::Connected;
};

struct ClientsSyncPayload {
    uint32_t clientsVersion = 0;
    std::vector<ClientDescriptor> clients;
};

struct TrackInfoPayload {
    uint32_t trackId = 0;
    std::string fileName;
    uint64_t fileSize = 0;
    uint64_t durationMs = 0;
};

struct TrackChunkPayload {
    uint32_t trackId = 0;
    uint64_t offset = 0;
    std::vector<uint8_t> chunkData;
};

struct ReadyPayload {
    uint32_t clientId = 0;
    uint32_t trackId = 0;
};

struct StateSyncPayload {
    uint32_t stateVersion = 0;
    uint32_t currentTrackId = 0;
    PlaybackState playbackState = PlaybackState::Stopped;
    uint64_t positionMs = 0;
    uint64_t hostTimeMs = 0;
    uint64_t startAtHostMs = 0;
    uint32_t volumePercent = 50;
    bool autoplayEnabled = true;
    RepeatMode repeatMode = RepeatMode::Queue;
};

struct PingPayload {
    uint64_t hostSendTimeMs = 0;
};

struct PongPayload {
    uint64_t hostSendTimeMs = 0;
    uint64_t clientReplyTimeMs = 0;
};

struct QuitPayload {
    uint32_t clientId = 0;
};

struct KickPayload {
    uint32_t clientId = 0;
    std::string reason;
};

struct ShutdownPayload {
    std::string reason;
};

class BinaryWriter {
public:
    void write_u8(uint8_t value) {
        buf_.push_back(value);
    }

    void write_bool(bool value) {
        write_u8(value ? 1 : 0);
    }

    void write_u16(uint16_t value) {
        buf_.push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
        buf_.push_back(static_cast<uint8_t>(value & 0xFFU));
    }

    void write_u32(uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            buf_.push_back(static_cast<uint8_t>((value >> shift) & 0xFFU));
        }
    }

    void write_u64(uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            buf_.push_back(static_cast<uint8_t>((value >> shift) & 0xFFU));
        }
    }

    void write_string(const std::string& value) {
        write_u32(static_cast<uint32_t>(value.size()));
        if (!value.empty()) {
            append_raw(value.data(), value.size());
        }
    }

    void write_bytes(const std::vector<uint8_t>& value) {
        write_u32(static_cast<uint32_t>(value.size()));
        if (!value.empty()) {
            append_raw(value.data(), value.size());
        }
    }

    void append_raw(const void* data, size_t size) {
        const auto* ptr = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), ptr, ptr + size);
    }

    std::vector<uint8_t> take() {
        return std::move(buf_);
    }

private:
    std::vector<uint8_t> buf_;
};

class BinaryReader {
public:
    explicit BinaryReader(std::span<const uint8_t> data)
        : data_(data) {
    }

    uint8_t read_u8() {
        ensure_available(1);
        return data_[pos_++];
    }

    bool read_bool() {
        return read_u8() != 0;
    }

    uint16_t read_u16() {
        ensure_available(2);
        uint16_t value = 0;
        value |= static_cast<uint16_t>(data_[pos_]) << 8U;
        value |= static_cast<uint16_t>(data_[pos_ + 1]);
        pos_ += 2;
        return value;
    }

    uint32_t read_u32() {
        ensure_available(4);
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value = (value << 8U) | static_cast<uint32_t>(data_[pos_ + i]);
        }
        pos_ += 4;
        return value;
    }

    uint64_t read_u64() {
        ensure_available(8);
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value = (value << 8U) | static_cast<uint64_t>(data_[pos_ + i]);
        }
        pos_ += 8;
        return value;
    }

    std::string read_string() {
        const uint32_t length = read_u32();
        ensure_available(length);
        std::string value(reinterpret_cast<const char*>(data_.data() + pos_), length);
        pos_ += length;
        return value;
    }

    std::vector<uint8_t> read_bytes() {
        const uint32_t length = read_u32();
        ensure_available(length);
        std::vector<uint8_t> value(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                   data_.begin() + static_cast<std::ptrdiff_t>(pos_ + length));
        pos_ += length;
        return value;
    }

private:
    void ensure_available(size_t size) const {
        if (pos_ + size > data_.size()) {
            throw std::runtime_error("Unexpected end of payload");
        }
    }

    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

inline void write_track_descriptor(BinaryWriter& writer, const TrackDescriptor& track) {
    writer.write_u32(track.trackId);
    writer.write_string(track.title);
    writer.write_string(track.fileName);
    writer.write_u64(track.fileSize);
    writer.write_u64(track.durationMs);
}

inline TrackDescriptor read_track_descriptor(BinaryReader& reader) {
    TrackDescriptor track;
    track.trackId = reader.read_u32();
    track.title = reader.read_string();
    track.fileName = reader.read_string();
    track.fileSize = reader.read_u64();
    track.durationMs = reader.read_u64();
    return track;
}

inline void write_client_descriptor(BinaryWriter& writer, const ClientDescriptor& client) {
    writer.write_u32(client.clientId);
    writer.write_string(client.nickname);
    writer.write_u16(static_cast<uint16_t>(client.role));
    writer.write_u16(static_cast<uint16_t>(client.status));
}

inline ClientDescriptor read_client_descriptor(BinaryReader& reader) {
    ClientDescriptor client;
    client.clientId = reader.read_u32();
    client.nickname = reader.read_string();
    client.role = static_cast<ClientRole>(reader.read_u16());
    client.status = static_cast<ClientStatus>(reader.read_u16());
    return client;
}

inline std::vector<uint8_t> build_hello_payload(const HelloPayload& payload) {
    BinaryWriter writer;
    writer.write_string(payload.nickname);
    writer.write_u64(payload.clientTimeMs);
    return writer.take();
}

inline HelloPayload parse_hello_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    HelloPayload result;
    result.nickname = reader.read_string();
    result.clientTimeMs = reader.read_u64();
    return result;
}

inline std::vector<uint8_t> build_welcome_payload(const WelcomePayload& payload) {
    BinaryWriter writer;
    writer.write_u32(payload.clientId);
    writer.write_string(payload.serverName);
    writer.write_u32(payload.maxClients);
    writer.write_u32(payload.protocolVersion);
    return writer.take();
}

inline WelcomePayload parse_welcome_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    WelcomePayload result;
    result.clientId = reader.read_u32();
    result.serverName = reader.read_string();
    result.maxClients = reader.read_u32();
    result.protocolVersion = reader.read_u32();
    return result;
}

inline std::vector<uint8_t> build_playlist_sync_payload(const PlaylistSyncPayload& payload) {
    BinaryWriter writer;
    writer.write_u32(payload.playlistVersion);
    writer.write_u32(payload.currentTrackId);
    writer.write_u32(static_cast<uint32_t>(payload.tracks.size()));
    for (const TrackDescriptor& track : payload.tracks) {
        write_track_descriptor(writer, track);
    }
    return writer.take();
}

inline PlaylistSyncPayload parse_playlist_sync_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    PlaylistSyncPayload result;
    result.playlistVersion = reader.read_u32();
    result.currentTrackId = reader.read_u32();
    const uint32_t trackCount = reader.read_u32();
    result.tracks.reserve(trackCount);
    for (uint32_t i = 0; i < trackCount; ++i) {
        result.tracks.push_back(read_track_descriptor(reader));
    }
    return result;
}

inline std::vector<uint8_t> build_clients_sync_payload(const ClientsSyncPayload& payload) {
    BinaryWriter writer;
    writer.write_u32(payload.clientsVersion);
    writer.write_u32(static_cast<uint32_t>(payload.clients.size()));
    for (const ClientDescriptor& client : payload.clients) {
        write_client_descriptor(writer, client);
    }
    return writer.take();
}

inline ClientsSyncPayload parse_clients_sync_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    ClientsSyncPayload result;
    result.clientsVersion = reader.read_u32();
    const uint32_t clientCount = reader.read_u32();
    result.clients.reserve(clientCount);
    for (uint32_t i = 0; i < clientCount; ++i) {
        result.clients.push_back(read_client_descriptor(reader));
    }
    return result;
}

inline std::vector<uint8_t> build_track_info_payload(const TrackInfoPayload& payload) {
    BinaryWriter writer;
    writer.write_u32(payload.trackId);
    writer.write_string(payload.fileName);
    writer.write_u64(payload.fileSize);
    writer.write_u64(payload.durationMs);
    return writer.take();
}

inline TrackInfoPayload parse_track_info_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    TrackInfoPayload result;
    result.trackId = reader.read_u32();
    result.fileName = reader.read_string();
    result.fileSize = reader.read_u64();
    result.durationMs = reader.read_u64();
    return result;
}

inline std::vector<uint8_t> build_track_chunk_payload(const TrackChunkPayload& payload) {
    BinaryWriter writer;
    writer.write_u32(payload.trackId);
    writer.write_u64(payload.offset);
    writer.write_bytes(payload.chunkData);
    return writer.take();
}

inline TrackChunkPayload parse_track_chunk_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    TrackChunkPayload result;
    result.trackId = reader.read_u32();
    result.offset = reader.read_u64();
    result.chunkData = reader.read_bytes();
    return result;
}

inline std::vector<uint8_t> build_ready_payload(const ReadyPayload& payload) {
    BinaryWriter writer;
    writer.write_u32(payload.clientId);
    writer.write_u32(payload.trackId);
    return writer.take();
}

inline ReadyPayload parse_ready_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    ReadyPayload result;
    result.clientId = reader.read_u32();
    result.trackId = reader.read_u32();
    return result;
}

inline std::vector<uint8_t> build_state_sync_payload(const StateSyncPayload& payload) {
    BinaryWriter writer;
    writer.write_u32(payload.stateVersion);
    writer.write_u32(payload.currentTrackId);
    writer.write_u16(static_cast<uint16_t>(payload.playbackState));
    writer.write_u64(payload.positionMs);
    writer.write_u64(payload.hostTimeMs);
    writer.write_u64(payload.startAtHostMs);
    writer.write_u32(payload.volumePercent);
    writer.write_bool(payload.autoplayEnabled);
    writer.write_u16(static_cast<uint16_t>(payload.repeatMode));
    return writer.take();
}

inline StateSyncPayload parse_state_sync_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    StateSyncPayload result;
    result.stateVersion = reader.read_u32();
    result.currentTrackId = reader.read_u32();
    result.playbackState = static_cast<PlaybackState>(reader.read_u16());
    result.positionMs = reader.read_u64();
    result.hostTimeMs = reader.read_u64();
    result.startAtHostMs = reader.read_u64();
    result.volumePercent = reader.read_u32();
    result.autoplayEnabled = reader.read_bool();
    result.repeatMode = static_cast<RepeatMode>(reader.read_u16());
    return result;
}

inline std::vector<uint8_t> build_ping_payload(const PingPayload& payload) {
    BinaryWriter writer;
    writer.write_u64(payload.hostSendTimeMs);
    return writer.take();
}

inline PingPayload parse_ping_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    PingPayload result;
    result.hostSendTimeMs = reader.read_u64();
    return result;
}

inline std::vector<uint8_t> build_pong_payload(const PongPayload& payload) {
    BinaryWriter writer;
    writer.write_u64(payload.hostSendTimeMs);
    writer.write_u64(payload.clientReplyTimeMs);
    return writer.take();
}

inline PongPayload parse_pong_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    PongPayload result;
    result.hostSendTimeMs = reader.read_u64();
    result.clientReplyTimeMs = reader.read_u64();
    return result;
}

inline std::vector<uint8_t> build_quit_payload(const QuitPayload& payload) {
    BinaryWriter writer;
    writer.write_u32(payload.clientId);
    return writer.take();
}

inline QuitPayload parse_quit_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    QuitPayload result;
    result.clientId = reader.read_u32();
    return result;
}

inline std::vector<uint8_t> build_kick_payload(const KickPayload& payload) {
    BinaryWriter writer;
    writer.write_u32(payload.clientId);
    writer.write_string(payload.reason);
    return writer.take();
}

inline KickPayload parse_kick_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    KickPayload result;
    result.clientId = reader.read_u32();
    result.reason = reader.read_string();
    return result;
}

inline std::vector<uint8_t> build_shutdown_payload(const ShutdownPayload& payload) {
    BinaryWriter writer;
    writer.write_string(payload.reason);
    return writer.take();
}

inline ShutdownPayload parse_shutdown_payload(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    ShutdownPayload result;
    result.reason = reader.read_string();
    return result;
}

inline const char* message_type_name(MessageType type) {
    switch (type) {
        case MessageType::Hello: return "Hello";
        case MessageType::Welcome: return "Welcome";
        case MessageType::PlaylistSync: return "PlaylistSync";
        case MessageType::ClientsSync: return "ClientsSync";
        case MessageType::TrackInfo: return "TrackInfo";
        case MessageType::TrackChunk: return "TrackChunk";
        case MessageType::Ready: return "Ready";
        case MessageType::StateSync: return "StateSync";
        case MessageType::Ping: return "Ping";
        case MessageType::Pong: return "Pong";
        case MessageType::Quit: return "Quit";
        case MessageType::Kick: return "Kick";
        case MessageType::Shutdown: return "Shutdown";
    }
    return "Unknown";
}

inline const char* playback_state_name(PlaybackState state) {
    switch (state) {
        case PlaybackState::Stopped: return "Stopped";
        case PlaybackState::Playing: return "Playing";
        case PlaybackState::Paused: return "Paused";
    }
    return "Unknown";
}

inline const char* repeat_mode_name(RepeatMode mode) {
    switch (mode) {
        case RepeatMode::Queue: return "Queue";
        case RepeatMode::Track: return "Track";
        case RepeatMode::Playlist: return "Playlist";
    }
    return "Unknown";
}

inline std::string summarize_message(const Message& message) {
    try {
        std::ostringstream out;
        out << message_type_name(message.type)
            << "(seq=" << message.sequence
            << ", payload=" << message.payload.size();

        switch (message.type) {
            case MessageType::Hello: {
                const HelloPayload payload = parse_hello_payload(message.payload);
                out << ", nickname=" << payload.nickname;
                break;
            }
            case MessageType::Welcome: {
                const WelcomePayload payload = parse_welcome_payload(message.payload);
                out << ", clientId=" << payload.clientId
                    << ", server=" << payload.serverName;
                break;
            }
            case MessageType::PlaylistSync: {
                const PlaylistSyncPayload payload = parse_playlist_sync_payload(message.payload);
                out << ", version=" << payload.playlistVersion
                    << ", currentTrackId=" << payload.currentTrackId
                    << ", tracks=" << payload.tracks.size();
                break;
            }
            case MessageType::ClientsSync: {
                const ClientsSyncPayload payload = parse_clients_sync_payload(message.payload);
                out << ", version=" << payload.clientsVersion
                    << ", clients=" << payload.clients.size();
                break;
            }
            case MessageType::TrackInfo: {
                const TrackInfoPayload payload = parse_track_info_payload(message.payload);
                out << ", trackId=" << payload.trackId
                    << ", file=" << payload.fileName
                    << ", size=" << payload.fileSize;
                break;
            }
            case MessageType::TrackChunk: {
                const TrackChunkPayload payload = parse_track_chunk_payload(message.payload);
                out << ", trackId=" << payload.trackId
                    << ", offset=" << payload.offset
                    << ", chunkSize=" << payload.chunkData.size();
                break;
            }
            case MessageType::Ready: {
                const ReadyPayload payload = parse_ready_payload(message.payload);
                out << ", clientId=" << payload.clientId
                    << ", trackId=" << payload.trackId;
                break;
            }
            case MessageType::StateSync: {
                const StateSyncPayload payload = parse_state_sync_payload(message.payload);
                out << ", version=" << payload.stateVersion
                    << ", trackId=" << payload.currentTrackId
                    << ", state=" << playback_state_name(payload.playbackState)
                    << ", positionMs=" << payload.positionMs
                    << ", startAtHostMs=" << payload.startAtHostMs
                    << ", volume=" << payload.volumePercent
                    << ", autoplay=" << (payload.autoplayEnabled ? "on" : "off")
                    << ", repeat=" << repeat_mode_name(payload.repeatMode);
                break;
            }
            case MessageType::Ping: {
                const PingPayload payload = parse_ping_payload(message.payload);
                out << ", hostSendTimeMs=" << payload.hostSendTimeMs;
                break;
            }
            case MessageType::Pong: {
                const PongPayload payload = parse_pong_payload(message.payload);
                out << ", hostSendTimeMs=" << payload.hostSendTimeMs
                    << ", clientReplyTimeMs=" << payload.clientReplyTimeMs;
                break;
            }
            case MessageType::Quit: {
                const QuitPayload payload = parse_quit_payload(message.payload);
                out << ", clientId=" << payload.clientId;
                break;
            }
            case MessageType::Kick: {
                const KickPayload payload = parse_kick_payload(message.payload);
                out << ", clientId=" << payload.clientId
                    << ", reason=" << payload.reason;
                break;
            }
            case MessageType::Shutdown: {
                const ShutdownPayload payload = parse_shutdown_payload(message.payload);
                out << ", reason=" << payload.reason;
                break;
            }
        }

        out << ")";
        return out.str();
    }
    catch (const std::exception& error) {
        std::ostringstream out;
        out << message_type_name(message.type)
            << "(seq=" << message.sequence
            << ", payload=" << message.payload.size()
            << ", parseError=" << error.what()
            << ")";
        return out.str();
    }
}

inline void log_protocol_message(const char* direction, const Message& message) {
    if (!protocol_logs_enabled()) {
        return;
    }

    log(std::string("[protocol][") + direction + "] " + summarize_message(message));
}
