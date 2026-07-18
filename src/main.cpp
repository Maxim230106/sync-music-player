#include "ui/main_window.hpp"

#include <QApplication>
#include <QLoggingCategory>

#include "common.hpp"

#ifdef _WIN32
    #include <windows.h>
    #include <cstdio>
    #include <io.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace {

QString qt_multimedia_log_filter_rules() {
    return QStringLiteral(
        "qt.multimedia.*=false\n"
        "qt.multimedia.ffmpeg.*=false\n"
        "qt.ffmpeg.*=false\n"
    );
}

void configure_qt_multimedia_logging() {
    qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",");
    qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", ",");
    qputenv("QT_LOGGING_RULES", qt_multimedia_log_filter_rules().toUtf8());
    QLoggingCategory::setFilterRules(qt_multimedia_log_filter_rules());
}

void print_help() {
    log("Sync Music Player");
    log("");
    log("Arguments:");
    log("  --enable-logs, --dev");
    log("      Enable protocol logging in a console window.");
    log("  --music-dir=auto");
    log("      Search music near the executable first, then in the current working directory.");
    log("  --music-dir=exe, --music-from-exe");
    log("      Search music only near the executable.");
    log("  --music-dir=current, --music-dir=cwd, --music-from-current");
    log("      Search music only in the current working directory.");
    log("  -?, -h, --help");
    log("      Print this help and exit.");
}

#ifdef _WIN32
void attach_parent_console_for_logging() {
    if (GetConsoleWindow() == nullptr && !AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }

    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
}
#endif

void suppress_process_stderr() {
#ifdef _WIN32
    FILE* stream = nullptr;
    freopen_s(&stream, "NUL", "w", stderr);
#else
    const int nullDescriptor = open("/dev/null", O_WRONLY);
    if (nullDescriptor < 0) {
        return;
    }

    dup2(nullDescriptor, fileno(stderr));
    close(nullDescriptor);
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    bool enableLogs = false;
    bool showHelp = false;
    MainWindow::MusicDirectoryMode musicDirectoryMode = MainWindow::MusicDirectoryMode::Auto;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]).trimmed();
        if (arg == "--enable-logs" || arg == "--dev") {
            enableLogs = true;
        }
        else if (arg == "--music-dir=exe" || arg == "--music-from-exe") {
            musicDirectoryMode = MainWindow::MusicDirectoryMode::ExecutableDirectory;
        }
        else if (arg == "--music-dir=current" || arg == "--music-dir=cwd" || arg == "--music-from-current") {
            musicDirectoryMode = MainWindow::MusicDirectoryMode::CurrentWorkingDirectory;
        }
        else if (arg == "--music-dir=auto") {
            musicDirectoryMode = MainWindow::MusicDirectoryMode::Auto;
        }
        else if (arg == "-?" || arg == "-h" || arg == "--help") {
            showHelp = true;
        }
    }

    configure_qt_multimedia_logging();

#ifdef _WIN32
    if (enableLogs || showHelp) {
        attach_parent_console_for_logging();
    }
#endif
    suppress_process_stderr();

    QApplication app(argc, argv);
    app.setApplicationName("sync-music-player");
    app.setApplicationVersion(APP_VERSION);

    set_protocol_logs_enabled(enableLogs);

    if (showHelp) {
        print_help();
        return 0;
    }

    MainWindow window(musicDirectoryMode);
    window.show();

    return app.exec();
}
