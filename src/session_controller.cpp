#include "session_controller.hpp"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QMediaPlayer>
#include <QPointer>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <memory>

namespace {

constexpr quint16 kDefaultPort = 54000;
constexpr int kPingIntervalMs = 3000;
constexpr int kPlaybackTickMs = 200;
// Host schedules playback slightly in the future so listeners can receive the
// latest STATE_SYNC and start the same file nearly at the same timestamp.
constexpr qint64 kScheduledLeadTimeMs = 500;
constexpr int kTrackChunkSize = 64 * 1024;
constexpr int kTrackChunksPerPump = 4;
constexpr qint64 kLateJoinMinLeadTimeMs = 200;
constexpr qint64 kLateJoinMaxLeadTimeMs = 1200;
constexpr qint64 kLateJoinJitterPaddingMs = 120;

uint16_t read_be16(const char* data) {
    return static_cast<uint16_t>((static_cast<uint8_t>(data[0]) << 8U) |
                                 static_cast<uint8_t>(data[1]));
}

uint32_t read_be32(const char* data) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8U) | static_cast<uint8_t>(data[i]);
    }
    return value;
}

void write_be16(char* data, uint16_t value) {
    data[0] = static_cast<char>((value >> 8U) & 0xFFU);
    data[1] = static_cast<char>(value & 0xFFU);
}

void write_be32(char* data, uint32_t value) {
    data[0] = static_cast<char>((value >> 24U) & 0xFFU);
    data[1] = static_cast<char>((value >> 16U) & 0xFFU);
    data[2] = static_cast<char>((value >> 8U) & 0xFFU);
    data[3] = static_cast<char>(value & 0xFFU);
}

QByteArray serialize_message(const Message& message) {
    QByteArray wire;
    wire.resize(static_cast<qsizetype>(sizeof(MessageHeader) + message.payload.size()));

    write_be32(wire.data(), kProtocolMagic);
    write_be16(wire.data() + 4, kProtocolVersion);
    write_be16(wire.data() + 6, static_cast<uint16_t>(message.type));
    write_be32(wire.data() + 8, static_cast<uint32_t>(message.payload.size()));
    write_be32(wire.data() + 12, message.sequence);

    if (!message.payload.empty()) {
        std::copy(message.payload.begin(), message.payload.end(), wire.begin() + static_cast<qsizetype>(sizeof(MessageHeader)));
    }

    return wire;
}

bool parse_endpoint(const QString& endpoint, QString& host, quint16& port, bool serverMode) {
    QString trimmed = endpoint.trimmed();
    if (trimmed.isEmpty()) {
        host = serverMode ? QStringLiteral("0.0.0.0") : QStringLiteral("127.0.0.1");
        port = kDefaultPort;
        return true;
    }

    const int colonIndex = trimmed.lastIndexOf(':');
    if (colonIndex >= 0) {
        host = trimmed.left(colonIndex).trimmed();
        bool ok = false;
        const int parsedPort = trimmed.mid(colonIndex + 1).trimmed().toInt(&ok);
        if (!ok || parsedPort <= 0 || parsedPort > 65535) {
            return false;
        }
        port = static_cast<quint16>(parsedPort);
    }
    else {
        host = trimmed;
        port = kDefaultPort;
    }

    if (host.isEmpty()) {
        host = serverMode ? QStringLiteral("0.0.0.0") : QStringLiteral("127.0.0.1");
    }

    if (!serverMode && host == QStringLiteral("0.0.0.0")) {
        host = QStringLiteral("127.0.0.1");
    }

    return true;
}

QString sanitize_file_name(const QString& value) {
    QString result = value;
    result.replace('\\', '_');
    result.replace('/', '_');
    result.replace(':', '_');
    result.replace('*', '_');
    result.replace('?', '_');
    result.replace('"', '_');
    result.replace('<', '_');
    result.replace('>', '_');
    result.replace('|', '_');
    return result;
}

class QtPlaybackEngine {
public:
    QtPlaybackEngine() {
        player_.setAudioOutput(&audioOutput_);
        audioOutput_.setVolume(0.5F);
    }

    bool openFile(const QString& path) {
        if (path.isEmpty()) {
            close();
            lastErrorText_ = QStringLiteral("Empty path.");
            return false;
        }

        if (openedPath_ == path && isOpen_) {
            return true;
        }

        close();
        if (!QFileInfo::exists(path)) {
            lastErrorText_ = QStringLiteral("File does not exist.");
            return false;
        }

        player_.setSource(QUrl::fromLocalFile(path));
        wait_for_media_state();

        if (player_.error() != QMediaPlayer::NoError ||
            player_.mediaStatus() == QMediaPlayer::InvalidMedia) {
            lastErrorText_ = player_.errorString().isEmpty()
                ? QStringLiteral("Unsupported or unavailable media format.")
                : player_.errorString();
            player_.setSource(QUrl());
            return false;
        }

        openedPath_ = path;
        isOpen_ = true;
        durationMs_ = std::max<qint64>(0, player_.duration());
        lastErrorText_.clear();
        return true;
    }

    void close() {
        player_.stop();
        player_.setSource(QUrl());
        openedPath_.clear();
        durationMs_ = 0;
        isOpen_ = false;
        lastErrorText_.clear();
    }

    bool playFrom(qint64 positionMs) {
        if (!isOpen_) {
            return false;
        }

        player_.setPosition(std::max<qint64>(0, positionMs));
        player_.play();
        if (player_.error() != QMediaPlayer::NoError) {
            lastErrorText_ = player_.errorString().isEmpty()
                ? QStringLiteral("Playback start failed.")
                : player_.errorString();
            return false;
        }
        return true;
    }

    void pause() {
        if (isOpen_) {
            player_.pause();
        }
    }

    void stop() {
        if (isOpen_) {
            player_.stop();
            player_.setPosition(0);
        }
    }

    void seek(qint64 positionMs) {
        if (isOpen_) {
            player_.setPosition(std::max<qint64>(0, positionMs));
        }
    }

    void setVolumePercent(int volumePercent) {
        const int clamped = std::clamp(volumePercent, 0, 100);
        audioOutput_.setVolume(static_cast<float>(clamped) / 100.0F);
    }

    qint64 positionMs() const {
        if (!isOpen_) {
            return 0;
        }
        return player_.position();
    }

    qint64 durationMs() const {
        return std::max(durationMs_, player_.duration());
    }

    bool isOpen() const {
        return isOpen_;
    }

    bool isPlaying() const {
        return player_.playbackState() == QMediaPlayer::PlayingState;
    }

    bool isPaused() const {
        return player_.playbackState() == QMediaPlayer::PausedState;
    }

    QString lastErrorText() const {
        return lastErrorText_.isEmpty() ? player_.errorString() : lastErrorText_;
    }

