#pragma once

namespace forandsim::cli {

// Runs the CLI front-end (argument parsing + headless acquisition). Returns
// the process exit code.
int run(int argc, char** argv);

} // namespace forandsim::cli
