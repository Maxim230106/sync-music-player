#include "ui/main_window.hpp"

#include <QAbstractItemView>
#include <QAbstractSocket>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFrame>
#include <QMediaPlayer>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QGuiApplication>
#include <QSignalBlocker>
#include <QSet>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStyleHints>
#include <QStringList>
#include <QPoint>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace {

constexpr const char* kAppDisplayName = "Sync Music Player v" APP_VERSION;

class JumpDragSlider : public QSlider {
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QSlider::mousePressEvent(event);
            return;
        }

        QStyleOptionSlider option;
        initStyleOption(&option);

        const QRect handleRect = style()->subControlRect(
            QStyle::CC_Slider,
            &option,
            QStyle::SC_SliderHandle,
            this
        );

        if (handleRect.contains(event->position().toPoint())) {
            dragFromAnywhere_ = false;
            QSlider::mousePressEvent(event);
            return;
        }

        // On Linux styles, clicking the groove usually triggers page-step logic.
        // We force the Windows-like behavior: jump to the clicked position and
        // immediately continue dragging until the mouse button is released.
        dragFromAnywhere_ = true;
        setSliderDown(true);
        updateFromPosition(event->position().toPoint());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragFromAnywhere_ && (event->buttons() & Qt::LeftButton)) {
            updateFromPosition(event->position().toPoint());
            event->accept();
            return;
        }

        QSlider::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragFromAnywhere_ && event->button() == Qt::LeftButton) {
            updateFromPosition(event->position().toPoint());
            dragFromAnywhere_ = false;
            setSliderDown(false);
            event->accept();
            return;
        }

        dragFromAnywhere_ = false;
        QSlider::mouseReleaseEvent(event);
    }

private:
    void updateFromPosition(const QPoint& point) {
        QStyleOptionSlider option;
        initStyleOption(&option);

        const QRect handleRect = style()->subControlRect(
            QStyle::CC_Slider,
            &option,
            QStyle::SC_SliderHandle,
            this
        );
        const QRect grooveRect = style()->subControlRect(
            QStyle::CC_Slider,
            &option,
            QStyle::SC_SliderGroove,
            this
        );

        const bool horizontal = orientation() == Qt::Horizontal;
        const int sliderLength = horizontal ? handleRect.width() : handleRect.height();
        const int sliderMin = horizontal ? grooveRect.x() : grooveRect.y();
        const int sliderMax = horizontal
            ? grooveRect.right() - sliderLength + 1
            : grooveRect.bottom() - sliderLength + 1;
        const int sliderPosition = horizontal
            ? point.x() - (sliderLength / 2)
            : point.y() - (sliderLength / 2);
        const int available = std::max(1, sliderMax - sliderMin);

        const int value = QStyle::sliderValueFromPosition(
            minimum(),
            maximum(),
            std::clamp(sliderPosition - sliderMin, 0, available),
            available,
            option.upsideDown
        );

        setSliderPosition(value);
        setValue(value);
    }

    bool dragFromAnywhere_ = false;
};

QStringList musicNameFilters() {
    return {
        "*.mp3",
        "*.wav",
        "*.ogg",
        "*.flac",
        "*.aac",
        "*.m4a"
    };
}

qint64 probeTrackDurationWithQt(const QString& path) {
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return 0;
    }

    QMediaPlayer player;
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);

    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&player, &QMediaPlayer::durationChanged, &loop, [&](qint64 duration) {
        if (duration >= 0) {
            loop.quit();
        }
    });
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &loop, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia ||
            status == QMediaPlayer::BufferedMedia ||
            status == QMediaPlayer::InvalidMedia ||
            status == QMediaPlayer::NoMedia) {
            loop.quit();
        }
    });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &loop, [&](QMediaPlayer::Error, const QString&) {
        loop.quit();
    });

    player.setSource(QUrl::fromLocalFile(path));
    timeoutTimer.start(1500);
    loop.exec();

    return std::max<qint64>(0, player.duration());
}

} // namespace

MainWindow::MainWindow(MusicDirectoryMode musicDirectoryMode, QWidget* parent)
    : QMainWindow(parent)
    , musicDirectoryMode_(musicDirectoryMode) {
    buildUi();
    session_ = new SessionController(this);

    connect(session_, &SessionController::sessionChanged, this, &MainWindow::syncUiFromSession);
    connect(session_, &SessionController::statusMessageChanged, this, [this](const QString& message) {
        statusBar()->showMessage(message);
        sessionActive_ = session_->isSessionActive();
        updateModeUi();
    });

    refreshTheme();
    applyPalette();
    populateAddressChoices();
    refreshLibrary();
    syncUiFromSession();
    updateModeUi();

    resize(1360, 840);
    setMinimumSize(1120, 720);
    setWindowTitle(kAppDisplayName);
    const QString logoPath = discoverLogoPath();
    if (!logoPath.isEmpty()) {
        setWindowIcon(QIcon(logoPath));
    }
    statusBar()->showMessage("Session controller is ready. Start host or connect as client.");

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(
        qGuiApp->styleHints(),
        &QStyleHints::colorSchemeChanged,
        this,
        [this](Qt::ColorScheme) {
            refreshTheme();
            applyPalette();
            updateModeUi();
        }
    );
#else
    connect(qGuiApp, &QGuiApplication::paletteChanged, this, [this](const QPalette&) {
        refreshTheme();
        applyPalette();
        updateModeUi();
    });
#endif
}

bool MainWindow::shouldUseDarkTheme() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme scheme = qGuiApp->styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Light) {
        return false;
    }

    if (scheme == Qt::ColorScheme::Dark) {
        return true;
    }
#else
    const QColor windowColor = qGuiApp->palette().color(QPalette::Window);
    if (windowColor.isValid()) {
        return windowColor.lightness() < 128;
    }
