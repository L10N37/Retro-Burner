#include "burn_ui_linux.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

namespace {

static const SDL_DialogFileFilter kPs2CdFilters[] = {
    {"PlayStation 2 CD ISO", "iso"},
    {"All files", "*"},
};

[[nodiscard]] bool HasIsoPath(
    const std::array<char, 4096>& path)
{
    return path[0] != '\0';
}

} // namespace

LinuxBurnUiState::LinuxBurnUiState()
{
    isoPath_.fill('\0');
}

void SDLCALL LinuxBurnUiState::FileDialogCallback(
    void* userdata,
    const char* const* filelist,
    int)
{
    auto* self =
        static_cast<LinuxBurnUiState*>(userdata);

    if (self == nullptr)
        return;

    std::lock_guard lock(self->dialogMutex_);

    self->pendingDialogPath_.clear();
    self->pendingDialogError_.clear();
    self->pendingDialogReady_ = true;

    if (filelist == nullptr) {
        const char* error = SDL_GetError();
        self->pendingDialogError_ =
            error != nullptr && *error != '\0'
                ? error
                : "SDL file dialog failed.";
        return;
    }

    if (*filelist != nullptr)
        self->pendingDialogPath_ = *filelist;
}

void LinuxBurnUiState::OpenIsoDialog(
    SDL_Window* window)
{
    SDL_ShowOpenFileDialog(
        &LinuxBurnUiState::FileDialogCallback,
        this,
        window,
        kPs2CdFilters,
        static_cast<int>(
            sizeof(kPs2CdFilters) /
            sizeof(kPs2CdFilters[0])),
        nullptr,
        false);
}

void LinuxBurnUiState::ConsumeDialogResult()
{
    std::lock_guard lock(dialogMutex_);

    if (!pendingDialogReady_)
        return;

    pendingDialogReady_ = false;

    if (!pendingDialogError_.empty()) {
        uiMessage_ =
            "File dialog error: " +
            pendingDialogError_;
        return;
    }

    if (pendingDialogPath_.empty())
        return;

    if (pendingDialogPath_.size() >= isoPath_.size()) {
        uiMessage_ =
            "Selected path is too long for this build.";
        return;
    }

    isoPath_.fill('\0');
    std::memcpy(
        isoPath_.data(),
        pendingDialogPath_.data(),
        pendingDialogPath_.size());

    uiMessage_.clear();
}

bool LinuxBurnUiState::StartRequest(
    BurnEngine& engine,
    const OpticalDrive& drive,
    const bool simulate)
{
    if (!HasIsoPath(isoPath_)) {
        uiMessage_ =
            "Choose a PlayStation 2 CD ISO first.";
        return false;
    }

    try {
        BurnRequest request;
        request.cdiPath =
            std::filesystem::path(
                isoPath_.data()).wstring();
        request.target = BurnTarget::PlayStation2Cd;
        request.cdrecordDevice = drive.cdrecordDevice;
        request.requestedSpeedX = 16;
        request.advanced.burnFree = burnFree_;
        request.simulate = simulate;

        if (!engine.Start(std::move(request))) {
            uiMessage_ = "BurnEngine is already busy.";
            return false;
        }
    } catch (const std::exception& error) {
        uiMessage_ =
            std::string("Could not use selected path: ") +
            error.what();
        return false;
    }

    uiMessage_.clear();

    if (!simulate)
        armPhysicalWrite_ = false;

    return true;
}

