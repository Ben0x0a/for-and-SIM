#include "cli_app.h"

#include <cstring>
#include <iostream>
#include <optional>
#include <string>

#include "acquisition_engine.h"
#include "case_metadata.h"
#include "html_report.h"
#include "output_paths.h"
#include "pcsc_transport.h"
#include "zip_writer.h"

namespace forandsim::cli {

namespace {

void printUsage() {
    std::cout <<
        "For&SIM - SIM/USIM forensic acquisition tool (CLI mode)\n\n"
        "Usage:\n"
        "  forandsim --list-readers\n"
        "  forandsim --check-pin --reader <name>\n"
        "  forandsim --reader <name> --case <id> --piece <n> --operator <name>\n"
        "                --output <dir> --confirm-authorized [--pin <digits> | --no-pin]\n"
        "                [--notes <text>] [--no-verify] [--no-scan-hidden] [--force]\n\n"
        "Options:\n"
        "  --list-readers        List detected PC/SC readers and exit\n"
        "  --check-pin           Read-only pre-flight: report CHV1 attempts remaining and exit\n"
        "                        (does NOT attempt to verify the PIN)\n"
        "  --reader <name>       Reader to connect to (see --list-readers)\n"
        "  --case <id>           Case identifier\n"
        "  --piece <n>           Piece / exhibit number\n"
        "  --operator <name>     Operator / examiner name\n"
        "  --notes <text>        Free-form examiner notes\n"
        "  --output <dir>        Base output directory; results go in <dir>/<case>/\n"
        "  --confirm-authorized  Attest that you are authorized to examine this exhibit\n"
        "                        (mandatory; acquisition refuses to run without it)\n"
        "  --pin <digits>        Verify this PIN (CHV1) and perform a full acquisition\n"
        "  --no-pin              Skip PIN verification; acquire ICCID only\n"
        "  --no-verify           Skip the post-acquisition verification pass (re-read + diff\n"
        "                        every file). Verification is ON by default; it roughly\n"
        "                        doubles acquisition time but is the only way to confirm\n"
        "                        nothing changed across every PIN-gated file, not just ICCID.\n"
        "  --no-scan-hidden      Skip the brute-force probe for non-standard/hidden EFs and\n"
        "                        DFs. Scanning is ON by default; it can be slow on a slow\n"
        "                        reader, especially on cards that answer SELECT 'successfully'\n"
        "                        for almost any id.\n"
        "  --force               Overwrite an existing result folder instead of refusing\n"
        "  --help                Show this message\n";
}

std::optional<std::string> argValue(int argc, char** argv, int& i) {
    if (i + 1 >= argc) return std::nullopt;
    return std::string(argv[++i]);
}

} // namespace

int run(int argc, char** argv) {
    bool listReaders = false;
    bool checkPin = false;
    std::string readerName, outputDir;
    forandsim::CaseMetadata caseMetadata;
    std::optional<std::string> pin;
    bool noPin = false;
    forandsim::AcquisitionOptions options;
    bool force = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--list-readers") {
            listReaders = true;
        } else if (arg == "--check-pin") {
            checkPin = true;
        } else if (arg == "--reader") {
            if (auto v = argValue(argc, argv, i)) readerName = *v;
        } else if (arg == "--case") {
            if (auto v = argValue(argc, argv, i)) caseMetadata.caseIdentifier = *v;
        } else if (arg == "--piece") {
            if (auto v = argValue(argc, argv, i)) caseMetadata.pieceNumber = *v;
        } else if (arg == "--operator") {
            if (auto v = argValue(argc, argv, i)) caseMetadata.operatorName = *v;
        } else if (arg == "--notes") {
            if (auto v = argValue(argc, argv, i)) caseMetadata.examinerNotes = *v;
        } else if (arg == "--output") {
            if (auto v = argValue(argc, argv, i)) outputDir = *v;
        } else if (arg == "--confirm-authorized") {
            caseMetadata.authorizationConfirmed = true;
        } else if (arg == "--pin") {
            if (auto v = argValue(argc, argv, i)) pin = *v;
        } else if (arg == "--no-pin") {
            noPin = true;
        } else if (arg == "--no-verify") {
            options.verify = false;
        } else if (arg == "--no-scan-hidden") {
            options.scanNonStandardFiles = false;
        } else if (arg == "--force") {
            force = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage();
            return 1;
        }
    }

    try {
        forandsim::PcscTransport transport;

        if (listReaders) {
            auto readers = transport.listReaders();
            if (readers.empty()) {
                std::cout << "No PC/SC readers found.\n";
            } else {
                for (auto& r : readers) std::cout << r << "\n";
            }
            return 0;
        }

        if (checkPin) {
            if (readerName.empty()) {
                std::cerr << "--check-pin requires --reader <name>.\n";
                return 1;
            }
            transport.connect(readerName);
            auto attempts = forandsim::checkChv1AttemptsRemaining(transport);
            if (!attempts.has_value()) {
                std::cout << "Could not determine CHV1 status from this card's response.\n";
            } else if (*attempts < 0) {
                std::cout << "CHV1 is not initialized (no PIN set on this card).\n";
            } else {
                std::cout << "CHV1 attempts remaining: " << *attempts << "\n";
                if (*attempts <= 1) {
                    std::cout << "WARNING: a wrong PIN now will block the card.\n";
                }
            }
            return 0;
        }

        if (readerName.empty() || outputDir.empty() || caseMetadata.caseIdentifier.empty()) {
            std::cerr << "Missing required options. --reader, --case and --output are mandatory.\n\n";
            printUsage();
            return 1;
        }
        if (!noPin && !pin.has_value()) {
            std::cerr << "Specify either --pin <digits> or --no-pin.\n";
            return 1;
        }
        if (!caseMetadata.authorizationConfirmed) {
            std::cerr << "Refusing to acquire: pass --confirm-authorized to attest you are "
                         "authorized to examine this exhibit.\n";
            return 1;
        }

        forandsim::output::OutputPaths paths =
            forandsim::output::computeOutputPaths(outputDir, caseMetadata.caseIdentifier);
        if (!force &&
            (forandsim::output::pathExists(paths.zipPath) ||
             forandsim::output::pathExists(paths.htmlPath))) {
            std::cerr << "Refusing to overwrite existing results in '" << paths.caseDir
                      << "'. Pass --force to overwrite, or choose a different --case/--output.\n";
            return 1;
        }
        forandsim::output::ensureCaseDir(paths.caseDir);

        transport.connect(readerName);

        auto progress = [](const std::string& msg) { std::cout << "[forandsim] " << msg << "\n"; };

        forandsim::AcquisitionResult result =
            forandsim::acquire(transport, caseMetadata, noPin ? std::nullopt : pin, options, progress);
        result.readerName = readerName;

        forandsim::output::EvidenceZipResult zipInfo =
            forandsim::output::writeEvidenceZip(result, paths.zipPath);
        forandsim::output::writeHtmlReport(result, paths.htmlPath, paths.zipFileName, zipInfo);

        std::cout << "Evidence container: " << paths.zipPath << "\n";
        std::cout << "Evidence SHA-256:   " << zipInfo.sha256 << "\n";
        std::cout << "HTML report:        " << paths.htmlPath << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace forandsim::cli