#endif

    return true;
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(14);

    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(16);

    auto* brandingCard = new QFrame(central);
    brandingCard->setObjectName("heroCard");
    auto* brandingLayout = new QHBoxLayout(brandingCard);
    brandingLayout->setContentsMargins(18, 18, 18, 18);
    brandingLayout->setSpacing(16);

    logoLabel_ = new QLabel(brandingCard);
    logoLabel_->setObjectName("logoLabel");
    logoLabel_->setFixedSize(72, 72);
    logoLabel_->setAlignment(Qt::AlignCenter);

    const QString logoPath = discoverLogoPath();
    QPixmap logoPixmap(logoPath);
    if (!logoPixmap.isNull()) {
        logoLabel_->setPixmap(logoPixmap.scaled(logoLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    else {
        logoLabel_->setText("SMP");
    }

    auto* titleBox = new QVBoxLayout();
    titleBox->setSpacing(4);

    titleLabel_ = new QLabel(kAppDisplayName, brandingCard);
    titleLabel_->setObjectName("heroTitle");

    subtitleLabel_ = new QLabel(
        "A program for simultaneous music listening by multiple participants on a local network.\n"
        "Shared session dashboard for host and listeners in one window.\n"
        "Made by Maxim230106 and K0SHAKk.",
        brandingCard
    );
    subtitleLabel_->setObjectName("heroSubtitle");
    subtitleLabel_->setWordWrap(true);

    titleBox->addWidget(titleLabel_);
    titleBox->addWidget(subtitleLabel_);
    titleBox->addStretch(1);

    brandingLayout->addWidget(logoLabel_, 0, Qt::AlignTop);
    brandingLayout->addLayout(titleBox, 1);

    auto* sessionCard = new QFrame(central);
    sessionCard->setObjectName("sessionCard");
    sessionCard->setMinimumWidth(420);
    auto* topLayout = new QGridLayout(sessionCard);
    topLayout->setContentsMargins(18, 16, 18, 16);
    topLayout->setHorizontalSpacing(12);
    topLayout->setVerticalSpacing(10);

    auto* sessionTitleLabel = new QLabel("Session setup", sessionCard);
    sessionTitleLabel->setObjectName("sectionTitle");

    roleToggleButton_ = new QPushButton(sessionCard);
    roleToggleButton_->setObjectName("secondaryButton");

    modeBadgeLabel_ = new QLabel(sessionCard);
    modeBadgeLabel_->setObjectName("modeBadge");
    modeBadgeLabel_->setAlignment(Qt::AlignCenter);

    endpointCombo_ = new QComboBox(sessionCard);
    endpointCombo_->setEditable(true);
    endpointCombo_->setInsertPolicy(QComboBox::NoInsert);
    endpointCombo_->setMinimumContentsLength(24);

    sessionActionButton_ = new QPushButton(sessionCard);
    sessionActionButton_->setObjectName("primaryButton");

    auto* endpointHelpLabel = new QLabel(
        "Endpoint accepts ip:port. If port is omitted, 54000 is used. Use 0.0.0.0 to listen on all local interfaces.",
        sessionCard
    );
    endpointHelpLabel->setWordWrap(true);
    endpointHelpLabel->setObjectName("mutedLabel");

    sessionHintLabel_ = new QLabel(
        "All peers are assumed to be in the same local network for this preview.",
        sessionCard
    );
    sessionHintLabel_->setObjectName("mutedLabel");
    sessionHintLabel_->setWordWrap(true);

    topLayout->addWidget(sessionTitleLabel, 0, 0);
    topLayout->addWidget(roleToggleButton_, 0, 1);
    topLayout->addWidget(modeBadgeLabel_, 0, 2);
    topLayout->addWidget(endpointCombo_, 1, 0, 1, 2);
    topLayout->addWidget(sessionActionButton_, 1, 2);
    topLayout->addWidget(endpointHelpLabel, 2, 0, 1, 3);
    topLayout->addWidget(sessionHintLabel_, 3, 0, 1, 3);

    headerLayout->addWidget(brandingCard, 1);
    headerLayout->addWidget(sessionCard, 0);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(14);

    auto* libraryGroup = new QFrame(central);
    libraryGroup->setObjectName("panelCard");
    libraryGroup->setMinimumWidth(300);
    auto* libraryLayout = new QVBoxLayout(libraryGroup);
    libraryLayout->setContentsMargins(16, 16, 16, 16);
    libraryLayout->setSpacing(10);

    auto* libraryTitle = new QLabel("Music library", libraryGroup);
    libraryTitle->setObjectName("panelTitle");

    libraryPathLabel_ = new QLabel(libraryGroup);
    libraryPathLabel_->setObjectName("mutedLabel");
    libraryPathLabel_->setWordWrap(true);

    refreshLibraryButton_ = new QPushButton("Refresh folder", libraryGroup);

    libraryList_ = new QListWidget(libraryGroup);
    libraryList_->setSelectionMode(QAbstractItemView::SingleSelection);
    libraryList_->setAlternatingRowColors(true);
    libraryList_->setSpacing(4);

    libraryLayout->addWidget(libraryTitle);
    libraryLayout->addWidget(libraryPathLabel_);
    libraryLayout->addWidget(refreshLibraryButton_);
    libraryLayout->addWidget(libraryList_, 1);

    auto* centerColumn = new QWidget(central);
    auto* centerLayout = new QVBoxLayout(centerColumn);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(14);

    auto* nowPlayingGroup = new QFrame(centerColumn);
    nowPlayingGroup->setObjectName("panelCard");
    auto* nowPlayingLayout = new QVBoxLayout(nowPlayingGroup);
    nowPlayingLayout->setContentsMargins(18, 18, 18, 18);
    nowPlayingLayout->setSpacing(12);

    auto* nowPlayingTitle = new QLabel("Now playing", nowPlayingGroup);
    nowPlayingTitle->setObjectName("panelTitle");

    currentTrackLabel_ = new QLabel("No track selected", nowPlayingGroup);
    currentTrackLabel_->setObjectName("trackTitle");

    currentTrackMetaLabel_ = new QLabel(
        "Pick a file from the music list to preview the title area.",
        nowPlayingGroup
    );
    currentTrackMetaLabel_->setObjectName("mutedLabel");
    currentTrackMetaLabel_->setWordWrap(true);

    seekSlider_ = new JumpDragSlider(Qt::Horizontal, nowPlayingGroup);
    seekSlider_->setRange(0, 0);
    seekSlider_->setValue(0);

    auto* timeRow = new QHBoxLayout();
    currentTimeLabel_ = new QLabel("--:--", nowPlayingGroup);
    totalTimeLabel_ = new QLabel("--:--", nowPlayingGroup);
    totalTimeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timeRow->addWidget(currentTimeLabel_);
    timeRow->addStretch(1);
    timeRow->addWidget(totalTimeLabel_);

    auto* transportRow = new QHBoxLayout();
    transportRow->setSpacing(8);
    previousButton_ = new QPushButton("Previous", nowPlayingGroup);
    playPauseButton_ = new QPushButton("Play", nowPlayingGroup);
    stopButton_ = new QPushButton("Stop", nowPlayingGroup);
    nextButton_ = new QPushButton("Next", nowPlayingGroup);
    previousButton_->setText("Prev");
    for (QPushButton* button : {previousButton_, playPauseButton_, stopButton_, nextButton_}) {
        button->setFixedWidth(84);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    transportRow->addStretch(1);
    transportRow->addWidget(previousButton_);
    transportRow->addWidget(playPauseButton_);
    transportRow->addWidget(stopButton_);
    transportRow->addWidget(nextButton_);
    transportRow->addStretch(1);

    auto* playbackOptionsRow = new QHBoxLayout();
    playbackOptionsRow->setSpacing(8);
    autoplayButton_ = new QPushButton(nowPlayingGroup);
    autoplayButton_->setObjectName("stateButton");
    repeatModeButton_ = new QPushButton(nowPlayingGroup);
    repeatModeButton_->setObjectName("stateButton");
    playbackOptionsRow->addStretch(1);
    playbackOptionsRow->addWidget(autoplayButton_);
    playbackOptionsRow->addWidget(repeatModeButton_);
    playbackOptionsRow->addStretch(1);

    auto* volumeCard = new QWidget(nowPlayingGroup);
    volumeCard->setObjectName("volumeCard");
    auto* volumeLayout = new QHBoxLayout(volumeCard);
    volumeLayout->setContentsMargins(12, 10, 12, 10);
    volumeLayout->setSpacing(10);

    auto* volumeLabel = new QLabel("Host volume", volumeCard);
    volumeSlider_ = new JumpDragSlider(Qt::Horizontal, volumeCard);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(50);
    volumeValueLabel_ = new QLabel("50%", volumeCard);
    volumeValueLabel_->setMinimumWidth(52);
    volumeValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    volumeLayout->addWidget(volumeLabel);
    volumeLayout->addWidget(volumeSlider_, 1);
    volumeLayout->addWidget(volumeValueLabel_);

    nowPlayingLayout->addWidget(nowPlayingTitle);
    nowPlayingLayout->addWidget(currentTrackLabel_);
    nowPlayingLayout->addWidget(currentTrackMetaLabel_);
    nowPlayingLayout->addWidget(seekSlider_);
    nowPlayingLayout->addLayout(timeRow);
    nowPlayingLayout->addLayout(transportRow);
    nowPlayingLayout->addLayout(playbackOptionsRow);
    nowPlayingLayout->addWidget(volumeCard);

    auto* notesGroup = new QFrame(centerColumn);
    notesGroup->setObjectName("panelCard");
    auto* notesLayout = new QVBoxLayout(notesGroup);
    notesLayout->setContentsMargins(18, 18, 18, 18);
    notesLayout->setSpacing(8);

    auto* notesTitle = new QLabel("Session notes", notesGroup);
    notesTitle->setObjectName("panelTitle");

    auto* notesLabel = new QLabel(
        "One window is shared by both roles.\n"
        "User mode keeps the same layout visible but disables host-only actions.\n"
        "This gives us a stable surface for the network protocol.",
        notesGroup
    );
    notesLabel->setWordWrap(true);
    notesLabel->setObjectName("bodyText");
    notesLayout->addWidget(notesTitle);
    notesLayout->addWidget(notesLabel);

    centerLayout->addWidget(nowPlayingGroup, 1);
    centerLayout->addWidget(notesGroup);

    auto* clientsGroup = new QFrame(central);
    clientsGroup->setObjectName("panelCard");
    clientsGroup->setMinimumWidth(300);
    auto* clientsLayout = new QVBoxLayout(clientsGroup);
    clientsLayout->setContentsMargins(16, 16, 16, 16);
    clientsLayout->setSpacing(10);

    auto* clientsTitle = new QLabel("Connected users", clientsGroup);
    clientsTitle->setObjectName("panelTitle");

    auto* clientsHelp = new QLabel(
        "Preview rows include a kick button to show how moderation controls may look.",
        clientsGroup
    );
    clientsHelp->setWordWrap(true);
    clientsHelp->setObjectName("mutedLabel");

    clientList_ = new QListWidget(clientsGroup);
    clientList_->setSpacing(6);
    clientList_->setSelectionMode(QAbstractItemView::NoSelection);

    clientsLayout->addWidget(clientsTitle);
    clientsLayout->addWidget(clientsHelp);
    clientsLayout->addWidget(clientList_, 1);

    contentLayout->addWidget(libraryGroup);
    contentLayout->addWidget(centerColumn, 1);
    contentLayout->addWidget(clientsGroup);

    rootLayout->addLayout(headerLayout);
    rootLayout->addLayout(contentLayout, 1);

    setCentralWidget(central);

    hostOnlyWidgets_ = {
        refreshLibraryButton_,
        seekSlider_,
        previousButton_,
        playPauseButton_,
        stopButton_,
        nextButton_,
        autoplayButton_,
        repeatModeButton_,
        volumeSlider_
    };

    connect(roleToggleButton_, &QPushButton::clicked, this, &MainWindow::toggleRole);
    connect(sessionActionButton_, &QPushButton::clicked, this, &MainWindow::handleSessionAction);
    connect(autoplayButton_, &QPushButton::clicked, this, &MainWindow::toggleAutoplay);
    connect(repeatModeButton_, &QPushButton::clicked, this, &MainWindow::cycleRepeatMode);
    connect(refreshLibraryButton_, &QPushButton::clicked, this, &MainWindow::refreshLibrary);
    connect(libraryList_, &QListWidget::itemClicked, this, &MainWindow::handleLibrarySelectionChange);
    connect(endpointCombo_, &QComboBox::currentTextChanged, this, [this](const QString&) {
        updateModeUi();
    });
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        volumeValueLabel_->setText(QString::number(value) + "%");
        if (hostMode_) {
            session_->setVolumePercent(value);
        }
    });
    connect(playPauseButton_, &QPushButton::clicked, this, [this]() {
        if (hostMode_) {
            session_->playPause();
        }
    });
    connect(stopButton_, &QPushButton::clicked, this, [this]() {
        if (hostMode_) {
            session_->stopPlayback();
        }
    });
    connect(previousButton_, &QPushButton::clicked, this, [this]() {
        if (hostMode_) {
            session_->previousTrack();
        }
    });
    connect(nextButton_, &QPushButton::clicked, this, [this]() {
        if (hostMode_) {
            session_->nextTrack();
        }
    });
    connect(seekSlider_, &QSlider::sliderPressed, this, [this]() {
        seekInteractionActive_ = true;
        pendingSeekPositionSeconds_ = seekSlider_->sliderPosition();
        currentTimeLabel_->setText(formatDuration(static_cast<qint64>(pendingSeekPositionSeconds_) * 1000));
    });
    connect(seekSlider_, &QSlider::sliderMoved, this, [this](int value) {
        pendingSeekPositionSeconds_ = value;
        currentTimeLabel_->setText(formatDuration(static_cast<qint64>(value) * 1000));
    });
    connect(seekSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (seekInteractionActive_) {
            pendingSeekPositionSeconds_ = value;
            currentTimeLabel_->setText(formatDuration(static_cast<qint64>(value) * 1000));
        }
    });
    connect(seekSlider_, &QSlider::sliderReleased, this, [this]() {
        const int targetSeconds = pendingSeekPositionSeconds_ >= 0
            ? pendingSeekPositionSeconds_
            : seekSlider_->sliderPosition();
        if (hostMode_) {
            session_->seekToPositionMs(static_cast<qint64>(targetSeconds) * 1000);
        }
        seekInteractionActive_ = false;
        pendingSeekPositionSeconds_ = -1;
    });
}

