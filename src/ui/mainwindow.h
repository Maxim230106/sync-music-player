#pragma once

#include <QMainWindow>
#include <QStringList>

class QAudioOutput;
class QListWidget;
class QListWidgetItem;
class QMediaPlayer;

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void configureUi();
    void connectSignals();
    void addTracks();
    void removeSelectedTrack();
    void playTrackAtRow(int row, bool autoplay = true);
    void playNextTrack();
    void playPreviousTrack();
    void togglePlayback();
    void updateTrackInfo(int row);
    void updatePosition(qint64 position);
    void updateDuration(qint64 duration);
    void updatePlaybackState();

    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audioOutput = nullptr;
    Ui::MainWindow* ui = nullptr;

    QStringList m_trackPaths;
    QString m_emptyTrackTitle;
    QString m_emptyTimeLabel;
    QString m_playButtonText;
    QString m_pauseButtonText;
    int m_currentTrackRow = -1;
};
