#include "gui_app.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "acquisition_engine.h"
#include "case_metadata.h"
#include "html_report.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "output_paths.h"
#include "pcsc_transport.h"
#include "reader_monitor.h"
#include "zip_writer.h"

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace forandsim::gui {

namespace {

// Dear ImGui's InputTextMultiline (used for the selectable/copyable log
// panel) doesn't word-wrap long lines on its own - it scrolls horizontally
// instead. Insert real newlines at word boundaries so it reads naturally.
std::string wrapLine(const std::string& line, size_t maxCols) {
    std::string out;
    size_t col = 0;
    size_t i = 0;
    while (i < line.size()) {
        size_t nextSpace = line.find(' ', i);
        size_t wordEnd = (nextSpace == std::string::npos) ? line.size() : nextSpace;
        size_t wordLen = wordEnd - i;
        if (col > 0 && col + 1 + wordLen > maxCols) {
            out += '\n';
            col = 0;
        } else if (col > 0) {
            out += ' ';
            ++col;
        }
        out += line.substr(i, wordLen);
        col += wordLen;
        i = (nextSpace == std::string::npos) ? line.size() : nextSpace + 1;
    }
    return out;
}

bool isDirectory(const std::string& path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// Native folder picker. Windows uses IFileDialog (COM); macOS/Linux shell out to a
// small helper that's present on essentially every desktop install, rather than
// vendoring a whole native-dialog library for one button.
std::optional<std::string> browseForFolder() {
#ifdef _WIN32
    std::optional<std::string> result;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        IFileDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&dialog)))) {
            DWORD opts = 0;
            dialog->GetOptions(&opts);
            dialog->SetOptions(opts | FOS_PICKFOLDERS);
            if (SUCCEEDED(dialog->Show(nullptr))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
                        std::string utf8(len > 0 ? len - 1 : 0, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), len, nullptr, nullptr);
                        result = utf8;
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        CoUninitialize();
    }
    return result;
#else
    auto runAndCapture = [](const char* cmd) -> std::optional<std::string> {
        FILE* pipe = popen(cmd, "r");
        if (!pipe) return std::nullopt;
        std::string out;
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) out += buf;
        int rc = pclose(pipe);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (rc != 0 || out.empty()) return std::nullopt;
        return out;
    };
#ifdef __APPLE__
    return runAndCapture("osascript -e 'POSIX path of (choose folder)' 2>/dev/null");
#else
    if (auto r = runAndCapture("zenity --file-selection --directory 2>/dev/null")) return r;
    return runAndCapture("kdialog --getexistingdirectory ~ 2>/dev/null");
#endif
#endif
}

struct AppState {
    std::vector<std::string> readers;      // actual reader names, used to connect
    std::vector<std::string> readerLabels; // "<ICCID or card state> - <reader name>", for display
    int selectedReader = -1;
    bool userPickedReader = false; // an explicit pick is never overridden by auto-selection
    bool readersScanned = false;
    std::string readerSummary;               // e.g. "2 readers, 1 card detected"
    std::optional<std::string> readerError; // PC/SC service unavailable, etc.

    // Held by every code path that talks to a card (acquisition, PIN check,
    // the ReaderMonitor's ICCID reads): in shared mode their APDUs would
    // otherwise interleave on the same card.
    std::mutex cardIoMutex;

    char caseId[128] = "";
    char piece[64] = "";
    char operatorName[128] = "";
    char notes[512] = "";
    char outputDir[512] = ".";
    char pin[16] = "";
    bool noPin = false;
    bool verify = true;
    bool scanNonStandardFiles = false;
    bool authorizationConfirmed = false;

    std::atomic<bool> running{false};
    std::atomic<bool> cancelRequested{false};
    std::mutex logMutex;
    std::vector<std::string> log;
    std::optional<std::string> lastError;
    std::optional<std::string> lastCaseDir;
    std::optional<std::string> lastZipSha256;
    std::optional<std::string> pinStatusText;

    bool showOverwriteConfirm = false;
    std::string pendingReaderName;
    std::string pendingCaseDir;

    void appendLog(const std::string& line) {
        std::lock_guard<std::mutex> lock(logMutex);
        log.push_back(line);
    }
};