void MainWindow::applyPalette() {
    const QString arrowAsset = discoverAssetPath(
        useDarkTheme_ ? "chevron-down-light.svg" : "chevron-down-dark.svg"
    );

    if (useDarkTheme_) {
        setStyleSheet(QString(
            "QMainWindow { background: #111827; }"
            "QWidget { color: #f3f4f6; }"
            "QFrame#heroCard, QFrame#sessionCard, QFrame#panelCard {"
            "  background: #1f2937;"
            "  border: 1px solid #374151;"
            "  border-radius: 16px;"
            "}"
            "QFrame#volumeCard {"
            "  background: transparent;"
            "  border: none;"
            "}"
            "QLabel#logoLabel {"
            "  background: #0f172a;"
            "  border: 1px solid #374151;"
            "  border-radius: 14px;"
            "  color: #f3f4f6;"
            "  font-size: 18px;"
            "  font-weight: 700;"
            "}"
            "QLabel#heroTitle {"
            "  font-size: 22px;"
            "  font-weight: 700;"
            "  color: #f9fafb;"
            "}"
            "QLabel#heroSubtitle {"
            "  color: #9ca3af;"
            "  font-size: 10.5pt;"
            "}"
            "QLabel#sectionTitle {"
            "  font-size: 14pt;"
            "  font-weight: 700;"
            "  color: #f9fafb;"
            "}"
            "QLabel#panelTitle {"
            "  font-size: 12pt;"
            "  font-weight: 600;"
            "  color: #f9fafb;"
            "}"
            "QLabel#trackTitle {"
            "  font-size: 30px;"
            "  font-weight: 700;"
            "  color: #ffffff;"
            "}"
            "QLabel#mutedLabel {"
            "  color: #9ca3af;"
            "}"
            "QLabel#bodyText {"
            "  color: #d1d5db;"
            "}"
            "QLabel#modeBadge {"
            "  background: #2563eb;"
            "  color: white;"
            "  border: none;"
            "  border-radius: 11px;"
            "  padding: 5px 10px;"
            "  font-weight: 700;"
            "}"
            "QPushButton {"
            "  background: #1f2937;"
            "  color: #f3f4f6;"
            "  border: 1px solid #4b5563;"
            "  border-radius: 12px;"
            "  padding: 8px 14px;"
            "}"
            "QPushButton#secondaryButton {"
            "  background: #111827;"
            "}"
            "QPushButton#stateButton {"
            "  min-width: 124px;"
            "}"
            "QPushButton:hover { background: #273244; }"
            "QPushButton:pressed { background: #111827; }"
            "QPushButton:disabled {"
            "  background: #1b2432;"
            "  color: #6b7280;"
            "  border-color: #374151;"
            "}"
            "QPushButton#primaryButton {"
            "  background: #2563eb;"
            "  color: white;"
            "  border-color: #2563eb;"
            "  font-weight: 700;"
            "}"
            "QPushButton#primaryButton:hover { background: #1d4ed8; }"
            "QPushButton#primaryButton:disabled {"
            "  background: #475569;"
            "  color: #e5e7eb;"
            "  border-color: #475569;"
            "}"
            "QListWidget {"
            "  background: #111827;"
            "  border: 1px solid #374151;"
            "  border-radius: 14px;"
            "  padding: 6px;"
            "  color: #f3f4f6;"
            "}"
            "QListWidget::item {"
            "  border-radius: 10px;"
            "  padding: 10px 8px;"
            "}"
            "QListWidget::item:selected {"
            "  background: #243041;"
            "  color: #ffffff;"
            "}"
            "QListWidget:disabled {"
            "  background: #161f2d;"
            "  color: #6b7280;"
            "  border-color: #2a3442;"
            "}"
            "QComboBox, QLineEdit {"
            "  background: #111827;"
            "  color: #f3f4f6;"
            "  border: 1px solid #4b5563;"
            "  border-radius: 12px;"
            "  padding: 8px 34px 8px 10px;"
            "}"
            "QComboBox::drop-down {"
            "  subcontrol-origin: padding;"
            "  subcontrol-position: top right;"
            "  width: 28px;"
            "  border-left: 1px solid #4b5563;"
            "  border-top-right-radius: 12px;"
            "  border-bottom-right-radius: 12px;"
            "  background: #1f2937;"
            "}"
            "QComboBox::down-arrow {"
            "  image: url(%1);"
            "  width: 12px;"
            "  height: 12px;"
            "}"
            "QComboBox:disabled, QLineEdit:disabled {"
            "  background: #1b2432;"
            "  color: #6b7280;"
            "}"
            "QSlider { background: transparent; }"
            "QSlider::groove:horizontal {"
            "  height: 8px;"
            "  background: #374151;"
            "  border-radius: 4px;"
            "}"
            "QSlider::sub-page:horizontal {"
            "  background: #2563eb;"
            "  border-radius: 4px;"
            "}"
            "QSlider::handle:horizontal {"
            "  width: 18px;"
            "  margin: -5px 0;"
            "  border-radius: 9px;"
            "  background: #f9fafb;"
            "  border: 1px solid #94a3b8;"
            "}"
            "QSlider:disabled::groove:horizontal {"
            "  background: #273244;"
            "}"
            "QSlider:disabled::sub-page:horizontal {"
            "  background: #475569;"
            "}"
            "QToolButton {"
            "  background: #1f2937;"
            "  border: 1px solid #4b5563;"
            "  color: #f87171;"
            "  border-radius: 10px;"
            "  padding: 4px 8px;"
            "  font-weight: 700;"
            "}"
            "QToolButton:disabled {"
            "  background: #1b2432;"
            "  color: #6b7280;"
            "  border-color: #374151;"
            "}"
            "QStatusBar {"
            "  background: #0f172a;"
            "  color: #cbd5e1;"
            "  border-top: 1px solid #374151;"
            "}"
        ).arg(arrowAsset));
        return;
    }

    setStyleSheet(QString(
        "QMainWindow { background: #f3f5f7; }"
        "QWidget { color: #111111; }"
        "QFrame#heroCard, QFrame#sessionCard, QFrame#panelCard {"
        "  background: #ffffff;"
        "  border: 1px solid #d8d8d8;"
        "  border-radius: 16px;"
        "}"
        "QFrame#volumeCard {"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#logoLabel {"
        "  background: #f6f7f9;"
        "  border: 1px solid #d8d8d8;"
        "  border-radius: 14px;"
        "  color: #111111;"
        "  font-size: 18px;"
        "  font-weight: 700;"
        "}"
        "QLabel#heroTitle {"
        "  font-size: 22px;"
        "  font-weight: 700;"
        "  color: #111111;"
        "}"
        "QLabel#heroSubtitle {"
        "  color: #666666;"
        "  font-size: 10.5pt;"
        "}"
        "QLabel#sectionTitle {"
        "  font-size: 14pt;"
        "  font-weight: 700;"
        "  color: #111111;"
        "}"
        "QLabel#panelTitle {"
        "  font-size: 12pt;"
        "  font-weight: 600;"
        "  color: #111111;"
        "}"
        "QLabel#trackTitle {"
        "  font-size: 30px;"
        "  font-weight: 700;"
        "  color: #000000;"
        "}"
        "QLabel#mutedLabel {"
        "  color: #6b7280;"
        "}"
        "QLabel#bodyText {"
        "  color: #222222;"
        "}"
        "QLabel#modeBadge {"
        "  background: #1f4e79;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 11px;"
        "  padding: 5px 10px;"
        "  font-weight: 700;"
        "}"
        "QPushButton {"
        "  background: #ffffff;"
        "  color: #111111;"
        "  border: 1px solid #d4d4d4;"
        "  border-radius: 12px;"
        "  padding: 8px 14px;"
        "}"
        "QPushButton#secondaryButton {"
        "  background: #f8fafc;"
        "}"
        "QPushButton#stateButton {"
        "  min-width: 124px;"
        "}"
        "QPushButton:hover { background: #f7f7f7; }"
        "QPushButton:pressed { background: #efefef; }"
        "QPushButton:disabled {"
        "  background: #f2f3f5;"
        "  color: #9ca3af;"
        "  border-color: #e5e7eb;"
        "}"
        "QPushButton#primaryButton {"
        "  background: #111827;"
        "  color: white;"
        "  border-color: #111827;"
        "  font-weight: 700;"
        "}"
        "QPushButton#primaryButton:hover { background: #1f2937; }"
        "QPushButton#primaryButton:disabled {"
        "  background: #9ca3af;"
        "  color: white;"
        "  border-color: #9ca3af;"
        "}"
        "QListWidget {"
        "  background: #ffffff;"
        "  border: 1px solid #d8d8d8;"
        "  border-radius: 14px;"
        "  padding: 6px;"
        "  color: #111111;"
        "}"
        "QListWidget::item {"
        "  border-radius: 10px;"
        "  padding: 10px 8px;"
        "}"
        "QListWidget::item:selected {"
        "  background: #eef3f8;"
        "  color: #111111;"
        "}"
        "QListWidget:disabled {"
        "  background: #f8f9fb;"
        "  color: #9ca3af;"
        "  border-color: #e5e7eb;"
        "}"
        "QComboBox, QLineEdit {"
        "  background: #ffffff;"
        "  color: #111111;"
        "  border: 1px solid #d1d5db;"
        "  border-radius: 12px;"
        "  padding: 8px 34px 8px 10px;"
        "}"
        "QComboBox::drop-down {"
        "  subcontrol-origin: padding;"
        "  subcontrol-position: top right;"
        "  width: 28px;"
        "  border-left: 1px solid #d1d5db;"
        "  border-top-right-radius: 12px;"
        "  border-bottom-right-radius: 12px;"
        "  background: #f8fafc;"
        "}"
        "QComboBox::down-arrow {"
        "  image: url(%1);"
        "  width: 12px;"
        "  height: 12px;"
        "}"
        "QComboBox:disabled, QLineEdit:disabled {"
        "  background: #f2f3f5;"
        "  color: #9ca3af;"
        "}"
        "QSlider { background: transparent; }"
        "QSlider::groove:horizontal {"
            "  height: 8px;"
            "  background: #e5e7eb;"
            "  border-radius: 4px;"
        "}"
        "QSlider::sub-page:horizontal {"
            "  background: #1f4e79;"
            "  border-radius: 4px;"
        "}"
        "QSlider::handle:horizontal {"
        "  width: 18px;"
        "  margin: -5px 0;"
        "  border-radius: 9px;"
        "  background: #ffffff;"
        "  border: 1px solid #9ca3af;"
        "}"
        "QSlider:disabled::groove:horizontal {"
        "  background: #eceff3;"
        "}"
        "QSlider:disabled::sub-page:horizontal {"
            "  background: #cbd5e1;"
        "}"
        "QToolButton {"
        "  background: #ffffff;"
        "  border: 1px solid #d4d4d4;"
        "  color: #b42318;"
        "  border-radius: 10px;"
        "  padding: 4px 8px;"
        "  font-weight: 700;"
        "}"
        "QToolButton:disabled {"
        "  background: #f2f3f5;"
        "  color: #c4c4c4;"
        "  border-color: #e5e7eb;"
        "}"
        "QStatusBar {"
        "  background: #ffffff;"
        "  color: #4b5563;"
        "  border-top: 1px solid #d8d8d8;"
        "}"
    ).arg(arrowAsset));
}

