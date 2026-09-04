#include "cli_app.h"

#ifdef FORANDSIM_BUILD_GUI
#include "gui_app.h"
#endif

#include <iostream>

int main(int argc, char** argv) {
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
