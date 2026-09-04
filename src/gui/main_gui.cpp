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
#include "reader_discovery.h"
#include "zip_writer.h"

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace forandsim::gui {

namespace {

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
    std::vector<std::string> readerLabels; // "<ICCID or (no card)> — <reader name>", for display
    int selectedReader = -1;

    char caseId[128] = "";
    char piece[64] = "";
    char operatorName[128] = "";
    char notes[512] = "";
    char outputDir[512] = ".";
    char pin[16] = "";
    bool noPin = false;
    bool verify = true;
    bool scanNonStandardFiles = true;
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

void refreshReaders(AppState& state) {
    state.readers.clear();
    state.readerLabels.clear();
    try {
        PcscTransport transport;
        for (auto& r : listReadersWithIccid(transport)) {
            state.readers.push_back(r.readerName);
            // Plain ASCII separator: Dear ImGui's default font atlas doesn't
            // include an em-dash glyph, so one would just render as a blank box.
            state.readerLabels.push_back((r.iccid ? *r.iccid : std::string("(no card)")) +
                                          " - " + r.readerName);
        }
    } catch (const std::exception& e) {
        state.appendLog(std::string("Failed to list readers: ") + e.what());
    }
    state.selectedReader = state.readers.empty() ? -1 : 0;
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
    refreshReaders(state);
    std::unique_ptr<std::thread> worker;

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
        if (ImGui::Button("Refresh") && !state.running) refreshReaders(state);
        if (state.readers.empty()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "No PC/SC readers detected.");
        } else {
            if (ImGui::BeginCombo("##reader", state.selectedReader >= 0
                                                   ? state.readerLabels[state.selectedReader].c_str()
                                                   : "Select a reader")) {
                for (int i = 0; i < (int)state.readers.size(); ++i) {
                    bool selected = (i == state.selectedReader);
                    if (ImGui::Selectable(state.readerLabels[i].c_str(), selected)) {
                        state.selectedReader = i;
                    }
                }
                ImGui::EndCombo();
            }
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
        ImGui::Text("Log");
        ImGui::BeginChild("log", ImVec2(0, 0), true);
        {
            std::lock_guard<std::mutex> lock(state.logMutex);
            for (auto& line : state.log) ImGui::TextUnformatted(line.c_str());
        }
        if (state.running) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

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