void MainWindow::handleSessionAction() {
    if (sessionActive_) {
        session_->stopSession();
        sessionActive_ = false;
        updateModeUi();
        return;
    }

    if (hostMode_) {
        syncLibraryToSession();
        if (!session_->startHost(endpointWithDefaultPort(endpointCombo_->currentText()))) {
            statusBar()->showMessage("Failed to start host session.");
            return;
        }
    }
    else if (!session_->connectToHost(endpointWithDefaultPort(endpointCombo_->currentText()))) {
        statusBar()->showMessage("Failed to connect to host.");
        return;
    }

    sessionActive_ = session_->isSessionActive();
    updateModeUi();
}

void MainWindow::populateAddressChoices() {
    QSet<QString> uniqueEndpoints;
    const QString wildcard = QString("0.0.0.0:%1").arg(kDefaultPort);
    const QString loopback = QString("127.0.0.1:%1").arg(kDefaultPort);

    uniqueEndpoints.insert(wildcard);
    uniqueEndpoints.insert(loopback);

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)) {
            continue;
        }

        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const auto ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }

            uniqueEndpoints.insert(QString("%1:%2").arg(ip.toString()).arg(kDefaultPort));
        }
    }

    QStringList ordered = uniqueEndpoints.values();
    ordered.sort(Qt::CaseInsensitive);
    ordered.removeAll(wildcard);
    ordered.removeAll(loopback);

    endpointCombo_->addItem(wildcard);
    endpointCombo_->addItem(loopback);
    for (const QString& endpoint : ordered) {
        endpointCombo_->addItem(endpoint);
    }

    endpointCombo_->setCurrentText(wildcard);
}