    bool reachedEnd() const {
        return player_.mediaStatus() == QMediaPlayer::EndOfMedia;
    }

private:
    void wait_for_media_state() {
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);

        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&player_, &QMediaPlayer::mediaStatusChanged, &loop, [&](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::LoadedMedia ||
                status == QMediaPlayer::BufferedMedia ||
                status == QMediaPlayer::InvalidMedia) {
                loop.quit();
            }
        });
        QObject::connect(&player_, &QMediaPlayer::errorOccurred, &loop, [&](QMediaPlayer::Error, const QString&) {
            loop.quit();
        });

        timeoutTimer.start(2000);
        loop.exec();

        durationMs_ = std::max<qint64>(0, player_.duration());
    }

    QAudioOutput audioOutput_;
    QMediaPlayer player_;
    QString openedPath_;
    qint64 durationMs_ = 0;
    bool isOpen_ = false;
    QString lastErrorText_;
};

} // namespace

class SessionController::Impl : public QObject {
public:
    struct PeerConnection {
        QPointer<QTcpSocket> socket;
        QByteArray buffer;
        uint32_t clientId = 0;
        QString nickname;
        ClientStatus status = ClientStatus::Connected;
        bool readyForCurrentTrack = false;
        qint64 lastRttMs = 0;
        uint32_t outgoingTrackId = 0;
        quint64 outgoingTrackOffset = 0;
        std::unique_ptr<QFile> outgoingTrackFile;
    };

    struct IncomingTrackTransfer {
        uint32_t trackId = 0;
        uint64_t expectedSize = 0;
        uint64_t receivedSize = 0;
        QString fileName;
        QString localPath;
        std::unique_ptr<QFile> file;
    };

    explicit Impl(SessionController& owner)
        : QObject(&owner)
        , owner_(owner) {
        pingTimer_.setInterval(kPingIntervalMs);
        connect(&pingTimer_, &QTimer::timeout, this, [this]() {
            if (sessionActive_ && hostMode_) {
                const PingPayload payload{ now_ms() };
                broadcast(MessageType::Ping, build_ping_payload(payload));
            }
        });

        progressTimer_.setInterval(kPlaybackTickMs);
        connect(&progressTimer_, &QTimer::timeout, this, [this]() {
            handlePlaybackTick();
        });
        progressTimer_.start();

        outgoingTrackPumpTimer_.setInterval(0);
        connect(&outgoingTrackPumpTimer_, &QTimer::timeout, this, [this]() {
            pump_outgoing_track_transfers();
        });

        scheduledStartTimer_.setSingleShot(true);
        connect(&scheduledStartTimer_, &QTimer::timeout, this, [this]() {
            if (scheduledStartPositionMs_ >= 0) {
                player_.playFrom(scheduledStartPositionMs_);
                player_.setVolumePercent(static_cast<int>(state_.volumePercent));
                invoke_session_changed();
            }
        });
    }

    void setLocalLibrary(const std::vector<TrackEntry>& tracks) {
        if (!hostMode_ && sessionActive_) {
            return;
        }

        const uint32_t previousTrackId = state_.currentTrackId;
        playlist_ = tracks;

        for (size_t index = 0; index < playlist_.size(); ++index) {
            playlist_[index].descriptor.trackId = static_cast<uint32_t>(index + 1);
            playlist_[index].availableLocally = !playlist_[index].localPath.isEmpty();
        }

        const int previousIndex = find_track_index(previousTrackId);
        if (previousIndex < 0) {
            state_.currentTrackId = playlist_.empty() ? 0 : playlist_.front().descriptor.trackId;
            state_.positionMs = 0;
            state_.playbackState = PlaybackState::Stopped;
        }
        else {
            state_.currentTrackId = playlist_[previousIndex].descriptor.trackId;
        }

        ++playlistVersion_;
        update_host_client_entry();
        if (sessionActive_ && hostMode_) {
            broadcast_playlist_sync();
            broadcast_state_sync();
        }
        prepare_local_track_if_available(state_.currentTrackId);
        invoke_session_changed();
    }

    bool startHost(const QString& endpoint) {
        stop_session_internal(false, QString());

        hostMode_ = true;

        QString host;
        quint16 port = kDefaultPort;
        if (!parse_endpoint(endpoint, host, port, true)) {
            set_status(QStringLiteral("Invalid endpoint for host mode."));
            return false;
        }

        server_ = new QTcpServer(this);
        connect(server_, &QTcpServer::newConnection, this, [this]() {
            handle_new_connection();
        });

        const QHostAddress address = (host == QStringLiteral("0.0.0.0"))
            ? QHostAddress::AnyIPv4
            : QHostAddress(host);

        if (!server_->listen(address, port)) {
            set_status(QStringLiteral("Failed to start host: %1").arg(server_->errorString()));
            server_->deleteLater();
            server_ = nullptr;
            return false;
        }

        sessionActive_ = true;
        serverName_ = QHostInfo::localHostName();
        localClientId_ = 1;
        nextClientId_ = 2;
        hostClockOffsetMs_ = 0;
        clients_.clear();
        update_host_client_entry();
        pingTimer_.start();

        if (state_.currentTrackId == 0 && !playlist_.empty()) {
            state_.currentTrackId = playlist_.front().descriptor.trackId;
        }

        prepare_local_track_if_available(state_.currentTrackId);
        set_status(QStringLiteral("Host started on %1:%2").arg(host).arg(port));
        invoke_session_changed();
        return true;
    }

    bool connectToHost(const QString& endpoint) {
        stop_session_internal(false, QString());

        hostMode_ = false;

        QString host;
        quint16 port = kDefaultPort;
        if (!parse_endpoint(endpoint, host, port, false)) {
            set_status(QStringLiteral("Invalid endpoint for client mode."));
            return false;
        }

        clientSocket_ = new QTcpSocket(this);
        connect(clientSocket_, &QTcpSocket::connected, this, [this, host, port]() {
            sessionActive_ = true;
            playlist_.clear();
            clients_.clear();
            state_ = {};
            set_status(QStringLiteral("Connected to host %1:%2, sending HELLO.").arg(host).arg(port));

            const HelloPayload hello{
                QHostInfo::localHostName().toStdString(),
                now_ms()
            };
            send_message(clientSocket_, MessageType::Hello, build_hello_payload(hello));
            invoke_session_changed();
        });

        connect(clientSocket_, &QTcpSocket::readyRead, this, [this]() {
            if (clientSocket_ == nullptr) {
                return;
            }
            clientBuffer_.append(clientSocket_->readAll());
            process_buffer(clientSocket_, clientBuffer_, nullptr);
        });

        connect(clientSocket_, &QTcpSocket::disconnected, this, [this]() {
            stop_session_internal(false, QStringLiteral("Disconnected from host."));
        });

        connect(clientSocket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (clientSocket_ != nullptr) {
                set_status(QStringLiteral("Client socket error: %1").arg(clientSocket_->errorString()));
            }
        });