void LinuxBurnUiState::Render(
    SDL_Window* window,
    BurnEngine& engine,
    const std::vector<OpticalDrive>& drives,
    const int selectedDrive)
{
    ConsumeDialogResult();

    BurnSnapshot snapshot = engine.Snapshot();

    if (snapshot.stage == BurnStage::Complete ||
        snapshot.stage == BurnStage::Failed) {
        armPhysicalWrite_ = false;
    }

    ImGui::Spacing();
    ImGui::SeparatorText("PlayStation 2 CD ISO");

    ImGui::TextWrapped(
        "RetroBeam SAO recording with blank-CD-R/NWA/capacity guards "
        "and direct SG_IO verification.");

    const bool validDrive =
        selectedDrive >= 0 &&
        selectedDrive < static_cast<int>(drives.size());

    if (validDrive) {
        const OpticalDrive& drive =
            drives[static_cast<std::size_t>(selectedDrive)];

        ImGui::Text(
            "Writer: %s %s",
            drive.vendor.c_str(),
            drive.product.c_str());
        ImGui::Text(
            "RetroBeam device: %s",
            drive.cdrecordDevice.c_str());
    } else {
        ImGui::TextUnformatted(
            "Writer: no optical drive selected");
    }

    if (snapshot.busy)
        ImGui::BeginDisabled();

    if (ImGui::Button("Choose PS2 CD ISO..."))
        OpenIsoDialog(window);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputText(
        "##ps2cd_iso_path",
        isoPath_.data(),
        isoPath_.size());

    ImGui::Checkbox("BURN-Free", &burnFree_);
    ImGui::TextUnformatted(
        "Write speed: 16x (hardware-validated Linux path)");

    if (snapshot.busy)
        ImGui::EndDisabled();

    const bool requestReady =
        validDrive &&
        HasIsoPath(isoPath_) &&
        !snapshot.busy;

    if (!requestReady)
        ImGui::BeginDisabled();

    if (ImGui::Button("Dummy test")) {
        const OpticalDrive& drive =
            drives[static_cast<std::size_t>(selectedDrive)];
        (void)StartRequest(engine, drive, true);
    }

    if (!requestReady)
        ImGui::EndDisabled();

    ImGui::SameLine();

    if (!snapshot.busy) {
        ImGui::Checkbox(
            "ARM PHYSICAL WRITE",
            &armPhysicalWrite_);
    } else {
        bool disabledArm = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox(
            "ARM PHYSICAL WRITE",
            &disabledArm);
        ImGui::EndDisabled();
    }

    const bool physicalReady =
        requestReady && armPhysicalWrite_;

    if (!physicalReady)
        ImGui::BeginDisabled();

    if (ImGui::Button("BURN PS2 CD")) {
        const OpticalDrive& drive =
            drives[static_cast<std::size_t>(selectedDrive)];
        (void)StartRequest(engine, drive, false);
    }

    if (!physicalReady)
        ImGui::EndDisabled();

    if (!uiMessage_.empty())
        ImGui::TextWrapped("%s", uiMessage_.c_str());

    snapshot = engine.Snapshot();

    ImGui::Spacing();
    ImGui::ProgressBar(
        std::clamp(snapshot.progress, 0.0F, 1.0F),
        ImVec2(-1.0F, 0.0F));

    ImGui::TextWrapped(
        "Status: %s",
        snapshot.status.c_str());

    if (!snapshot.actualSpeed.empty())
        ImGui::Text("Speed: %s", snapshot.actualSpeed.c_str());

    if (!snapshot.remainingTime.empty()) {
        ImGui::SameLine();
        ImGui::Text(
            "Remaining: %s",
            snapshot.remainingTime.c_str());
    }

    if (snapshot.ringBufferPercent >= 0)
        ImGui::Text("FIFO: %d%%", snapshot.ringBufferPercent);

    if (snapshot.driveBufferPercent >= 0) {
        ImGui::SameLine();
        ImGui::Text(
            "Drive buffer: %d%%",
            snapshot.driveBufferPercent);
    }

    if (!snapshot.log.empty()) {
        ImGui::BeginChild(
            "ps2cd_burn_log",
            ImVec2(0.0F, 220.0F),
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_HorizontalScrollbar);

        ImGui::TextUnformatted(snapshot.log.c_str());

        if (snapshot.busy)
            ImGui::SetScrollHereY(1.0F);

        ImGui::EndChild();
    }

    if (!snapshot.busy &&
        snapshot.stage != BurnStage::Idle) {
        if (ImGui::Button("Clear burn result")) {
            engine.Reset();
            uiMessage_.clear();
            armPhysicalWrite_ = false;
        }
    }
}