void MainWindow::refreshLibrary() {
    musicDirectory_ = discoverMusicDirectory();
    localLibrary_.clear();

    if (musicDirectory_.isEmpty()) {
        libraryPathLabel_->setText("Music folder not found. Expected a nearby ./music directory.");
        syncLibraryToSession();
        rebuildLibraryList();
        syncUiFromSession();
        return;
    }

    libraryPathLabel_->setText("Source folder: " + musicDirectory_);

    QDir musicDir(musicDirectory_);
    const QFileInfoList files = musicDir.entryInfoList(
        musicNameFilters(),
        QDir::Files | QDir::Readable,
        QDir::Name | QDir::IgnoreCase
    );

    if (files.isEmpty()) {
        syncLibraryToSession();
        rebuildLibraryList();
        syncUiFromSession();
        return;
    }

    for (const QFileInfo& fileInfo : files) {
        SessionController::TrackEntry entry;
        entry.descriptor.title = fileInfo.completeBaseName().toStdString();
        entry.descriptor.fileName = fileInfo.fileName().toStdString();
        entry.descriptor.fileSize = static_cast<uint64_t>(fileInfo.size());
        entry.descriptor.durationMs = static_cast<uint64_t>(probeTrackDuration(fileInfo.absoluteFilePath()));
        entry.localPath = fileInfo.absoluteFilePath();
        entry.availableLocally = true;
        localLibrary_.push_back(entry);
    }

    syncLibraryToSession();
    syncUiFromSession();
}

