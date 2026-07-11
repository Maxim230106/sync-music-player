#include "ui/main_window.hpp"

#include <QApplication>
#include <QLoggingCategory>

#include "common.hpp"

#ifdef _WIN32
    #include <windows.h>
    #include <cstdio>
#endif

namespace {

void suppress_qt_multimedia_logs() {
    QLoggingCategory::setFilterRules(
        QStringLiteral(
            "qt.multimedia.*=false\n"
            "qt.multimedia.ffmpeg.*=false\n"
            "qt.ffmpeg.*=false\n"
        )
    );
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
void ensure_console() {
    if (GetConsoleWindow() == nullptr) {
        AllocConsole();
    }

    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    suppress_qt_multimedia_logs();

    QApplication app(argc, argv);
    app.setApplicationName("sync-music-player");
    app.setApplicationVersion(APP_VERSION);

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

    set_protocol_logs_enabled(enableLogs);

#ifdef _WIN32
    if (enableLogs || showHelp) {
        ensure_console();
    }
#endif

    if (showHelp) {
        print_help();
        return 0;
    }

    MainWindow window(musicDirectoryMode);
    window.show();

    return app.exec();
}
