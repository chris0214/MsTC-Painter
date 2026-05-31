#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QLocale>
#include <QSettings>
#include <QStyleFactory>
#include <QTranslator>
#include <spdlog/spdlog.h>

#include <cstring>

#include <Windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace {

// Apply Windows 10/11 dark titlebar to a top-level window.
// Safe no-op on older Windows.
void enableDarkTitleBar(HWND hwnd)
{
    BOOL dark = TRUE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE
    // Build 18985+: attribute 20. Earlier builds: 19.
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

/// Pick the UI language for this run. Priority:
///   1. QSettings "uiLanguage" if set by the user (set via the Language menu)
///   2. System locale → if it starts with "zh" use Chinese, else English
QString resolveUiLanguage()
{
    QSettings s("msTC", "TextureStudio");
    QString saved = s.value("uiLanguage").toString();
    if (!saved.isEmpty()) return saved;

    const QString sys = QLocale::system().name();   // e.g. "zh_CN", "en_US"
    if (sys.startsWith("zh", Qt::CaseInsensitive)) return "zh_CN";
    return "en";
}

/// Install a QTranslator for the chosen language. Returns true if a .qm
/// was found and loaded; false means we fall back to the C++ source strings
/// (which are English).
bool installTranslator(QApplication& app, const QString& lang)
{
    static QTranslator translator;   // static so it lives for the app lifetime

    if (lang == "en") return true;   // English = source language, no .qm needed

    // .qm files are staged into <exe-dir>/translations by the build.
    const QString tsBase = "mstc_" + lang;
    const QString tsDir  = QCoreApplication::applicationDirPath() + "/translations";

    if (translator.load(tsBase, tsDir)) {
        app.installTranslator(&translator);
        return true;
    }
    spdlog::warn("Could not load translation '{}' from '{}'",
                 tsBase.toStdString(), tsDir.toStdString());
    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    // Console attachment is opt-in via --console (or --debug). Without the
    // flag the app launches as a regular WIN32 GUI with no console window,
    // which is the right default for end users. Devs/triagers who need
    // spdlog output can launch with the flag.
    bool wantConsole = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--console") == 0
            || std::strcmp(a, "--debug")   == 0
            || std::strcmp(a, "-c")        == 0)
        {
            wantConsole = true;
        }
    }

    if (wantConsole) {
        // Attach to parent console (if launched from cmd) or allocate a new
        // one. Without this the WIN32 subsystem app discards stdout silently.
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
        }
        spdlog::set_level(spdlog::level::debug);
    } else {
        // No console: drop spdlog to a no-op level. (Prevents the cost of
        // formatting messages that nobody will read.)
        spdlog::set_level(spdlog::level::off);
    }

    spdlog::info("msTC Texture Studio starting...");

    QApplication app(argc, argv);
    app.setApplicationName("msTC Texture Studio");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("msTC");

    // Install translator BEFORE creating MainWindow so menus/toolbars come up
    // already in the user's language.
    installTranslator(app, resolveUiLanguage());

    // Use Fusion as the base style — it's the most paintable cross-platform style
    // and our QSS layers cleanly on top of it.
    app.setStyle(QStyleFactory::create("Fusion"));
    ui::applyDarkTheme(app);

    MainWindow window;
    window.show();

    // Match the Windows titlebar to our dark UI
    enableDarkTitleBar(reinterpret_cast<HWND>(window.winId()));

    spdlog::info("Main window shown, entering event loop");
    return app.exec();
}
