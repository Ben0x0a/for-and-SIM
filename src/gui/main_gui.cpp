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
#include "pcsc_transport.h"
#include "zip_writer.h"

namespace forandsim::gui {

namespace {

struct AppState {
    std::vector<std::string> readers;
    int selectedReader = -1;

    char caseId[128] = "";
    char piece[64] = "";
    char operatorName[128] = "";
    char notes[512] = "";
    char outputDir[512] = ".";
    char pin[16] = "";
    bool noPin = false;
    bool verify = true;
    bool authorizationConfirmed = false;

    std::atomic<bool> running{false};
    std::mutex logMutex;
    std::vector<std::string> log;
    std::optional<std::string> lastError;
    std::optional<std::string> lastZipPath;
    std::optional<std::string> lastHtmlPath;
    std::optional<std::string> lastZipSha256;
    std::optional<std::string> pinStatusText;

    void appendLog(const std::string& line) {
        std::lock_guard<std::mutex> lock(logMutex);
        log.push_back(line);
    }
};

void refreshReaders(AppState& state) {
    try {
        PcscTransport transport;
        state.readers = transport.listReaders();
    } catch (const std::exception& e) {
        state.readers.clear();
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

void runAcquisition(AppState* state, std::string readerName) {
    state->running = true;
    state->lastError.reset();
    state->lastZipPath.reset();
    state->lastHtmlPath.reset();
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

        PcscTransport transport;
        transport.connect(readerName);

        auto progress = [state](const std::string& msg) { state->appendLog(msg); };
        AcquisitionResult result = acquire(transport, meta, pin, state->verify, progress);
        result.readerName = readerName;

        std::string base = std::string(state->outputDir) + "/" +
            (meta.caseIdentifier.empty() ? "acquisition" : meta.caseIdentifier);
        std::string zipPath = base + ".zip";
        std::string htmlPath = base + ".html";
        std::string zipFileName = (meta.caseIdentifier.empty() ? "acquisition" : meta.caseIdentifier) + ".zip";

        output::EvidenceZipResult zipInfo = output::writeEvidenceZip(result, zipPath);
        output::writeHtmlReport(result, htmlPath, zipFileName, zipInfo);

        state->lastZipPath = zipPath;
        state->lastHtmlPath = htmlPath;
        state->lastZipSha256 = zipInfo.sha256;
        state->appendLog("Done. Wrote " + zipPath + " (SHA-256 " + zipInfo.sha256 + ") and " + htmlPath);
    } catch (const std::exception& e) {
        state->lastError = e.what();
        state->appendLog(std::string("ERROR: ") + e.what());
    }

    state->running = false;
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
            if (event.type == SDL_QUIT) done = true;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("For&SIM", nullptr,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

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
                                                   ? state.readers[state.selectedReader].c_str()
                                                   : "Select a reader")) {
                for (int i = 0; i < (int)state.readers.size(); ++i) {
                    bool selected = (i == state.selectedReader);
                    if (ImGui::Selectable(state.readers[i].c_str(), selected)) {
                        state.selectedReader = i;
                    }
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Separator();
        ImGui::Text("Case information");
        ImGui::InputText("Case identifier", state.caseId, sizeof(state.caseId));
        ImGui::InputText("Piece / exhibit number", state.piece, sizeof(state.piece));
        ImGui::InputText("Operator", state.operatorName, sizeof(state.operatorName));
        ImGui::InputTextMultiline("Notes", state.notes, sizeof(state.notes), ImVec2(-1, 60));
        ImGui::InputText("Output directory", state.outputDir, sizeof(state.outputDir));

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
            ImGui::InputText("PIN (CHV1)", state.pin, sizeof(state.pin),
                              ImGuiInputTextFlags_Password);
            ImGui::SameLine();
            if (ImGui::Button("Check PIN status") && !state.running && state.selectedReader >= 0) {
                checkPinStatus(&state, state.readers[state.selectedReader]);
            }
            if (state.pinStatusText) {
                ImGui::TextWrapped("%s", state.pinStatusText->c_str());
            }
        }
        ImGui::Checkbox("Verify (re-read every file after acquisition, ~2x time)", &state.verify);

        ImGui::Separator();
        bool canStart = !state.running && state.selectedReader >= 0 && state.authorizationConfirmed &&
                         state.caseId[0] != '\0' && (state.noPin || state.pin[0] != '\0');
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button("Start acquisition", ImVec2(200, 32))) {
            if (worker && worker->joinable()) worker->join();
            std::string readerName = state.readers[state.selectedReader];
            worker = std::make_unique<std::thread>(runAcquisition, &state, readerName);
        }
        ImGui::EndDisabled();

        if (state.running) {
            ImGui::SameLine();
            ImGui::Text("Acquiring... this can take a while for a full dump.");
        }

        if (state.lastZipPath) {
            ImGui::SameLine();
            if (ImGui::Button("Open output folder")) {
                openPath(state.outputDir);
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
