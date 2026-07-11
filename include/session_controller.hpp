#pragma once

#include "protocol.hpp"

#include <QObject>
#include <QString>
#include <vector>

class SessionController : public QObject {
    Q_OBJECT

public:
    struct TrackEntry {
        TrackDescriptor descriptor;
        QString localPath;
        bool availableLocally = false;
    };

    explicit SessionController(QObject* parent = nullptr);
    ~SessionController() override;

    void setLocalLibrary(const std::vector<TrackEntry>& tracks);
    bool startHost(const QString& endpoint);
    bool connectToHost(const QString& endpoint);
    void stopSession();

    void selectTrackByRow(int row);
    void playPause();
    void stopPlayback();
    void nextTrack();
    void previousTrack();
    void seekToPositionMs(qint64 positionMs);
    void setVolumePercent(int volumePercent);
    void setAutoplayEnabled(bool enabled);
    void setRepeatMode(RepeatMode mode);
    void kickClient(uint32_t clientId);

    bool isSessionActive() const;
    bool isHostMode() const;
    uint32_t localClientId() const;
    const std::vector<TrackEntry>& playlist() const;
    const std::vector<ClientDescriptor>& clients() const;
    const StateSyncPayload& sessionState() const;
    int currentTrackIndex() const;
    qint64 currentPlaybackPositionMs() const;
    qint64 currentPlaybackDurationMs() const;
    QString currentTrackPath() const;
    QString statusText() const;

signals:
    void sessionChanged();
    void statusMessageChanged(const QString& message);

private:
    class Impl;
    Impl* impl_ = nullptr;
};