        clientSocket_->connectToHost(host, port);
        set_status(QStringLiteral("Connecting to %1:%2...").arg(host).arg(port));
        return true;
    }

    void stopSession() {
        stop_session_internal(true, QStringLiteral("Session stopped locally."));
    }

    void selectTrackByRow(int row) {
        if (!hostMode_) {
            invoke_session_changed();
            return;
        }

        if (row < 0 || row >= static_cast<int>(playlist_.size())) {
            return;
        }

        scheduledStartTimer_.stop();
        pendingPlayWhenReady_ = false;
        scheduledStartPositionMs_ = -1;
        player_.stop();

        state_.currentTrackId = playlist_[static_cast<size_t>(row)].descriptor.trackId;
        state_.positionMs = 0;
        state_.playbackState = PlaybackState::Stopped;
        prepare_local_track_if_available(state_.currentTrackId);

        if (sessionActive_ && hostMode_) {
            mark_all_peers_not_ready();
            ++playlistVersion_;
            broadcast_playlist_sync();
            distribute_current_track();
            broadcast_state_sync();
            broadcast_clients_sync();
        }

        if (state_.autoplayEnabled) {
            start_playback_when_ready(0);
        }

        invoke_session_changed();
    }

    void playPause() {
        if (!hostMode_) {
            return;
        }

        if (playlist_.empty()) {
            set_status(QStringLiteral("Playlist is empty."));
            return;
        }

        if (state_.currentTrackId == 0) {
            selectTrackByRow(0);
        }

        if (!prepare_local_track_if_available(state_.currentTrackId)) {
            const int index = currentTrackIndex();
            const QString trackTitle = index >= 0
                ? QString::fromStdString(playlist_[static_cast<size_t>(index)].descriptor.title)
                : QStringLiteral("current track");
            const QString backendReason = player_.lastErrorText().isEmpty()
                ? QStringLiteral("Unsupported or unavailable local playback format.")
                : player_.lastErrorText();
            set_status(QStringLiteral("Current track is not available for playback: %1 (%2)")
                           .arg(trackTitle, backendReason));
            return;
        }

        if (state_.playbackState == PlaybackState::Playing) {
            state_.positionMs = currentPlaybackPositionMs();
            state_.playbackState = PlaybackState::Paused;
            scheduledStartTimer_.stop();
            player_.pause();
            if (sessionActive_) {
                broadcast_state_sync();
            }
            invoke_session_changed();
            return;
        }

        const uint64_t resumePosition = static_cast<uint64_t>(currentPlaybackPositionMs());
        start_playback_when_ready(resumePosition);
    }

    void stopPlayback() {
        if (!hostMode_) {
            return;
        }

        pendingPlayWhenReady_ = false;
        scheduledStartTimer_.stop();
        scheduledStartPositionMs_ = -1;
        player_.stop();
        state_.positionMs = 0;
        state_.playbackState = PlaybackState::Stopped;
        if (sessionActive_) {
            broadcast_state_sync();
        }
        invoke_session_changed();
    }

    void nextTrack() {
        if (!hostMode_ || playlist_.empty()) {
            return;
        }

        int nextIndex = currentTrackIndex();
        if (nextIndex < 0) {
            nextIndex = 0;
        }
        else if (nextIndex + 1 < static_cast<int>(playlist_.size())) {
            ++nextIndex;
        }
        else if (state_.repeatMode == RepeatMode::Playlist) {
            nextIndex = 0;
        }
        else {
            return;
        }

        selectTrackByRow(nextIndex);
    }

    void previousTrack() {
        if (!hostMode_ || playlist_.empty()) {
            return;
        }

        int previousIndex = currentTrackIndex();
        if (previousIndex < 0) {
            previousIndex = 0;
        }
        else if (previousIndex > 0) {
            --previousIndex;
        }
        else if (state_.repeatMode == RepeatMode::Playlist) {
            previousIndex = static_cast<int>(playlist_.size()) - 1;
        }
        else {
            previousIndex = 0;
        }

        selectTrackByRow(previousIndex);
    }

    void seekToPositionMs(qint64 positionMs) {
        if (!hostMode_ || state_.currentTrackId == 0) {
            return;
        }

        const qint64 durationMs = currentPlaybackDurationMs();
        qint64 clamped = std::max<qint64>(0, positionMs);
        if (durationMs > 0) {
            clamped = std::min(clamped, durationMs);
        }
        state_.positionMs = static_cast<uint64_t>(clamped);

        if (state_.playbackState == PlaybackState::Playing) {
            start_playback_when_ready(state_.positionMs);
        }
        else {
            player_.seek(clamped);
            if (sessionActive_) {
                broadcast_state_sync();
            }
            invoke_session_changed();
        }
    }

    void setVolumePercent(int volumePercent) {
        state_.volumePercent = static_cast<uint32_t>(std::clamp(volumePercent, 0, 100));
        player_.setVolumePercent(static_cast<int>(state_.volumePercent));
        if (sessionActive_ && hostMode_) {
            broadcast_state_sync();
        }
        invoke_session_changed();
    }

    void setAutoplayEnabled(bool enabled) {
        state_.autoplayEnabled = enabled;
        if (sessionActive_ && hostMode_) {
            broadcast_state_sync();
        }
        invoke_session_changed();
    }

    void setRepeatMode(RepeatMode mode) {
        state_.repeatMode = mode;
        if (sessionActive_ && hostMode_) {
            broadcast_state_sync();
        }
        invoke_session_changed();
    }

    void kickClient(uint32_t clientId) {
        if (!hostMode_) {
            return;
        }

        PeerConnection* peer = find_peer_by_client_id(clientId);
        if (peer == nullptr || peer->socket == nullptr) {
            return;
        }

        const KickPayload payload{ clientId, std::string("Removed by host") };
        send_message(peer->socket, MessageType::Kick, build_kick_payload(payload));
        peer->socket->disconnectFromHost();
    }

    bool isSessionActive() const {
        return sessionActive_;
    }

    bool isHostMode() const {
        return hostMode_;
    }

    uint32_t localClientId() const {
        return localClientId_;
    }

    int currentTrackIndex() const {
        return find_track_index(state_.currentTrackId);
    }

    qint64 currentPlaybackPositionMs() const {
        if (scheduledStartTimer_.isActive()) {
            return scheduledStartPositionMs_;
        }

        if (player_.isOpen() && (player_.isPlaying() || player_.isPaused())) {
            return player_.positionMs();
        }

        return static_cast<qint64>(state_.positionMs);
    }

    qint64 currentPlaybackDurationMs() const {
        const int index = currentTrackIndex();
        if (index < 0) {
            return 0;
        }

        return static_cast<qint64>(playlist_[static_cast<size_t>(index)].descriptor.durationMs);
    }

    QString currentTrackPath() const {
        const int index = currentTrackIndex();
        if (index < 0) {
            return {};
        }
        return playlist_[static_cast<size_t>(index)].localPath;
    }

    const std::vector<TrackEntry>& playlist() const {
        return playlist_;
    }

    const std::vector<ClientDescriptor>& clients() const {
        return clients_;
    }

    const StateSyncPayload& sessionState() const {
        return state_;
    }

    QString statusText() const {
        return statusText_;
    }

