#include "mainwindow.h"

#include "ui_mainwindow.h"

#include <QAudioOutput>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidget>
#include <QMediaPlayer>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QUrl>
#include <algorithm>

namespace {

constexpr auto kDefaultPauseText = "Pause";

QString formatDuration(qint64 milliseconds)
{
    const qint64 totalSeconds = milliseconds / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_player(new QMediaPlayer(this))
    , m_audioOutput(new QAudioOutput(this))
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    m_player->setAudioOutput(m_audioOutput);

    configureUi();
    connectSignals();
    updatePlaybackState();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::configureUi()
{
    m_emptyTrackTitle = ui->trackTitleLabel->text();
    m_emptyTimeLabel = ui->timeLabel->text();
    m_playButtonText = ui->playPauseButton->text();
    if (m_playButtonText.isEmpty()) {
        m_playButtonText = "Play";
    }

    const QString pauseText = ui->playPauseButton->property("pauseText").toString();
    m_pauseButtonText = pauseText.isEmpty() ? QString::fromLatin1(kDefaultPauseText) : pauseText;

    if (ui->prevButton->icon().isNull()) {
        ui->prevButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    }
    if (ui->playPauseButton->icon().isNull()) {
        ui->playPauseButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    }
    if (ui->nextButton->icon().isNull()) {
        ui->nextButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    }

    const int volumeMax = std::max(1, ui->volumeSlider->maximum());
    m_audioOutput->setVolume(static_cast<float>(ui->volumeSlider->value()) / static_cast<float>(volumeMax));
}

void MainWindow::connectSignals()
{
    connect(ui->addButton, &QPushButton::clicked, this, &MainWindow::addTracks);
    connect(ui->removeButton, &QPushButton::clicked, this, &MainWindow::removeSelectedTrack);
    connect(ui->playlistWidget, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        playTrackAtRow(ui->playlistWidget->row(item));
    });
    connect(ui->playlistWidget, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_currentTrackRow < 0) {
            updateTrackInfo(row);
        }
        updatePlaybackState();
    });

    connect(ui->prevButton, &QPushButton::clicked, this, &MainWindow::playPreviousTrack);
    connect(ui->playPauseButton, &QPushButton::clicked, this, &MainWindow::togglePlayback);
    connect(ui->nextButton, &QPushButton::clicked, this, &MainWindow::playNextTrack);

    connect(ui->volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        const int volumeMax = std::max(1, ui->volumeSlider->maximum());
        m_audioOutput->setVolume(static_cast<float>(value) / static_cast<float>(volumeMax));
    });

    connect(ui->positionSlider, &QSlider::sliderMoved, this, [this](int position) {
        m_player->setPosition(position);
    });

    connect(m_player, &QMediaPlayer::positionChanged, this, &MainWindow::updatePosition);
    connect(m_player, &QMediaPlayer::durationChanged, this, &MainWindow::updateDuration);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this]() {
        updatePlaybackState();
    });
    connect(m_player, &QMediaPlayer::sourceChanged, this, [this]() {
        updatePlaybackState();
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            playNextTrack();
        }
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString& errorString) {
        statusBar()->showMessage(errorString, 5000);
        updatePlaybackState();
    });
}

void MainWindow::addTracks()
{
    const QStringList filePaths = QFileDialog::getOpenFileNames(
        this,
        "Select audio files",
        QString(),
        "Audio Files (*.mp3 *.wav *.ogg *.flac *.m4a);;All Files (*.*)");

    if (filePaths.isEmpty()) {
        return;
    }

    for (const QString& path : filePaths) {
        m_trackPaths.append(path);
        ui->playlistWidget->addItem(QFileInfo(path).fileName());
    }

    if (ui->playlistWidget->currentRow() < 0 && ui->playlistWidget->count() > 0) {
        ui->playlistWidget->setCurrentRow(0);
    }

    statusBar()->showMessage(QString("Tracks added: %1").arg(filePaths.size()), 3000);
    updatePlaybackState();
}

