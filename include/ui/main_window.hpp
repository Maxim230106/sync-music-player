#pragma once

#include "session_controller.hpp"

#include <QMainWindow>
#include <QPointer>

#include <vector>

class QComboBox;
class QEvent;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    enum class MusicDirectoryMode {
        Auto,
        ExecutableDirectory,
        CurrentWorkingDirectory
    };

    explicit MainWindow(
        MusicDirectoryMode musicDirectoryMode = MusicDirectoryMode::Auto,
        QWidget* parent = nullptr
    );

private:
    enum class RepeatMode {
        Queue,
        Track,
        Playlist
    };

    bool shouldUseDarkTheme() const;
    void toggleRole();
    void buildUi();
    void applyPalette();
    void handleSessionAction();
    void importMusicFiles();
    void populateAddressChoices();
    void refreshLibrary();
    void refreshTheme();
    void removeTrackByRow(int row);
    void toggleAutoplay();
    void cycleRepeatMode();
    void updateModeUi();
    void updateLibraryActionButtons();
    void updatePlaybackOptionUi();
    void syncUiFromSession();
    void rebuildLibraryList();
    void rebuildClientList();
    void syncLibraryToSession();
    void handleLibrarySelectionChange(QListWidgetItem* item);
    void clearTrackPreview();
    qint64 probeTrackDuration(const QString& path) const;
    QString discoverAssetPath(const QString& fileName) const;
    QString discoverTransportIconPath(const QString& baseName) const;
    void updateEndpointComboChevronUi();
    QString discoverMusicDirectory() const;
    QString ensureMusicDirectory() const;
    QString discoverLogoPath() const;
    QString endpointWithDefaultPort(const QString& endpoint) const;
    QString formatDuration(qint64 durationMs) const;
    void updateTransportButtonUi();
    bool eventFilter(QObject* watched, QEvent* event) override;

    static constexpr int kDefaultPort = 54000;

    QLabel* logoLabel_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* subtitleLabel_ = nullptr;
    QPushButton* roleToggleButton_ = nullptr;
    QComboBox* endpointCombo_ = nullptr;
    QLabel* endpointComboChevronLabel_ = nullptr;
    QPushButton* sessionActionButton_ = nullptr;
    QLabel* modeBadgeLabel_ = nullptr;
    QLabel* libraryPathLabel_ = nullptr;
    QListWidget* libraryList_ = nullptr;
    QPushButton* refreshLibraryButton_ = nullptr;
    QPushButton* importMusicButton_ = nullptr;
    QLabel* currentTrackLabel_ = nullptr;
    QLabel* currentTrackMetaLabel_ = nullptr;
    QLabel* currentTimeLabel_ = nullptr;
    QLabel* totalTimeLabel_ = nullptr;
    QSlider* seekSlider_ = nullptr;
    QPushButton* previousButton_ = nullptr;
    QPushButton* playPauseButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* autoplayButton_ = nullptr;
    QPushButton* repeatModeButton_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QLabel* volumeValueLabel_ = nullptr;
    QListWidget* clientList_ = nullptr;
    QLabel* sessionHintLabel_ = nullptr;
    SessionController* session_ = nullptr;

    QString musicDirectory_;
    MusicDirectoryMode musicDirectoryMode_ = MusicDirectoryMode::Auto;
    bool hostMode_ = true;
    bool sessionActive_ = false;
    bool useDarkTheme_ = true;
    bool autoplayEnabled_ = true;
    RepeatMode repeatMode_ = RepeatMode::Queue;
    bool seekInteractionActive_ = false;
    int pendingSeekPositionSeconds_ = -1;
    std::vector<SessionController::TrackEntry> localLibrary_;
    std::vector<QPointer<QWidget>> hostOnlyWidgets_;
};