std::string readerLabel(const MonitoredReader& r) {
    std::string card;
    switch (r.presence) {
    case CardPresence::Absent: card = "(no card)"; break;
    case CardPresence::Mute: card = "(card not responding)"; break;
    case CardPresence::Exclusive: card = "(card in use by another application)"; break;
    case CardPresence::Present:
    case CardPresence::Unknown:
        if (r.iccidState == IccidState::Read) {
            card = r.iccid;
        } else if (r.iccidState == IccidState::Pending) {
            card = r.presence == CardPresence::Present ? "(card detected, reading ICCID...)"
                                                       : "(checking...)";
        } else {
            card = r.presence == CardPresence::Present ? "(card detected, ICCID not readable)"
                                                       : "(no card or not readable)";
        }
        break;
    }
    // Plain ASCII separator: Dear ImGui's default font atlas doesn't
    // include an em-dash glyph, so one would just render as a blank box.
    return card + " - " + r.readerName;
}

// Applies a ReaderMonitor snapshot: rebuilds the list and re-selects a reader.
// WHY auto-select: with two readers (e.g. built-in + USB), always selecting
// the first one hid a SIM sitting in the second, so it looked undetected.
void applyReaderSnapshot(AppState& state, const ReaderSnapshot& snap) {
    std::string previous =
        state.selectedReader >= 0 ? state.readers[state.selectedReader] : std::string();

    state.readersScanned = snap.scanned;
    state.readerError = snap.error;
    state.readers.clear();
    state.readerLabels.clear();
    int cards = 0;
    for (auto& r : snap.readers) {
        state.readers.push_back(r.readerName);
        state.readerLabels.push_back(readerLabel(r));
        if (r.presence != CardPresence::Absent &&
            !(r.presence == CardPresence::Unknown && r.iccidState != IccidState::Read)) {
            ++cards;
        }
    }
    state.readerSummary = std::to_string(snap.readers.size()) +
                          (snap.readers.size() == 1 ? " reader, " : " readers, ") +
                          std::to_string(cards) + (cards == 1 ? " card" : " cards") + " detected";

    bool stillPlugged = false;
    for (auto& name : state.readers) stillPlugged |= (name == previous);
    if (!stillPlugged) state.userPickedReader = false;

    state.selectedReader = pickReader(snap.readers, previous, state.userPickedReader);
    std::string now = state.selectedReader >= 0 ? state.readers[state.selectedReader] : "";
    if (now != previous) {
        // The PIN status shown belonged to the previous reader's card.
        state.pinStatusText.reset();
        if (!now.empty() && !previous.empty()) {
            state.appendLog("Reader selection changed automatically to: " +
                            state.readerLabels[state.selectedReader]);
        }
    }
}

void openPath(const std::string& path) {
#ifdef _WIN32
    std::string cmd = "start \"\" \"" + path + "\"";
#elif __APPLE__
    std::string cmd = "open \"" + path + "\"";
#else
    std::string cmd = "xdg-open \"" + path + "\"";
#endif
    std::system(cmd.c_str());
}

void checkPinStatus(AppState* state, std::string readerName) {
    try {
        std::lock_guard<std::mutex> cardIo(state->cardIoMutex);
        PcscTransport transport;
        transport.connect(readerName);
        auto attempts = checkChv1AttemptsRemaining(transport);
        if (!attempts.has_value()) {
            state->pinStatusText = "Could not determine CHV1 status.";
        } else if (*attempts < 0) {
            state->pinStatusText = "CHV1 not initialized (no PIN set on this card).";
        } else {
            state->pinStatusText = "CHV1 attempts remaining: " + std::to_string(*attempts) +
                (*attempts <= 1 ? "  /!\\ a wrong PIN now will block the card" : "");
        }
    } catch (const std::exception& e) {
        state->pinStatusText = std::string("Failed to check PIN status: ") + e.what();
    }
}