void MainWindow::refreshTheme() {
    useDarkTheme_ = shouldUseDarkTheme();
}

void MainWindow::toggleAutoplay() {
    autoplayEnabled_ = !autoplayEnabled_;
    session_->setAutoplayEnabled(autoplayEnabled_);
    updatePlaybackOptionUi();
}

void MainWindow::cycleRepeatMode() {
    switch (repeatMode_) {
        case RepeatMode::Queue:
            repeatMode_ = RepeatMode::Track;
            break;
        case RepeatMode::Track:
            repeatMode_ = RepeatMode::Playlist;
            break;
        case RepeatMode::Playlist:
            repeatMode_ = RepeatMode::Queue;
            break;
    }

    switch (repeatMode_) {
        case RepeatMode::Queue:
            session_->setRepeatMode(::RepeatMode::Queue);
            break;
        case RepeatMode::Track:
            session_->setRepeatMode(::RepeatMode::Track);
            break;
        case RepeatMode::Playlist:
            session_->setRepeatMode(::RepeatMode::Playlist);
            break;
    }

    updatePlaybackOptionUi();
}

void MainWindow::toggleRole() {
    if (sessionActive_) {
        return;
    }

    hostMode_ = !hostMode_;
    if (hostMode_) {
        syncLibraryToSession();
    }
    updateModeUi();
    syncUiFromSession();
}