void MainWindow::removeSelectedTrack()
{
    const int row = ui->playlistWidget->currentRow();
    if (row < 0 || row >= m_trackPaths.size()) {
        return;
    }

    const bool removedCurrent = row == m_currentTrackRow;
    delete ui->playlistWidget->takeItem(row);
    m_trackPaths.removeAt(row);

    if (m_trackPaths.isEmpty()) {
        m_currentTrackRow = -1;
        m_player->stop();
        m_player->setSource(QUrl());
        updateTrackInfo(-1);
    } else {
        if (removedCurrent) {
            const int nextRow = qMin(row, m_trackPaths.size() - 1);
            m_currentTrackRow = -1;
            playTrackAtRow(nextRow, false);
        } else if (row < m_currentTrackRow) {
            --m_currentTrackRow;
        }

        if (ui->playlistWidget->currentRow() < 0) {
            ui->playlistWidget->setCurrentRow(qMin(row, ui->playlistWidget->count() - 1));
        }
    }

    updatePlaybackState();
}

void MainWindow::togglePlayback()
{
    if (m_trackPaths.isEmpty()) {
        return;
    }

    if (m_player->source().isEmpty()) {
        const int row = ui->playlistWidget->currentRow() >= 0 ? ui->playlistWidget->currentRow() : 0;
        playTrackAtRow(row);
        return;
    }

    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->pause();
    } else {
        m_player->play();
    }
}

void MainWindow::playPreviousTrack()
{
    if (m_trackPaths.isEmpty()) {
        return;
    }

    int row = m_currentTrackRow >= 0 ? m_currentTrackRow - 1 : ui->playlistWidget->currentRow() - 1;
    if (row < 0) {
        row = m_trackPaths.size() - 1;
    }

    playTrackAtRow(row);
}

void MainWindow::playNextTrack()
{
    if (m_trackPaths.isEmpty()) {
        return;
    }

    int row = m_currentTrackRow >= 0 ? m_currentTrackRow + 1 : ui->playlistWidget->currentRow() + 1;
    if (row >= m_trackPaths.size() || row < 0) {
        row = 0;
    }

    playTrackAtRow(row);
}

void MainWindow::playTrackAtRow(int row, bool autoplay)
{
    if (row < 0 || row >= m_trackPaths.size()) {
        return;
    }

    m_currentTrackRow = row;

    const QSignalBlocker blocker(ui->playlistWidget);
    ui->playlistWidget->setCurrentRow(row);

    m_player->setSource(QUrl::fromLocalFile(m_trackPaths.at(row)));
    updateTrackInfo(row);

    if (autoplay) {
        m_player->play();
    } else {
        m_player->pause();
    }

    updatePlaybackState();
}

void MainWindow::updateTrackInfo(int row)
{
    if (row < 0 || row >= m_trackPaths.size()) {
        ui->trackTitleLabel->setText(m_emptyTrackTitle);
        ui->timeLabel->setText(m_emptyTimeLabel);
        ui->positionSlider->setRange(0, 0);
        ui->positionSlider->setValue(0);
        return;
    }

    const QFileInfo fileInfo(m_trackPaths.at(row));
    ui->trackTitleLabel->setText(fileInfo.completeBaseName());

    if (m_player->source().isEmpty()) {
        ui->timeLabel->setText(m_emptyTimeLabel);
        ui->positionSlider->setRange(0, 0);
        ui->positionSlider->setValue(0);
    }
}

void MainWindow::updatePlaybackState()
{
    const bool hasTracks = !m_trackPaths.isEmpty();
    const bool hasSelection = ui->playlistWidget->currentRow() >= 0;
    const bool isPlaying = m_player->playbackState() == QMediaPlayer::PlayingState;

    ui->prevButton->setEnabled(hasTracks);
    ui->playPauseButton->setEnabled(hasTracks && hasSelection);
    ui->nextButton->setEnabled(hasTracks);
    ui->removeButton->setEnabled(hasSelection);
    ui->positionSlider->setEnabled(hasTracks && !m_player->source().isEmpty());

    ui->playPauseButton->setText(isPlaying ? m_pauseButtonText : m_playButtonText);
    ui->playPauseButton->setIcon(style()->standardIcon(
        isPlaying ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

void MainWindow::updatePosition(qint64 position)
{
    if (!ui->positionSlider->isSliderDown()) {
        ui->positionSlider->setValue(static_cast<int>(position));
    }

    const QString current = formatDuration(position);
    const QString total = formatDuration(m_player->duration());
    ui->timeLabel->setText(current + " / " + total);
}

void MainWindow::updateDuration(qint64 duration)
{
    ui->positionSlider->setRange(0, static_cast<int>(duration));
    updatePosition(m_player->position());
}