void runAcquisition(AppState* state, std::string readerName, output::OutputPaths paths) {
    state->running = true;
    state->cancelRequested = false;
    state->lastError.reset();
    state->lastCaseDir.reset();
    state->lastZipSha256.reset();

    try {
        CaseMetadata meta;
        meta.caseIdentifier = state->caseId;
        meta.pieceNumber = state->piece;
        meta.operatorName = state->operatorName;
        meta.examinerNotes = state->notes;
        meta.authorizationConfirmed = state->authorizationConfirmed;

        std::optional<std::string> pin;
        if (!state->noPin) pin = std::string(state->pin);

        AcquisitionOptions options;
        options.verify = state->verify;
        options.scanNonStandardFiles = state->scanNonStandardFiles;
        options.cancelRequested = &state->cancelRequested;

        // Held for the whole run so the ReaderMonitor never sends the card
        // a SELECT in the middle of the acquisition's own file walk.
        std::lock_guard<std::mutex> cardIo(state->cardIoMutex);
        PcscTransport transport;
        transport.connect(readerName);

        auto progress = [state](const std::string& msg) { state->appendLog(msg); };
        AcquisitionResult result = acquire(transport, meta, pin, options, progress);
        result.readerName = readerName;

        output::ensureCaseDir(paths.caseDir);
        output::EvidenceZipResult zipInfo = output::writeEvidenceZip(result, paths.zipPath);
        output::writeHtmlReport(result, paths.htmlPath, paths.zipFileName, zipInfo);

        state->lastCaseDir = paths.caseDir;
        state->lastZipSha256 = zipInfo.sha256;
        if (result.cancelled) {
            state->appendLog("Cancelled. Partial results written to " + paths.caseDir);
        } else {
            state->appendLog("Done. Wrote " + paths.zipPath + " (SHA-256 " + zipInfo.sha256 +
                              ") and " + paths.htmlPath);
        }
    } catch (const std::exception& e) {
        state->lastError = e.what();
        state->appendLog(std::string("ERROR: ") + e.what());
    }

    state->running = false;
}

void startAcquisition(AppState& state, std::unique_ptr<std::thread>& worker,
                       const std::string& readerName, const output::OutputPaths& paths) {
    if (worker && worker->joinable()) worker->join();
    worker = std::make_unique<std::thread>(runAcquisition, &state, readerName, paths);
}

} // namespace

int run() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