void MainWindow::updateModeUi() {
    const bool hostMode = hostMode_;

    for (const QPointer<QWidget>& widget : hostOnlyWidgets_) {
        if (widget) {
            widget->setEnabled(hostMode);
        }
    }

    modeBadgeLabel_->setText(hostMode ? "HOST" : "USER");
    modeBadgeLabel_->setStyleSheet(
        hostMode
            ? "background:#1f4e79;color:white;border:none;border-radius:11px;padding:5px 10px;font-weight:700;"
            : "background:#6b7280;color:white;border:none;border-radius:11px;padding:5px 10px;font-weight:700;"
    );

    roleToggleButton_->setText(hostMode ? "Switch to user" : "Switch to host");
    roleToggleButton_->setEnabled(!sessionActive_);
    endpointCombo_->setEnabled(!sessionActive_);
    sessionActionButton_->setText(
        sessionActive_
            ? (hostMode ? "Stop host" : "Disconnect")
            : (hostMode ? "Start host" : "Connect")
    );
    sessionActionButton_->setEnabled(true);
    sessionHintLabel_->setText(
        sessionActive_
            ? (hostMode
                   ? QString("Host is currently exposing the session on %1. Library and transport controls are live.")
                         .arg(endpointWithDefaultPort(endpointCombo_->currentText()))
                   : QString("User is currently connected to %1. Playback follows STATE_SYNC from host.")
                         .arg(endpointWithDefaultPort(endpointCombo_->currentText())))
            : (hostMode
                   ? QString("Host will bind to %1 and expose the session on LAN.")
                         .arg(endpointWithDefaultPort(endpointCombo_->currentText()))
                   : QString("User will connect to %1 inside the local network.")
                         .arg(endpointWithDefaultPort(endpointCombo_->currentText())))
    );

    updatePlaybackOptionUi();
}

void MainWindow::updatePlaybackOptionUi() {
    autoplayButton_->setText(autoplayEnabled_ ? "Autoplay: On" : "Autoplay: Off");
    playPauseButton_->setText(
        session_->sessionState().playbackState == PlaybackState::Playing ? "Pause" : "Play"
    );

    switch (repeatMode_) {
        case RepeatMode::Queue:
            repeatModeButton_->setText("Repeat: Queue");
            break;
        case RepeatMode::Track:
            repeatModeButton_->setText("Repeat: Track");
            break;
        case RepeatMode::Playlist:
            repeatModeButton_->setText("Repeat: Playlist");
            break;
    }
}

void MainWindow::syncUiFromSession() {
    sessionActive_ = session_->isSessionActive();

    switch (session_->sessionState().repeatMode) {
        case ::RepeatMode::Queue:
            repeatMode_ = RepeatMode::Queue;
            break;
        case ::RepeatMode::Track:
            repeatMode_ = RepeatMode::Track;
            break;
        case ::RepeatMode::Playlist:
            repeatMode_ = RepeatMode::Playlist;
            break;
    }

    autoplayEnabled_ = session_->sessionState().autoplayEnabled;

    bool rebuildPlaylist = libraryList_->count() != static_cast<int>(session_->playlist().size());
    if (!rebuildPlaylist) {
        for (int row = 0; row < libraryList_->count(); ++row) {
            if (libraryList_->item(row)->data(Qt::UserRole).toUInt() != session_->playlist()[static_cast<size_t>(row)].descriptor.trackId) {
                rebuildPlaylist = true;
                break;
            }
        }
    }
    if (rebuildPlaylist) {
        rebuildLibraryList();
    }

    bool rebuildClients = clientList_->count() != static_cast<int>(session_->clients().size());
    if (!rebuildClients) {
        for (int row = 0; row < clientList_->count(); ++row) {
            if (clientList_->item(row)->data(Qt::UserRole).toUInt() != session_->clients()[static_cast<size_t>(row)].clientId) {
                rebuildClients = true;
                break;
            }
        }
    }
    if (rebuildClients) {
        rebuildClientList();
    }

    {
        const QSignalBlocker blocker(libraryList_);
        const int currentIndex = session_->currentTrackIndex();
        if (currentIndex >= 0 && currentIndex < libraryList_->count()) {
            libraryList_->setCurrentRow(currentIndex);
        }
        else {
            libraryList_->clearSelection();
        }
    }

    const int currentIndex = session_->currentTrackIndex();
    const auto& playlist = session_->playlist();
    if (currentIndex >= 0 && currentIndex < static_cast<int>(playlist.size())) {
        const auto& track = playlist[static_cast<size_t>(currentIndex)];
        const qint64 durationMs = session_->currentPlaybackDurationMs();
        const qint64 positionMs = session_->currentPlaybackPositionMs();

        currentTrackLabel_->setText(QString::fromStdString(track.descriptor.title));
        currentTrackMetaLabel_->setText(
            track.availableLocally && !track.localPath.isEmpty()
                ? QString("Local file: %1").arg(track.localPath)
                : QString("Remote track: %1").arg(QString::fromStdString(track.descriptor.fileName))
        );
        const qint64 displayedPositionMs = seekInteractionActive_ && pendingSeekPositionSeconds_ >= 0
            ? static_cast<qint64>(pendingSeekPositionSeconds_) * 1000
            : positionMs;
        currentTimeLabel_->setText(durationMs > 0 ? formatDuration(displayedPositionMs) : "--:--");
        totalTimeLabel_->setText(durationMs > 0 ? formatDuration(durationMs) : "--:--");

        if (!seekInteractionActive_) {
            const QSignalBlocker blocker(seekSlider_);
            seekSlider_->setRange(0, durationMs > 0 ? static_cast<int>(durationMs / 1000) : 0);
            seekSlider_->setValue(static_cast<int>(positionMs / 1000));
        }
    }
    else {
        clearTrackPreview();
    }

    {
        const QSignalBlocker blocker(volumeSlider_);
        volumeSlider_->setValue(static_cast<int>(session_->sessionState().volumePercent));
    }
    volumeValueLabel_->setText(QString("%1%").arg(session_->sessionState().volumePercent));

    updatePlaybackOptionUi();
    updateModeUi();
}