private:
    void invoke_session_changed() {
        QMetaObject::invokeMethod(&owner_, "sessionChanged", Qt::DirectConnection);
    }

    void set_status(const QString& message) {
        statusText_ = message;
        QMetaObject::invokeMethod(
            &owner_,
            "statusMessageChanged",
            Qt::DirectConnection,
            Q_ARG(QString, message)
        );
    }

    void handle_new_connection() {
        while (server_ != nullptr && server_->hasPendingConnections()) {
            QTcpSocket* socket = server_->nextPendingConnection();
            auto peer = std::make_unique<PeerConnection>();
            peer->socket = socket;

            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                PeerConnection* peerPtr = find_peer_by_socket(socket);
                if (peerPtr == nullptr) {
                    return;
                }
                peerPtr->buffer.append(socket->readAll());
                process_buffer(socket, peerPtr->buffer, peerPtr);
            });

            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                handle_peer_disconnect(socket);
            });

            connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
                PeerConnection* peerPtr = find_peer_by_socket(socket);
                if (peerPtr != nullptr && socket != nullptr) {
                    set_status(QStringLiteral("Peer socket error: %1").arg(socket->errorString()));
                }
            });

            peers_.push_back(std::move(peer));
            set_status(QStringLiteral("Incoming connection accepted, waiting for HELLO."));
        }
    }

    void process_buffer(QTcpSocket* socket, QByteArray& buffer, PeerConnection* peer) {
        // TCP is a byte stream, so one readyRead() can contain partial frames
        // or several protocol messages back-to-back.
        while (buffer.size() >= static_cast<int>(sizeof(MessageHeader))) {
            const MessageHeader header{
                read_be32(buffer.constData()),
                read_be16(buffer.constData() + 4),
                read_be16(buffer.constData() + 6),
                read_be32(buffer.constData() + 8),
                read_be32(buffer.constData() + 12)
            };

            if (header.magic != kProtocolMagic || header.version != kProtocolVersion) {
                set_status(QStringLiteral("Protocol header mismatch, closing socket."));
                socket->disconnectFromHost();
                buffer.clear();
                return;
            }

            const qsizetype totalSize = static_cast<qsizetype>(sizeof(MessageHeader) + header.payloadSize);
            if (buffer.size() < totalSize) {
                return;
            }

            Message message;
            message.type = static_cast<MessageType>(header.type);
            message.sequence = header.sequence;
            message.payload.assign(
                reinterpret_cast<const uint8_t*>(buffer.constData() + sizeof(MessageHeader)),
                reinterpret_cast<const uint8_t*>(buffer.constData() + totalSize)
            );

            buffer.remove(0, totalSize);
            log_protocol_message("recv", message);
            process_message(socket, peer, message);
        }
    }

    void process_message(QTcpSocket* socket, PeerConnection* peer, const Message& message) {
        if (hostMode_) {
            process_host_message(socket, peer, message);
        }
        else {
            process_client_message(socket, message);
        }
    }

    void process_host_message(QTcpSocket* socket, PeerConnection* peer, const Message& message) {
        if (peer == nullptr) {
            return;
        }

        switch (message.type) {
            case MessageType::Hello: {
                const HelloPayload payload = parse_hello_payload(message.payload);
                peer->clientId = nextClientId_++;
                peer->nickname = QString::fromStdString(payload.nickname);
                peer->status = ClientStatus::Connected;
                peer->readyForCurrentTrack = false;

                const WelcomePayload welcome{
                    peer->clientId,
                    serverName_.toStdString(),
                    4,
                    kProtocolVersion
                };
                send_message(socket, MessageType::Welcome, build_welcome_payload(welcome));
                update_remote_client_entries();
                broadcast_clients_sync();
                send_playlist_sync_to(socket);

                if (state_.currentTrackId != 0) {
                    send_current_track_to(*peer);
                }
                else {
                    send_state_sync_to(socket);
                }

                set_status(QStringLiteral("Client %1 connected.").arg(peer->nickname));
                break;
            }
            case MessageType::Ready: {
                const ReadyPayload payload = parse_ready_payload(message.payload);
                if (payload.clientId == peer->clientId && payload.trackId == state_.currentTrackId) {
                    peer->readyForCurrentTrack = true;
                    peer->status = ClientStatus::Ready;
                    update_remote_client_entries();
                    broadcast_clients_sync();
                    maybe_start_pending_playback();
                    if (state_.playbackState == PlaybackState::Playing &&
                        !pendingPlayWhenReady_ &&
                        !scheduledStartTimer_.isActive()) {
                        send_late_join_playback_sync(*peer);
                    }
                    else {
                        send_state_sync_to(socket);
                    }
                }
                break;
            }
            case MessageType::Pong: {
                const PongPayload payload = parse_pong_payload(message.payload);
                peer->lastRttMs = static_cast<qint64>(now_ms() - payload.hostSendTimeMs);
                break;
            }
            case MessageType::Quit: {
                socket->disconnectFromHost();
                break;
            }
            default:
                break;
        }
    }

    void process_client_message(QTcpSocket* socket, const Message& message) {
        Q_UNUSED(socket);

        switch (message.type) {
            case MessageType::Welcome: {
                const WelcomePayload payload = parse_welcome_payload(message.payload);
                localClientId_ = payload.clientId;
                serverName_ = QString::fromStdString(payload.serverName);
                set_status(QStringLiteral("WELCOME received from %1 with id #%2.")
                               .arg(serverName_)
                               .arg(localClientId_));
                break;
            }
            case MessageType::PlaylistSync: {
                const PlaylistSyncPayload payload = parse_playlist_sync_payload(message.payload);
                apply_playlist_sync(payload);
                break;
            }
            case MessageType::ClientsSync: {
                const ClientsSyncPayload payload = parse_clients_sync_payload(message.payload);
                clients_ = payload.clients;
                invoke_session_changed();
                break;
            }
            case MessageType::TrackInfo: {
                const TrackInfoPayload payload = parse_track_info_payload(message.payload);
                begin_track_receive(payload);
                break;
            }
            case MessageType::TrackChunk: {
                const TrackChunkPayload payload = parse_track_chunk_payload(message.payload);
                consume_track_chunk(payload);
                break;
            }
            case MessageType::StateSync: {
                const StateSyncPayload payload = parse_state_sync_payload(message.payload);
                apply_remote_state(payload);
                break;
            }
            case MessageType::Ping: {
                const PingPayload payload = parse_ping_payload(message.payload);
                hostClockOffsetMs_ = static_cast<qint64>(payload.hostSendTimeMs) - static_cast<qint64>(now_ms());
                const PongPayload pong{
                    payload.hostSendTimeMs,
                    now_ms()
                };
                send_message(clientSocket_, MessageType::Pong, build_pong_payload(pong));
                break;
            }
            case MessageType::Kick: {
                const KickPayload payload = parse_kick_payload(message.payload);
                QTimer::singleShot(0, this, [this, reason = QString::fromStdString(payload.reason)]() {
                    stop_session_internal(false, QStringLiteral("Disconnected by host: %1").arg(reason));
                });
                break;
            }
            case MessageType::Shutdown: {
                const ShutdownPayload payload = parse_shutdown_payload(message.payload);
                QTimer::singleShot(0, this, [this, reason = QString::fromStdString(payload.reason)]() {
                    stop_session_internal(false, QStringLiteral("Host closed the session: %1").arg(reason));
                });
                break;
            }
            default:
                break;
        }
    }

    void send_message(QTcpSocket* socket, MessageType type, std::vector<uint8_t> payload) {
        if (socket == nullptr) {
            return;
        }

        Message message;
        message.type = type;
        message.sequence = nextSequence_++;
        message.payload = std::move(payload);

        log_protocol_message("send", message);
        socket->write(serialize_message(message));
    }

    void broadcast(MessageType type, const std::vector<uint8_t>& payload) {
        for (const auto& peer : peers_) {
            if (peer->socket != nullptr && peer->socket->state() == QAbstractSocket::ConnectedState) {
                send_message(peer->socket, type, payload);
            }
        }
    }

    void broadcast_playlist_sync() {
        if (!hostMode_) {
            return;
        }
        broadcast(MessageType::PlaylistSync, make_playlist_sync_payload());
    }

    void send_playlist_sync_to(QTcpSocket* socket) {
        send_message(socket, MessageType::PlaylistSync, make_playlist_sync_payload());
    }

    std::vector<uint8_t> make_playlist_sync_payload() const {
        PlaylistSyncPayload payload;
        payload.playlistVersion = playlistVersion_;
        payload.currentTrackId = state_.currentTrackId;
        payload.tracks.reserve(playlist_.size());
        for (const TrackEntry& track : playlist_) {
            payload.tracks.push_back(track.descriptor);
        }
        return build_playlist_sync_payload(payload);
    }

    void broadcast_clients_sync() {
        if (!hostMode_) {
            return;
        }

        ClientsSyncPayload payload;
        payload.clientsVersion = clientsVersion_;
        payload.clients = clients_;
        broadcast(MessageType::ClientsSync, build_clients_sync_payload(payload));
    }

    void broadcast_state_sync() {
        if (!hostMode_) {
            return;
        }

        StateSyncPayload snapshot = make_state_sync_snapshot();
        snapshot.stateVersion = stateVersion_++;
        state_.hostTimeMs = snapshot.hostTimeMs;
        state_.startAtHostMs = snapshot.startAtHostMs;
        state_.positionMs = snapshot.positionMs;
        broadcast(MessageType::StateSync, build_state_sync_payload(snapshot));
    }

    void send_state_sync_to(QTcpSocket* socket) {
        StateSyncPayload snapshot = make_state_sync_snapshot();
        snapshot.stateVersion = stateVersion_++;
        send_message(socket, MessageType::StateSync, build_state_sync_payload(snapshot));
    }

    void mark_all_peers_not_ready() {
        for (const auto& peer : peers_) {
            peer->readyForCurrentTrack = false;
            peer->status = ClientStatus::Connected;
        }
        update_remote_client_entries();
    }

    void distribute_current_track() {
        for (const auto& peer : peers_) {
            if (peer->socket != nullptr && peer->socket->state() == QAbstractSocket::ConnectedState) {
                send_current_track_to(*peer);
            }
        }
    }

    void send_current_track_to(PeerConnection& peer) {
        const int index = currentTrackIndex();
        if (index < 0 || peer.socket == nullptr) {
            return;
        }

        const TrackEntry& track = playlist_[static_cast<size_t>(index)];
        if (track.localPath.isEmpty()) {
            return;
        }

        peer.readyForCurrentTrack = false;
        peer.status = ClientStatus::Connected;
        peer.outgoingTrackId = track.descriptor.trackId;
        peer.outgoingTrackOffset = 0;
        peer.outgoingTrackFile = std::make_unique<QFile>(track.localPath);
        if (!peer.outgoingTrackFile->open(QIODevice::ReadOnly)) {
            set_status(QStringLiteral("Failed to open %1 for async transfer.").arg(track.localPath));
            peer.outgoingTrackFile.reset();
            peer.outgoingTrackId = 0;
            return;
        }

        const TrackInfoPayload info{
            track.descriptor.trackId,
            track.descriptor.fileName,
            track.descriptor.fileSize,
            track.descriptor.durationMs
        };
        send_message(peer.socket, MessageType::TrackInfo, build_track_info_payload(info));
        if (!outgoingTrackPumpTimer_.isActive()) {
            outgoingTrackPumpTimer_.start();
        }
    }

    void begin_track_receive(const TrackInfoPayload& payload) {
        incomingTransfer_ = {};
        incomingTransfer_.trackId = payload.trackId;
        incomingTransfer_.expectedSize = payload.fileSize;
        incomingTransfer_.fileName = QString::fromStdString(payload.fileName);

        // Listeners keep incoming tracks in a per-client temp cache so Qt can
        // open them as ordinary local files on both Windows and Linux.
        const QString baseDir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("sync-music-player-cache/%1").arg(localClientId_ == 0 ? 0 : localClientId_));
        QDir().mkpath(baseDir);

        const QString localFileName = QStringLiteral("%1_%2").arg(payload.trackId).arg(sanitize_file_name(incomingTransfer_.fileName));
        incomingTransfer_.localPath = QDir(baseDir).filePath(localFileName);
        incomingTransfer_.file = std::make_unique<QFile>(incomingTransfer_.localPath);

        if (!incomingTransfer_.file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            set_status(QStringLiteral("Failed to prepare track file %1").arg(incomingTransfer_.localPath));
            incomingTransfer_ = {};
            return;
        }

        set_status(QStringLiteral("Receiving track %1").arg(incomingTransfer_.fileName));
    }

    void consume_track_chunk(const TrackChunkPayload& payload) {
        if (incomingTransfer_.file == nullptr || incomingTransfer_.trackId != payload.trackId) {
            return;
        }

        if (incomingTransfer_.file->pos() != static_cast<qint64>(payload.offset)) {
            incomingTransfer_.file->seek(static_cast<qint64>(payload.offset));
        }

        if (!payload.chunkData.empty()) {
            incomingTransfer_.file->write(reinterpret_cast<const char*>(payload.chunkData.data()),
                                          static_cast<qint64>(payload.chunkData.size()));
            incomingTransfer_.receivedSize += payload.chunkData.size();
        }

        if (incomingTransfer_.receivedSize >= incomingTransfer_.expectedSize) {
            incomingTransfer_.file->flush();
            incomingTransfer_.file->close();

            const int index = find_track_index(incomingTransfer_.trackId);
            if (index >= 0) {
                TrackEntry& track = playlist_[static_cast<size_t>(index)];
                track.localPath = incomingTransfer_.localPath;
                track.availableLocally = true;
                track.descriptor.fileName = incomingTransfer_.fileName.toStdString();
                track.descriptor.fileSize = incomingTransfer_.expectedSize;
            }

            const ReadyPayload ready{ localClientId_, incomingTransfer_.trackId };
            send_message(clientSocket_, MessageType::Ready, build_ready_payload(ready));
            prepare_local_track_if_available(incomingTransfer_.trackId);
            set_status(QStringLiteral("Track %1 received. READY sent.").arg(incomingTransfer_.fileName));
            invoke_session_changed();

            if (state_.currentTrackId == incomingTransfer_.trackId &&
                state_.playbackState == PlaybackState::Playing) {
                apply_remote_state(state_);
            }

            incomingTransfer_ = {};
        }
    }

    void apply_playlist_sync(const PlaylistSyncPayload& payload) {
        std::vector<TrackEntry> updated;
        updated.reserve(payload.tracks.size());

        for (const TrackDescriptor& descriptor : payload.tracks) {
            TrackEntry entry;
            entry.descriptor = descriptor;
            const int oldIndex = find_track_index(descriptor.trackId);
            if (oldIndex >= 0) {
                entry.localPath = playlist_[static_cast<size_t>(oldIndex)].localPath;
                entry.availableLocally = playlist_[static_cast<size_t>(oldIndex)].availableLocally;
            }
            updated.push_back(entry);
        }

        playlist_ = std::move(updated);
        state_.currentTrackId = payload.currentTrackId;
        invoke_session_changed();
    }

    void apply_remote_state(const StateSyncPayload& payload) {
        const StateSyncPayload previousState = state_;
        state_ = payload;
        prepare_local_track_if_available(state_.currentTrackId);

        const bool previousImmediatePlaying =
            previousState.playbackState == PlaybackState::Playing &&
            previousState.startAtHostMs <= previousState.hostTimeMs;
        const bool currentImmediatePlaying =
            state_.playbackState == PlaybackState::Playing &&
            state_.startAtHostMs <= state_.hostTimeMs;
        const bool onlySoftRuntimeChange =
            previousState.currentTrackId == state_.currentTrackId &&
            previousState.playbackState == state_.playbackState &&
            previousImmediatePlaying &&
            currentImmediatePlaying;

        if (onlySoftRuntimeChange) {
            // Avoid reopening the media pipeline when the playing track did not
            // really change; that would introduce an audible glitch.
            player_.setVolumePercent(static_cast<int>(state_.volumePercent));
        }
        else {
            apply_state_to_local_player();
        }

        invoke_session_changed();
    }

    void apply_state_to_local_player() {
        scheduledStartTimer_.stop();
        scheduledStartPositionMs_ = -1;

        const int index = currentTrackIndex();
        if (index >= 0 && !playlist_[static_cast<size_t>(index)].localPath.isEmpty()) {
            if (!player_.openFile(playlist_[static_cast<size_t>(index)].localPath)) {
                set_status(QStringLiteral("Local playback backend could not open %1 (%2)")
                               .arg(QString::fromStdString(playlist_[static_cast<size_t>(index)].descriptor.fileName),
                                    player_.lastErrorText().isEmpty()
                                        ? QStringLiteral("unsupported format")
                                        : player_.lastErrorText()));
                return;
            }
            player_.setVolumePercent(static_cast<int>(state_.volumePercent));
        }

        switch (state_.playbackState) {
            case PlaybackState::Stopped:
                player_.stop();
                player_.seek(0);
                break;
            case PlaybackState::Paused:
                player_.seek(static_cast<qint64>(state_.positionMs));
                player_.pause();
                break;
            case PlaybackState::Playing: {
                qint64 delayMs = 0;
                qint64 positionMs = static_cast<qint64>(state_.positionMs);
                if (!hostMode_) {
                    const qint64 estimatedHostNow = static_cast<qint64>(now_ms()) + hostClockOffsetMs_;
                    if (state_.startAtHostMs > state_.hostTimeMs) {
                        delayMs = static_cast<qint64>(state_.startAtHostMs) - estimatedHostNow;
                        if (delayMs < 0) {
                            positionMs += -delayMs;
                            delayMs = 0;
                        }
                    }
                    else {
                        positionMs += std::max<qint64>(0, estimatedHostNow - static_cast<qint64>(state_.hostTimeMs));
                    }
                }
                else {
                    const qint64 hostNow = static_cast<qint64>(now_ms());
                    if (state_.startAtHostMs > state_.hostTimeMs) {
                        delayMs = static_cast<qint64>(state_.startAtHostMs) - hostNow;
                        if (delayMs < 0) {
                            positionMs += -delayMs;
                            delayMs = 0;
                        }
                    }
                    else {
                        positionMs += std::max<qint64>(0, hostNow - static_cast<qint64>(state_.hostTimeMs));
                    }
                }

                scheduledStartPositionMs_ = clamp_position_to_current_track(positionMs);
                if (delayMs <= 0) {
                    player_.playFrom(scheduledStartPositionMs_);
                }
                else {
                    scheduledStartTimer_.start(static_cast<int>(delayMs));
                }
                break;
            }
        }
    }

    bool prepare_local_track_if_available(uint32_t trackId) {
        const int index = find_track_index(trackId);
        if (index < 0) {
            return false;
        }

        const TrackEntry& track = playlist_[static_cast<size_t>(index)];
        if (track.localPath.isEmpty()) {
            return false;
        }

        if (!player_.openFile(track.localPath)) {
            return false;
        }

        player_.setVolumePercent(static_cast<int>(state_.volumePercent));
        return true;
    }

    void start_playback_when_ready(uint64_t positionMs) {
        state_.positionMs = positionMs;

        if (!prepare_local_track_if_available(state_.currentTrackId)) {
            const int index = currentTrackIndex();
            const QString trackTitle = index >= 0
                ? QString::fromStdString(playlist_[static_cast<size_t>(index)].descriptor.title)
                : QStringLiteral("current track");
            const QString backendReason = player_.lastErrorText().isEmpty()
                ? QStringLiteral("Unsupported or unavailable local playback format.")
                : player_.lastErrorText();
            set_status(QStringLiteral("Cannot start playback for %1 (%2)")
                           .arg(trackTitle, backendReason));
            return;
        }

        if (!all_peers_ready_for_current_track()) {
            // READY gates playback until every connected listener has finished
            // receiving and opening the current track locally.
            pendingPlayWhenReady_ = true;
            pendingPlayPositionMs_ = positionMs;
            set_status(QStringLiteral("Waiting for listeners to send READY..."));
            if (sessionActive_) {
                broadcast_state_sync();
                broadcast_clients_sync();
            }
            invoke_session_changed();
            return;
        }

        pendingPlayWhenReady_ = false;
        state_.playbackState = PlaybackState::Playing;
        state_.hostTimeMs = now_ms();
        state_.startAtHostMs = state_.hostTimeMs + kScheduledLeadTimeMs;
        scheduledStartPositionMs_ = static_cast<qint64>(positionMs);
        scheduledStartTimer_.start(static_cast<int>(kScheduledLeadTimeMs));
        set_status(QStringLiteral("All listeners are ready. Playback scheduled."));
        if (sessionActive_) {
            broadcast_state_sync();
        }
        invoke_session_changed();
    }

    void maybe_start_pending_playback() {
        if (!pendingPlayWhenReady_ || !all_peers_ready_for_current_track()) {
            return;
        }

        start_playback_when_ready(pendingPlayPositionMs_);
    }

    bool all_peers_ready_for_current_track() const {
        for (const auto& peer : peers_) {
            if (peer->socket != nullptr &&
                peer->socket->state() == QAbstractSocket::ConnectedState &&
                !peer->readyForCurrentTrack) {
                return false;
            }
        }
        return true;
    }

    qint64 clamp_position_to_current_track(qint64 positionMs) const {
        const qint64 durationMs = currentPlaybackDurationMs();
        if (durationMs > 0) {
            return std::clamp(positionMs, qint64{0}, durationMs);
        }
        return std::max<qint64>(0, positionMs);
    }

    qint64 compute_late_join_lead_time_ms(const PeerConnection& peer) const {
        const qint64 halfRttMs = std::max<qint64>(0, peer.lastRttMs / 2);
        return std::clamp(halfRttMs + kLateJoinJitterPaddingMs,
                          kLateJoinMinLeadTimeMs,
                          kLateJoinMaxLeadTimeMs);
    }

    StateSyncPayload make_state_sync_snapshot() const {
        StateSyncPayload snapshot = state_;
        snapshot.hostTimeMs = now_ms();
        if (snapshot.playbackState == PlaybackState::Playing) {
            if (scheduledStartTimer_.isActive()) {
                snapshot.positionMs = static_cast<uint64_t>(clamp_position_to_current_track(scheduledStartPositionMs_));
            }
            else {
                snapshot.positionMs = static_cast<uint64_t>(clamp_position_to_current_track(currentPlaybackPositionMs()));
                snapshot.startAtHostMs = snapshot.hostTimeMs;
            }
        }
        else {
            snapshot.positionMs = static_cast<uint64_t>(clamp_position_to_current_track(currentPlaybackPositionMs()));
            snapshot.startAtHostMs = snapshot.hostTimeMs;
        }
        return snapshot;
    }

    void send_late_join_playback_sync(PeerConnection& peer) {
        if (peer.socket == nullptr || state_.playbackState != PlaybackState::Playing) {
            return;
        }

        StateSyncPayload snapshot = state_;
        const qint64 leadTimeMs = compute_late_join_lead_time_ms(peer);
        snapshot.stateVersion = stateVersion_++;
        snapshot.hostTimeMs = now_ms();
        snapshot.startAtHostMs = snapshot.hostTimeMs + static_cast<uint64_t>(leadTimeMs);
        snapshot.positionMs = static_cast<uint64_t>(
            clamp_position_to_current_track(currentPlaybackPositionMs() + leadTimeMs));
        send_message(peer.socket, MessageType::StateSync, build_state_sync_payload(snapshot));
    }

    void pump_outgoing_track_transfers() {
        bool hasPendingTransfers = false;
        for (const auto& peerPtr : peers_) {
            PeerConnection& peer = *peerPtr;
            if (peer.socket == nullptr || peer.socket->state() != QAbstractSocket::ConnectedState) {
                peer.outgoingTrackFile.reset();
                peer.outgoingTrackId = 0;
                peer.outgoingTrackOffset = 0;
                continue;
            }
            if (peer.outgoingTrackFile == nullptr) {
                continue;
            }

            for (int chunkIndex = 0; chunkIndex < kTrackChunksPerPump; ++chunkIndex) {
                if (peer.outgoingTrackFile == nullptr) {
                    break;
                }

                const QByteArray chunk = peer.outgoingTrackFile->read(kTrackChunkSize);
                if (chunk.isEmpty()) {
                    peer.outgoingTrackFile.reset();
                    peer.outgoingTrackId = 0;
                    peer.outgoingTrackOffset = 0;
                    break;
                }

                TrackChunkPayload payload;
                payload.trackId = peer.outgoingTrackId;
                payload.offset = peer.outgoingTrackOffset;
                payload.chunkData.assign(chunk.begin(), chunk.end());
                send_message(peer.socket, MessageType::TrackChunk, build_track_chunk_payload(payload));
                peer.outgoingTrackOffset += static_cast<quint64>(chunk.size());

                if (peer.outgoingTrackFile->atEnd()) {
                    peer.outgoingTrackFile.reset();
                    peer.outgoingTrackId = 0;
                    peer.outgoingTrackOffset = 0;
                    break;
                }
            }

            if (peer.outgoingTrackFile != nullptr) {
                hasPendingTransfers = true;
            }
        }

        if (!hasPendingTransfers) {
            outgoingTrackPumpTimer_.stop();
        }
    }

    void update_host_client_entry() {
        clients_.clear();
        ClientDescriptor host;
        host.clientId = localClientId_ == 0 ? 1 : localClientId_;
        host.nickname = serverName_.isEmpty() ? QHostInfo::localHostName().toStdString() : serverName_.toStdString();
        host.role = ClientRole::Host;
        host.status = ClientStatus::Ready;
        clients_.push_back(host);
        ++clientsVersion_;
    }

    void update_remote_client_entries() {
        if (!hostMode_) {
            return;
        }

        clients_.erase(clients_.begin() + 1, clients_.end());
        for (const auto& peer : peers_) {
            if (peer->clientId == 0 || peer->socket == nullptr) {
                continue;
            }

            ClientDescriptor descriptor;
            descriptor.clientId = peer->clientId;
            descriptor.nickname = peer->nickname.toStdString();
            descriptor.role = ClientRole::Listener;
            descriptor.status = peer->status;
            clients_.push_back(descriptor);
        }
        ++clientsVersion_;
        invoke_session_changed();
    }

    void handle_peer_disconnect(QTcpSocket* socket) {
        auto it = std::remove_if(peers_.begin(), peers_.end(), [socket](const std::unique_ptr<PeerConnection>& peer) {
            return peer->socket == socket;
        });

        if (it != peers_.end()) {
            peers_.erase(it, peers_.end());
            update_host_client_entry();
            update_remote_client_entries();
            broadcast_clients_sync();
            set_status(QStringLiteral("A client disconnected."));
        }
    }

    PeerConnection* find_peer_by_socket(QTcpSocket* socket) {
        for (const auto& peer : peers_) {
            if (peer->socket == socket) {
                return peer.get();
            }
        }
        return nullptr;
    }

    PeerConnection* find_peer_by_client_id(uint32_t clientId) {
        for (const auto& peer : peers_) {
            if (peer->clientId == clientId) {
                return peer.get();
            }
        }
        return nullptr;
    }

    int find_track_index(uint32_t trackId) const {
        if (trackId == 0) {
            return -1;
        }

        for (size_t index = 0; index < playlist_.size(); ++index) {
            if (playlist_[index].descriptor.trackId == trackId) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    void stop_session_internal(bool notifyRemote, const QString& statusMessage) {
        if (stopInProgress_) {
            return;
        }
        stopInProgress_ = true;

        if (notifyRemote) {
            if (hostMode_) {
                const ShutdownPayload payload{ std::string("Host stopped the session") };
                broadcast(MessageType::Shutdown, build_shutdown_payload(payload));
            }
            else if (clientSocket_ != nullptr && clientSocket_->state() == QAbstractSocket::ConnectedState && localClientId_ != 0) {
                const QuitPayload payload{ localClientId_ };
                send_message(clientSocket_, MessageType::Quit, build_quit_payload(payload));
            }
        }

        scheduledStartTimer_.stop();
        pendingPlayWhenReady_ = false;
        scheduledStartPositionMs_ = -1;
        pingTimer_.stop();
        outgoingTrackPumpTimer_.stop();
        player_.stop();
        player_.close();

        if (server_ != nullptr) {
            server_->close();
            server_->deleteLater();
            server_ = nullptr;
        }

        for (auto& peer : peers_) {
            if (peer->socket != nullptr) {
                peer->socket->disconnectFromHost();
                peer->socket->deleteLater();
            }
        }
        peers_.clear();

        if (clientSocket_ != nullptr) {
            clientSocket_->disconnectFromHost();
            clientSocket_->deleteLater();
            clientSocket_ = nullptr;
        }
        clientBuffer_.clear();
        incomingTransfer_ = {};

        sessionActive_ = false;
        localClientId_ = 0;
        nextClientId_ = 2;
        clients_.clear();
        if (hostMode_) {
            update_host_client_entry();
        }

        state_.playbackState = PlaybackState::Stopped;
        state_.positionMs = 0;
        state_.hostTimeMs = 0;
        state_.startAtHostMs = 0;

        if (!statusMessage.isEmpty()) {
            set_status(statusMessage);
        }

        invoke_session_changed();
        stopInProgress_ = false;
    }

    void handlePlaybackTick() {
        if (state_.playbackState == PlaybackState::Playing && player_.isOpen()) {
            state_.positionMs = static_cast<uint64_t>(currentPlaybackPositionMs());
        }

        const qint64 duration = currentPlaybackDurationMs();
        const qint64 position = currentPlaybackPositionMs();
        const bool reachedTrackEnd = player_.reachedEnd() ||
            (duration > 0 && position >= std::max<qint64>(0, duration - 120));
        if (hostMode_ &&
            state_.playbackState == PlaybackState::Playing &&
            reachedTrackEnd &&
            !handlingTrackFinish_) {
            handlingTrackFinish_ = true;
            handle_track_finished();
            handlingTrackFinish_ = false;
        }

        invoke_session_changed();
    }

    void handle_track_finished() {
        player_.stop();
        state_.positionMs = 0;
        state_.playbackState = PlaybackState::Stopped;

        if (!state_.autoplayEnabled) {
            broadcast_state_sync();
            return;
        }

        const int index = currentTrackIndex();
        if (index < 0) {
            return;
        }

        if (state_.repeatMode == RepeatMode::Track) {
            start_playback_when_ready(0);
            return;
        }

        int nextIndex = index + 1;
        if (nextIndex >= static_cast<int>(playlist_.size())) {
            if (state_.repeatMode == RepeatMode::Playlist) {
                nextIndex = 0;
            }
            else {
                broadcast_state_sync();
                return;
            }
        }

        selectTrackByRow(nextIndex);
    }

    SessionController& owner_;
    QTcpServer* server_ = nullptr;
    QTcpSocket* clientSocket_ = nullptr;
    QByteArray clientBuffer_;
    std::vector<std::unique_ptr<PeerConnection>> peers_;
    std::vector<TrackEntry> playlist_;
    std::vector<ClientDescriptor> clients_;
    IncomingTrackTransfer incomingTransfer_;
    QtPlaybackEngine player_;
    QTimer pingTimer_;
    QTimer progressTimer_;
    QTimer outgoingTrackPumpTimer_;
    QTimer scheduledStartTimer_;
    StateSyncPayload state_;
    QString statusText_;
    QString serverName_;
    bool sessionActive_ = false;
    bool hostMode_ = true;
    bool pendingPlayWhenReady_ = false;
    bool handlingTrackFinish_ = false;
    bool stopInProgress_ = false;
    qint64 hostClockOffsetMs_ = 0;
    qint64 scheduledStartPositionMs_ = -1;
    uint64_t pendingPlayPositionMs_ = 0;
    uint32_t localClientId_ = 0;
    uint32_t nextClientId_ = 2;
    uint32_t nextSequence_ = 1;
    uint32_t playlistVersion_ = 1;
    uint32_t clientsVersion_ = 1;
    uint32_t stateVersion_ = 1;
};

SessionController::SessionController(QObject* parent)
    : QObject(parent)
    , impl_(new Impl(*this)) {
}

SessionController::~SessionController() {
    delete impl_;
}

void SessionController::setLocalLibrary(const std::vector<TrackEntry>& tracks) {
    impl_->setLocalLibrary(tracks);
}

bool SessionController::startHost(const QString& endpoint) {
    return impl_->startHost(endpoint);
}

bool SessionController::connectToHost(const QString& endpoint) {
    return impl_->connectToHost(endpoint);
}

void SessionController::stopSession() {
    impl_->stopSession();
}

void SessionController::selectTrackByRow(int row) {
    impl_->selectTrackByRow(row);
}

void SessionController::playPause() {
    impl_->playPause();
}

void SessionController::stopPlayback() {
    impl_->stopPlayback();
}

void SessionController::nextTrack() {
    impl_->nextTrack();
}

void SessionController::previousTrack() {
    impl_->previousTrack();
}

void SessionController::seekToPositionMs(qint64 positionMs) {
    impl_->seekToPositionMs(positionMs);
}

void SessionController::setVolumePercent(int volumePercent) {
    impl_->setVolumePercent(volumePercent);
}

void SessionController::setAutoplayEnabled(bool enabled) {
    impl_->setAutoplayEnabled(enabled);
}

void SessionController::setRepeatMode(RepeatMode mode) {
    impl_->setRepeatMode(mode);
}

void SessionController::kickClient(uint32_t clientId) {
    impl_->kickClient(clientId);
}

bool SessionController::isSessionActive() const {
    return impl_->isSessionActive();
}

bool SessionController::isHostMode() const {
    return impl_->isHostMode();
}

uint32_t SessionController::localClientId() const {
    return impl_->localClientId();
}

const std::vector<SessionController::TrackEntry>& SessionController::playlist() const {
    return impl_->playlist();
}

const std::vector<ClientDescriptor>& SessionController::clients() const {
    return impl_->clients();
}

const StateSyncPayload& SessionController::sessionState() const {
    return impl_->sessionState();
}

int SessionController::currentTrackIndex() const {
    return impl_->currentTrackIndex();
}

qint64 SessionController::currentPlaybackPositionMs() const {
    return impl_->currentPlaybackPositionMs();
}

qint64 SessionController::currentPlaybackDurationMs() const {
    return impl_->currentPlaybackDurationMs();
}

QString SessionController::currentTrackPath() const {
    return impl_->currentTrackPath();
}

QString SessionController::statusText() const {
    return impl_->statusText();
}