#ifdef __APPLE__
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    SDL_WindowFlags windowFlags =
        (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow("For&SIM - SIM/USIM Forensic Acquisition", 100, 100,
                                           900, 700, windowFlags);
    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, glContext);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init(glsl_version);

    AppState state;
    // Declared after `state` so it is destroyed (thread joined) before the
    // cardIoMutex it references.
    ReaderMonitor readerMonitor(state.cardIoMutex);
    readerMonitor.start();
    std::unique_ptr<std::thread> worker;
    ReaderSnapshot readerSnapshot;

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                done = true;
            } else if (event.type == SDL_DROPFILE) {
                char* dropped = event.drop.file;
                if (isDirectory(dropped)) {
                    std::snprintf(state.outputDir, sizeof(state.outputDir), "%s", dropped);
                    state.appendLog(std::string("Output directory set via drag-and-drop: ") + dropped);
                } else {
                    state.appendLog(std::string("Ignored dropped item (not a folder): ") + dropped);
                }
                SDL_free(dropped);
            }
        }

        if (readerMonitor.takeSnapshotIfChanged(readerSnapshot)) {
            applyReaderSnapshot(state, readerSnapshot);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 16));
        ImGui::Begin("For&SIM", nullptr,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        // Leave room on the right for trailing buttons (Browse..., Check PIN
        // status, ...) placed with SameLine() after these fields, so they
        // never end up flush against the window's edge.
        float fieldWidth = ImGui::GetContentRegionAvail().x * 0.68f;

        ImGui::Text("SIM/USIM forensic acquisition");
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f),
                            "Developed for educational purposes. Do not use on real cases.");
        ImGui::Separator();

        ImGui::Text("Reader");
        ImGui::SameLine();
        // Readers and cards are picked up automatically (ReaderMonitor polls
        // every second); Refresh only forces an immediate poll and a fresh
        // ICCID read of every card.
        if (ImGui::Button("Refresh")) readerMonitor.rescan();
        if (!state.readersScanned) {
            ImGui::TextDisabled("Looking for readers...");
        } else if (state.readers.empty()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "No PC/SC readers detected.");
            if (state.readerError) ImGui::TextWrapped("(%s)", state.readerError->c_str());
        } else {
            if (ImGui::BeginCombo("##reader", state.selectedReader >= 0
                                                   ? state.readerLabels[state.selectedReader].c_str()
                                                   : "Select a reader")) {
                for (int i = 0; i < (int)state.readers.size(); ++i) {
                    bool selected = (i == state.selectedReader);
                    if (ImGui::Selectable(state.readerLabels[i].c_str(), selected)) {
                        if (i != state.selectedReader) state.pinStatusText.reset();
                        state.selectedReader = i;
                        state.userPickedReader = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", state.readerSummary.c_str());
        }

        ImGui::Separator();
        ImGui::Text("Case information");
        ImGui::PushItemWidth(fieldWidth);
        ImGui::InputText("Case identifier", state.caseId, sizeof(state.caseId));
        ImGui::InputText("Piece / exhibit number", state.piece, sizeof(state.piece));
        ImGui::InputText("Operator", state.operatorName, sizeof(state.operatorName));
        ImGui::PopItemWidth();
        ImGui::InputTextMultiline("Notes", state.notes, sizeof(state.notes), ImVec2(fieldWidth, 60));
        ImGui::PushItemWidth(fieldWidth);
        ImGui::InputText("Output directory", state.outputDir, sizeof(state.outputDir));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            if (auto picked = browseForFolder()) {
                std::snprintf(state.outputDir, sizeof(state.outputDir), "%s", picked->c_str());
            }
        }
        ImGui::TextDisabled("(you can also drag and drop a folder onto this window; "
                             "results go in <output dir>/<case>/)");

        ImGui::Separator();
        ImGui::Checkbox("I am authorized to examine this exhibit", &state.authorizationConfirmed);
        if (!state.authorizationConfirmed) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1),
                                "Required: acquisition refuses to touch the card without this.");
        }

        ImGui::Separator();
        ImGui::Text("PIN");
        ImGui::Checkbox("Extract without PIN (ICCID only)", &state.noPin);
        if (!state.noPin) {
            ImGui::PushItemWidth(fieldWidth);
            ImGui::InputText("PIN (CHV1)", state.pin, sizeof(state.pin), ImGuiInputTextFlags_Password);
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("Check PIN status") && !state.running && state.selectedReader >= 0) {
                checkPinStatus(&state, state.readers[state.selectedReader]);
            }
            if (state.pinStatusText) {
                ImGui::TextWrapped("%s", state.pinStatusText->c_str());
            }
        }
        ImGui::Checkbox("Verify (re-read every file after acquisition, ~2x time)", &state.verify);
        ImGui::Checkbox("Scan for non-standard/hidden files (can be slow on some readers/cards)",
                         &state.scanNonStandardFiles);

        ImGui::Separator();
        bool canStart = !state.running && state.selectedReader >= 0 && state.authorizationConfirmed &&
                         state.caseId[0] != '\0' && (state.noPin || state.pin[0] != '\0');
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button("Start acquisition", ImVec2(200, 32))) {
            std::string readerName = state.readers[state.selectedReader];
            output::OutputPaths paths =
                output::computeOutputPaths(state.outputDir, state.caseId);
            if (output::pathExists(paths.zipPath) || output::pathExists(paths.htmlPath)) {
                state.showOverwriteConfirm = true;
                state.pendingReaderName = readerName;
                state.pendingCaseDir = paths.caseDir;
                ImGui::OpenPopup("Overwrite existing results?");
            } else {
                startAcquisition(state, worker, readerName, paths);
            }
        }
        ImGui::EndDisabled();

        if (ImGui::BeginPopupModal("Overwrite existing results?", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Results already exist in:\n%s", state.pendingCaseDir.c_str());
            ImGui::Text("Overwrite them?");
            ImGui::Separator();
            if (ImGui::Button("Overwrite", ImVec2(120, 0))) {
                output::OutputPaths paths =
                    output::computeOutputPaths(state.outputDir, state.caseId);
                startAcquisition(state, worker, state.pendingReaderName, paths);
                state.showOverwriteConfirm = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                state.showOverwriteConfirm = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (state.running) {
            ImGui::SameLine();
            ImGui::Text("Acquiring... this can take a while for a full dump.");
            ImGui::SameLine();
            if (ImGui::Button("Stop")) {
                state.cancelRequested = true;
                state.appendLog("Stop requested; finishing current step cleanly...");
            }
        }

        if (state.lastCaseDir) {
            ImGui::SameLine();
            if (ImGui::Button("Open output folder", ImVec2(0, 32))) {
                openPath(*state.lastCaseDir);
            }
        }

        ImGui::Separator();
        ImGui::Text("Log (selectable - click and drag, or Ctrl+A / Ctrl+C, to copy)");
        {
            std::string logText;
            {
                std::lock_guard<std::mutex> lock(state.logMutex);
                for (auto& line : state.log) {
                    logText += wrapLine(line, 100);
                    logText += '\n';
                }
            }
            std::vector<char> logBuf(logText.begin(), logText.end());
            logBuf.push_back('\0');
            ImGui::InputTextMultiline("##log", logBuf.data(), logBuf.size(), ImVec2(-1, -1),
                                       ImGuiInputTextFlags_ReadOnly);
        }

        ImGui::End();
        ImGui::PopStyleVar(); // WindowPadding

        ImGui::Render();
        glViewport(0, 0, (int)ImGui::GetIO().DisplaySize.x, (int)ImGui::GetIO().DisplaySize.y);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    if (worker && worker->joinable()) worker->join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace forandsim::gui
