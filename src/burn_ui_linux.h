#pragma once

#include "burn_engine.h"
#include "drive_manager.h"

#include <SDL3/SDL_video.h>

#include <array>
#include <mutex>
#include <string>
#include <vector>

class LinuxBurnUiState final {
public:
    LinuxBurnUiState();

    void Render(
        SDL_Window* window,
        BurnEngine& engine,
        const std::vector<OpticalDrive>& drives,
        int selectedDrive);

private:
    static void SDLCALL FileDialogCallback(
        void* userdata,
        const char* const* filelist,
        int filter);

    void OpenIsoDialog(SDL_Window* window);
    void ConsumeDialogResult();
    bool StartRequest(
        BurnEngine& engine,
        const OpticalDrive& drive,
        bool simulate);

    std::mutex dialogMutex_;
    std::string pendingDialogPath_;
    std::string pendingDialogError_;
    bool pendingDialogReady_ = false;

    std::array<char, 4096> isoPath_{};
    bool burnFree_ = true;
    bool armPhysicalWrite_ = false;
    std::string uiMessage_;
};