void MainWindow::rebuildLibraryList() {
    const QSignalBlocker blocker(libraryList_);
    libraryList_->clear();

    const auto& playlist = session_->playlist();
    if (playlist.empty()) {
        libraryList_->addItem(musicDirectory_.isEmpty()
            ? "No music directory available"
            : "Drop audio files into /music and press Refresh folder");
        return;
    }

    for (const auto& track : playlist) {
        auto* item = new QListWidgetItem(QString::fromStdString(track.descriptor.title), libraryList_);
        item->setData(Qt::UserRole, track.descriptor.trackId);
        item->setData(Qt::UserRole + 1, track.localPath);
        item->setData(Qt::UserRole + 2, static_cast<qulonglong>(track.descriptor.durationMs));
        item->setData(Qt::UserRole + 3, track.availableLocally);
        item->setToolTip(track.availableLocally ? track.localPath : QString::fromStdString(track.descriptor.fileName));
        if (!track.availableLocally) {
            item->setText(item->text() + " [remote]");
        }
    }
}

void MainWindow::rebuildClientList() {
    clientList_->clear();

    for (const auto& client : session_->clients()) {
        auto* item = new QListWidgetItem();
        item->setData(Qt::UserRole, client.clientId);
        item->setSizeHint(QSize(0, 54));

        auto* rowWidget = new QWidget(clientList_);
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(8, 4, 8, 4);
        rowLayout->setSpacing(10);

        auto* textColumn = new QVBoxLayout();
        textColumn->setContentsMargins(0, 0, 0, 0);
        textColumn->setSpacing(2);

        auto* titleLabel = new QLabel(
            QString("#%1 %2").arg(client.clientId).arg(QString::fromStdString(client.nickname)),
            rowWidget
        );

        QString statusText;
        switch (client.status) {
            case ClientStatus::Connected:
                statusText = "connected";
                break;
            case ClientStatus::Ready:
                statusText = "ready";
                break;
            case ClientStatus::Disconnected:
                statusText = "disconnected";
                break;
        }

        auto* subtitleLabel = new QLabel(
            QString("%1 • %2")
                .arg(client.role == ClientRole::Host ? "host" : "listener")
                .arg(statusText),
            rowWidget
        );
        subtitleLabel->setObjectName("mutedLabel");

        textColumn->addWidget(titleLabel);
        textColumn->addWidget(subtitleLabel);

        auto* kickButton = new QToolButton(rowWidget);
        kickButton->setText("x");
        kickButton->setEnabled(hostMode_ && client.role != ClientRole::Host);
        connect(kickButton, &QToolButton::clicked, this, [this, client]() {
            session_->kickClient(client.clientId);
        });

        rowLayout->addLayout(textColumn, 1);
        rowLayout->addWidget(kickButton);

        clientList_->addItem(item);
        clientList_->setItemWidget(item, rowWidget);
    }
}

void MainWindow::syncLibraryToSession() {
    if (hostMode_) {
        session_->setLocalLibrary(localLibrary_);
    }
}

void MainWindow::handleLibrarySelectionChange(QListWidgetItem* item) {
    if (item == nullptr) {
        clearTrackPreview();
        return;
    }

    if (!hostMode_) {
        syncUiFromSession();
        return;
    }

    session_->selectTrackByRow(libraryList_->row(item));
}

void MainWindow::clearTrackPreview() {
    currentTrackLabel_->setText("No track selected");
    currentTrackMetaLabel_->setText("Pick a file from the music list to preview the title area.");
    currentTimeLabel_->setText("--:--");
    totalTimeLabel_->setText("--:--");
    seekInteractionActive_ = false;
    pendingSeekPositionSeconds_ = -1;
    seekSlider_->setValue(0);
    seekSlider_->setRange(0, 0);
}

QString MainWindow::discoverMusicDirectory() const {
    QStringList candidates;
    switch (musicDirectoryMode_) {
        case MusicDirectoryMode::ExecutableDirectory:
            candidates << (QCoreApplication::applicationDirPath() + "/music");
            break;
        case MusicDirectoryMode::CurrentWorkingDirectory:
            candidates << QDir::current().absoluteFilePath("music");
            break;
        case MusicDirectoryMode::Auto:
            candidates << (QCoreApplication::applicationDirPath() + "/music");
            candidates << QDir::current().absoluteFilePath("music");
            break;
    }

    for (const QString& candidate : candidates) {
        QDir dir(candidate);
        if (dir.exists()) {
            return dir.absolutePath();
        }
    }

    return {};
}

QString MainWindow::discoverLogoPath() const {
    return discoverAssetPath("sync-music-player_logo.png");
}

QString MainWindow::discoverAssetPath(const QString& fileName) const {
    const QStringList candidates = {
        QDir::current().absoluteFilePath("assets/" + fileName),
        QCoreApplication::applicationDirPath() + "/assets/" + fileName,
        QCoreApplication::applicationDirPath() + "/../assets/" + fileName,
        QCoreApplication::applicationDirPath() + "/../../assets/" + fileName
    };

    for (const QString& candidate : candidates) {
        QFileInfo info(candidate);
        if (info.exists() && info.isFile()) {
            return info.absoluteFilePath().replace('\\', '/');
        }
    }

    return {};
}

QString MainWindow::endpointWithDefaultPort(const QString& endpoint) const {
    QString trimmed = endpoint.trimmed();
    if (trimmed.isEmpty()) {
        return QString("127.0.0.1:%1").arg(kDefaultPort);
    }

    if (trimmed.contains(':')) {
        return trimmed;
    }

    return QString("%1:%2").arg(trimmed).arg(kDefaultPort);
}

QString MainWindow::formatDuration(qint64 durationMs) const {
    if (durationMs < 0) {
        return "--:--";
    }

    const qint64 totalSeconds = durationMs / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

qint64 MainWindow::probeTrackDuration(const QString& path) const {
    return probeTrackDurationWithQt(path);
}
