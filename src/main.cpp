#include "cli_app.h"

#ifdef FORANDSIM_BUILD_GUI
#include "gui_app.h"
#endif

#include <iostream>

#ifdef _WIN32
#include <cstdio>
#include <windows.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32) && defined(FORANDSIM_BUILD_GUI)
    // This build links as a WINDOWS-subsystem exe (see CMakeLists.txt) so
    // double-clicking it doesn't spawn an unstoppable console window. That
    // means a CLI invocation from an existing terminal has no console
    // attached by default; re-attach to the caller's so --help etc. still work.
    if (argc > 1 && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
    }
#endif

    if (argc > 1) {
        return forandsim::cli::run(argc, argv);
    }

#ifdef FORANDSIM_BUILD_GUI
    return forandsim::gui::run();
#else
    std::cout << "For&SIM built without GUI support; use --help for CLI options.\n";
    return forandsim::cli::run(argc, argv);
#endif
}
