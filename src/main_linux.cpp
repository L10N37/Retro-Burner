#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3_image/SDL_image.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include "burn_engine.h"
#include "embedded_bundle_linux.h"
#include "drive_manager.h"
#include "optical_verify_linux.h"
#include "process_runner.h"
#include "retrobeam_linux.h"
#include "texture_loader_linux.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <wchar.h>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

SDL_Window* g_pickerWindow = nullptr;
SDL_AudioStream* g_burnCompleteAudioStream = nullptr;

enum class ConsoleProfile : int {
    Dreamcast = 0,
    PlayStation,
    PlayStation2Cd,
    PlayStation2Dvd,
    Saturn,
    Xbox360,
    Count,
};

constexpr int kConsoleProfileCount =
    static_cast<int>(ConsoleProfile::Count);

constexpr std::uint64_t kDvdSingleLayerBytes =
    4707319808ULL;

[[nodiscard]] const char* ConsoleName(
    const ConsoleProfile profile)
{
    switch (profile) {
    case ConsoleProfile::Dreamcast:
        return "Dreamcast";
    case ConsoleProfile::PlayStation:
        return "PlayStation";
    case ConsoleProfile::PlayStation2Cd:
        return "PlayStation 2 - CD";
    case ConsoleProfile::PlayStation2Dvd:
        return "PlayStation 2 - DVD";
    case ConsoleProfile::Saturn:
        return "Sega Saturn";
    case ConsoleProfile::Xbox360:
        return "Xbox 360";
    default:
        return "Unknown";
    }
}

[[nodiscard]] const char* ConsoleSubtitle(
    const ConsoleProfile profile)
{
    // Deliberately identical to Windows. Both shipping builds are native x64.
    switch (profile) {
    case ConsoleProfile::Dreamcast:
        return "native x64  |  CDI to CD-R";
    case ConsoleProfile::PlayStation:
        return "native x64  |  BIN/CUE to CD-R";
    case ConsoleProfile::PlayStation2Cd:
        return "native x64  |  CUE/ISO to CD-R";
    case ConsoleProfile::PlayStation2Dvd:
        return "native x64  |  ISO to DVD / DVD-DL";
    case ConsoleProfile::Saturn:
        return "native x64  |  BIN/CUE to CD-R";
    case ConsoleProfile::Xbox360:
        return "native x64  |  XGD2/XGD3 ISO to DVD+R DL";
    default:
        return "native x64";
    }
}

[[nodiscard]] BurnTarget ToBurnTarget(
    const ConsoleProfile profile)
{
    switch (profile) {
    case ConsoleProfile::Dreamcast:
        return BurnTarget::Dreamcast;
    case ConsoleProfile::PlayStation:
        return BurnTarget::PlayStation;
    case ConsoleProfile::PlayStation2Cd:
        return BurnTarget::PlayStation2Cd;
    case ConsoleProfile::PlayStation2Dvd:
        return BurnTarget::PlayStation2Dvd;
    case ConsoleProfile::Saturn:
        return BurnTarget::Saturn;
    case ConsoleProfile::Xbox360:
        return BurnTarget::Xbox360;
    default:
        return BurnTarget::Dreamcast;
    }
}

[[nodiscard]] bool IsDvdProfile(
    const ConsoleProfile profile)
{
    return
        profile == ConsoleProfile::PlayStation2Dvd ||
        profile == ConsoleProfile::Xbox360;
}

[[nodiscard]] const char* Xbox360DiscTypeName(
    const Xbox360DiscType type)
{
    return
        type == Xbox360DiscType::Xgd3
            ? "XGD3 - BurnerMAX"
            : "XGD2 / standard Xbox 360";
}

[[nodiscard]] const char* ImageHint(
    const ConsoleProfile profile)
{
    switch (profile) {
    case ConsoleProfile::Dreamcast:
        return "Drop a .cdi here or choose Browse";
    case ConsoleProfile::PlayStation:
        return "Drop the game's .cue here or choose Browse";
    case ConsoleProfile::PlayStation2Cd:
        return "Drop a .cue or .iso here or choose Browse";
    case ConsoleProfile::PlayStation2Dvd:
        return "Drop a PS2 DVD .iso here or choose Browse";
    case ConsoleProfile::Saturn:
        return "Drop the game's .cue here or choose Browse";
    case ConsoleProfile::Xbox360:
        return "Drop an Xbox 360 .iso here or choose Browse";
    default:
        return "Choose a disc image";
    }
}

[[nodiscard]] const char* ExpectedMediaName(
    const ConsoleProfile profile)
{
    switch (profile) {
    case ConsoleProfile::PlayStation2Dvd:
        return "blank DVD-R / DVD+R or dual-layer DVD";
    case ConsoleProfile::Xbox360:
        return "blank DVD+R DL";
    default:
        return "blank CD-R";
    }
}

struct AppState final {
    std::wstring selectedCdi;
    std::vector<OpticalDrive> drives;
    int selectedDrive = 0;
    int selectedSpeed = 0;
    ConsoleProfile selectedConsole =
        ConsoleProfile::Dreamcast;
    Xbox360DiscType xbox360DiscType =
        Xbox360DiscType::Xgd2;
    bool useGrowisofsForDvd = false;
    // RB_STAGE44M_VERIFY_UI_STATE
    bool verifyAfterBurn = false;
    RetroBeamAdvancedOptions advanced;
    std::string status =
        "Choose a disc image and insert compatible blank media.";
    BurnStage lastBurnStage = BurnStage::Idle;

    std::mutex dialogMutex;
    bool dialogReady = false;
    std::string dialogPath;
    std::string dialogError;
};

[[nodiscard]] std::string WideToUtf8(
    const std::wstring& value)
{
    if (value.empty())
        return {};

    std::wstring_convert<
        std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(value);
}

[[nodiscard]] std::wstring Utf8ToWide(
    const std::string& value)
{
    if (value.empty())
        return {};

    std::wstring_convert<
        std::codecvt_utf8<wchar_t>> converter;
    return converter.from_bytes(value);
}

[[nodiscard]] std::wstring LowerExtension(
    const std::wstring& path)
{
    const std::size_t dot =
        path.find_last_of(L'.');

    if (dot == std::wstring::npos)
        return {};

    std::wstring extension =
        path.substr(dot);

    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const wchar_t c) {
            return static_cast<wchar_t>(
                std::towlower(c));
        });

    return extension;
}

[[nodiscard]] bool IsSupportedImagePath(
    const ConsoleProfile profile,
    const std::wstring& path)
{
    const std::wstring extension =
        LowerExtension(path);

    switch (profile) {
    case ConsoleProfile::Dreamcast:
        return extension == L".cdi";
    case ConsoleProfile::PlayStation:
    case ConsoleProfile::Saturn:
        return extension == L".cue";
    case ConsoleProfile::PlayStation2Cd:
        return
            extension == L".cue" ||
            extension == L".iso";
    case ConsoleProfile::PlayStation2Dvd:
    case ConsoleProfile::Xbox360:
        return extension == L".iso";
    default:
        return false;
    }
}

[[nodiscard]] fs::path FsPath(
    const std::wstring& path)
{
    return fs::path(
        WideToUtf8(path));
}

[[nodiscard]] bool FileExists(
    const std::wstring& path)
{
    std::error_code error;
    return fs::is_regular_file(
        FsPath(path),
        error);
}

[[nodiscard]] std::uint64_t FileSizeBytes(
    const std::wstring& path)
{
    std::error_code error;
    const std::uintmax_t bytes =
        fs::file_size(
            FsPath(path),
            error);

    return error
        ? 0ULL
        : static_cast<std::uint64_t>(
              bytes);
}

[[nodiscard]] bool SelectedPs2ImageNeedsDualLayer(
    const AppState& state)
{
    return
        state.selectedConsole ==
            ConsoleProfile::PlayStation2Dvd &&
        FileSizeBytes(
            state.selectedCdi) >
            kDvdSingleLayerBytes;
}

[[nodiscard]] int _wcsicmp(
    const wchar_t* left,
    const wchar_t* right)
{
    return ::wcscasecmp(left, right);
}

void SelectCdi(
    AppState& state,
    BurnEngine& burnEngine,
    std::wstring path)
{
    if (!IsSupportedImagePath(
            state.selectedConsole,
            path)) {
        state.status =
            std::string("Unsupported image for ") +
            ConsoleName(
                state.selectedConsole) +
            ".";
        return;
    }

    if (!FileExists(path)) {
        state.status =
            "Linux cannot find the selected disc image.";
        return;
    }

    state.selectedCdi =
        std::move(path);
    state.selectedSpeed = 0;
    state.verifyAfterBurn = false;
    burnEngine.Reset();

    state.status =
        std::string(
            ConsoleName(
                state.selectedConsole)) +
        " image selected. Run Check Image before burning to " +
        ExpectedMediaName(
            state.selectedConsole) +
        ".";
}

void SDLCALL FileDialogCallback(
    void* userdata,
    const char* const* filelist,
    int)
{
    auto* state =
        static_cast<AppState*>(userdata);

    if (state == nullptr)
        return;

    std::lock_guard lock(
        state->dialogMutex);

    state->dialogReady = true;
    state->dialogPath.clear();
    state->dialogError.clear();

    if (filelist == nullptr) {
        const char* error =
            SDL_GetError();

        state->dialogError =
            error != nullptr
                ? error
                : "File dialog failed.";
        return;
    }

    if (*filelist != nullptr)
        state->dialogPath = *filelist;
}

void ShowCdiPicker(
    AppState& state,
    BurnEngine&)
{
    if (g_pickerWindow == nullptr) {
        state.status =
            "Linux could not open the file picker.";
        return;
    }

    static const SDL_DialogFileFilter
        dreamcastFilters[] = {
            {"Dreamcast CDI images", "cdi"},
            {"All files", "*"},
        };

    static const SDL_DialogFileFilter
        playStationFilters[] = {
            {"PlayStation CUE sheets", "cue"},
            {"All files", "*"},
        };

    static const SDL_DialogFileFilter
        ps2CdFilters[] = {
            {"PS2 CD images", "cue;iso"},
            {"CUE sheets", "cue"},
            {"ISO images", "iso"},
            {"All files", "*"},
        };

    static const SDL_DialogFileFilter
        ps2DvdFilters[] = {
            {"PS2 DVD ISO images", "iso"},
            {"All files", "*"},
        };

    static const SDL_DialogFileFilter
        saturnFilters[] = {
            {"Sega Saturn CUE sheets", "cue"},
            {"All files", "*"},
        };

    static const SDL_DialogFileFilter
        xbox360Filters[] = {
            {"Xbox 360 ISO images", "iso"},
            {"All files", "*"},
        };

    const SDL_DialogFileFilter* filters =
        ps2DvdFilters;
    int count =
        static_cast<int>(
            std::size(ps2DvdFilters));

    switch (state.selectedConsole) {
    case ConsoleProfile::Dreamcast:
        filters = dreamcastFilters;
        count =
            static_cast<int>(
                std::size(
                    dreamcastFilters));
        break;

    case ConsoleProfile::PlayStation:
        filters = playStationFilters;
        count =
            static_cast<int>(
                std::size(
                    playStationFilters));
        break;

    case ConsoleProfile::PlayStation2Cd:
        filters = ps2CdFilters;
        count =
            static_cast<int>(
                std::size(
                    ps2CdFilters));
        break;

    case ConsoleProfile::PlayStation2Dvd:
        filters = ps2DvdFilters;
        count =
            static_cast<int>(
                std::size(
                    ps2DvdFilters));
        break;

    case ConsoleProfile::Saturn:
        filters = saturnFilters;
        count =
            static_cast<int>(
                std::size(
                    saturnFilters));
        break;

    case ConsoleProfile::Xbox360:
        filters = xbox360Filters;
        count =
            static_cast<int>(
                std::size(
                    xbox360Filters));
        break;

    default:
        break;
    }

    SDL_ShowOpenFileDialog(
        FileDialogCallback,
        &state,
        g_pickerWindow,
        filters,
        count,
        nullptr,
        false);
}

void ConsumeDialogResult(
    AppState& state,
    BurnEngine& engine)
{
    std::string selected;
    std::string error;

    {
        std::lock_guard lock(
            state.dialogMutex);

        if (!state.dialogReady)
            return;

        state.dialogReady = false;
        selected =
            std::move(
                state.dialogPath);
        error =
            std::move(
                state.dialogError);
    }

    if (!error.empty()) {
        state.status =
            "The image picker closed with an error: " +
            error;
        return;
    }

    if (!selected.empty()) {
        SelectCdi(
            state,
            engine,
            Utf8ToWide(selected));
    }
}

[[nodiscard]] fs::path ExecutableDirectory()
{
    std::array<char, 4096> buffer{};

    const ssize_t length =
        ::readlink(
            "/proc/self/exe",
            buffer.data(),
            buffer.size() - 1U);

    if (length <= 0)
        return fs::current_path();

    buffer[
        static_cast<std::size_t>(
            length)] = '\0';

    return
        fs::path(buffer.data())
            .parent_path();
}

void EnrichDriveWithRetroBeam(
    OpticalDrive& drive)
{
    const fs::path retrobeam =
        FindRetroBeamExecutable();

    if (retrobeam.empty())
        return;

    const CapturedProcessResult minfo =
        RunProcessCapture(
            retrobeam,
            {
                "dev=" +
                    drive.cdrecordDevice,
                "-minfo",
            },
            retrobeam.parent_path());

    const CapturedProcessResult prcap =
        RunProcessCapture(
            retrobeam,
            {
                "dev=" +
                    drive.cdrecordDevice,
                "-prcap",
            },
            retrobeam.parent_path());

    const std::string output =
        minfo.output +
        "\n" +
        prcap.output;

    std::smatch match;

    static const std::regex mediaPattern(
        R"(Mounted media type:\s+([^\r\n]+))");
    static const std::regex currentMediaPattern(
        R"(Current:\s+([^\r\n]+))",
        std::regex::icase);
    static const std::regex diskPattern(
        R"(disk status:\s+([A-Za-z]+))");
    static const std::regex speedPattern(
        R"(\[RBMI\] wspd index=[0-9]+ write_kbps=([0-9]+))");
    static const std::regex opcPattern(
        R"(\[RBMI\] opc_descriptors=([0-9]+))");
    static const std::regex bufferPattern(
        R"((?:Drive buffer|Buffer size):\s*([0-9]+)\s*(?:KB|kB))",
        std::regex::icase);

    std::string media;

    if (std::regex_search(
            output,
            match,
            mediaPattern)) {
        media = match[1].str();

        while (!media.empty() &&
               std::isspace(
                   static_cast<unsigned char>(
                       media.back()))) {
            media.pop_back();
        }

        drive.mediaPresent = true;
    } else {
        drive.mediaPresent = false;
    }

    // Stage 40: MMC Current profile description outranks legacy mounted/book text.
    //
    // Some drives/media combinations (observed with SONY16D1 in a
    // Slimtype eBAU108) expose a legacy DVD-ROM/book-type description while
    // the MMC current profile is correctly DVD-R Sequential.  The current
    // profile is authoritative for burn eligibility.
    std::smatch currentMediaMatch;

    if (std::regex_search(
            output,
            currentMediaMatch,
            currentMediaPattern)) {
        std::string currentMedia =
            currentMediaMatch[1].str();

        while (!currentMedia.empty() &&
               std::isspace(
                   static_cast<unsigned char>(
                       currentMedia.back()))) {
            currentMedia.pop_back();
        }

        std::string upperCurrent =
            currentMedia;

        std::transform(
            upperCurrent.begin(),
            upperCurrent.end(),
            upperCurrent.begin(),
            [](const unsigned char c) {
                return static_cast<char>(
                    std::toupper(c));
            });

        // Only replace the legacy description when "Current:" actually
        // describes an optical MMC profile.  Do not replace useful media
        // text with "none", "unknown", or another non-profile status.
        if (upperCurrent.find("DVD") !=
                std::string::npos ||
            upperCurrent.find("CD-") !=
                std::string::npos ||
            upperCurrent.find("BD-") !=
                std::string::npos) {
            media =
                std::move(
                    currentMedia);
            drive.mediaPresent = true;
        }
    }

    std::string diskStatus;

    if (std::regex_search(
            output,
            match,
            diskPattern)) {
        diskStatus = match[1].str();
        drive.blankMediaKnown = true;
        drive.blankMedia =
            diskStatus == "empty";
    }

    if (!media.empty()) {
        drive.mediaDescription = media;

        if (!diskStatus.empty()) {
            drive.mediaDescription +=
                drive.blankMedia
                    ? " - blank"
                    : " - " +
                        diskStatus;
        }

        // RetroBeam/cdrecord media names vary slightly by drive/driver.
        // In particular DVD+R DL is commonly printed as "DVD+R/DL".
        // Normalize separators before mapping the MMC current profile so the
        // shared Windows DrawApp can apply the exact same media gates.
        std::string normalizedMedia;
        normalizedMedia.reserve(
            media.size());

        for (const unsigned char c : media) {
            if (c == ' ' ||
                c == '/' ||
                c == '-' ||
                c == '_' ||
                c == '\t') {
                continue;
            }

            normalizedMedia.push_back(
                static_cast<char>(
                    std::toupper(c)));
        }

        // Test dual-layer forms before their single-layer prefixes.
        if (normalizedMedia.find("DVD+RDL") !=
            std::string::npos) {
            // MMC profile 002Bh: DVD+R Double Layer.
            drive.currentProfile =
                0x002B;
        } else if (
            normalizedMedia.find("DVD-RDL") !=
                std::string::npos ||
            normalizedMedia.find("DVDRDL") !=
                std::string::npos) {
            // MMC profile 0015h: DVD-R Dual Layer Sequential.
            drive.currentProfile =
                0x0015;
        } else if (
            normalizedMedia.find("DVD+R") !=
            std::string::npos) {
            // MMC profile 001Bh: DVD+R.
            drive.currentProfile =
                0x001B;
        } else if (
            normalizedMedia.find("DVDR") !=
                std::string::npos) {
            // MMC profile 0011h: DVD-R Sequential.
            drive.currentProfile =
                0x0011;
        } else if (
            normalizedMedia.find("CDR") !=
                std::string::npos) {
            // MMC profile 0009h: CD-R.
            drive.currentProfile =
                0x0009;
        }
    } else {
        drive.mediaDescription =
            "No disc / not ready";
    }

    drive.burnFreeSupported =
        output.find("BURNFREE") !=
        std::string::npos;

    drive.forceSpeedSupported =
        output.find("FORCESPEED") !=
        std::string::npos;

    drive.retrobeamCapabilitiesKnown =
        prcap.started &&
        prcap.exitCode == 0;

    drive.realTimeStreamingKnown =
        output.find(
            "[RBMI] realtime_streaming=") !=
        std::string::npos;

    drive.streamRecordingSupported =
        output.find(
            "stream_recording:1") !=
        std::string::npos;

    drive.getPerformanceWriteSpeedSupported =
        output.find(
            "get_performance_wspd:1") !=
        std::string::npos;

    drive.modePage2AWriteSpeedSupported =
        output.find(
            "mode_page_2a_wspd:1") !=
        std::string::npos;

    drive.setCdSpeedSupported =
        output.find(
            "set_cd_speed:1") !=
        std::string::npos;

    drive.readBufferCapacitySupported =
        output.find(
            "read_buffer_capacity:1") !=
        std::string::npos;

    if (std::regex_search(
            output,
            match,
            opcPattern)) {
        drive.opcDescriptorCountKnown = true;
        drive.opcDescriptorCount =
            static_cast<std::uint32_t>(
                std::stoul(
                    match[1].str()));
    }

    if (std::regex_search(
            output,
            match,
            bufferPattern)) {
        drive.driveBufferCapacityKnown = true;
        drive.driveBufferCapacityBytes =
            static_cast<std::uint32_t>(
                std::stoul(
                    match[1].str()) *
                1024ULL);
        drive.driveBufferAvailableBytes =
            drive.driveBufferCapacityBytes;
    }

    drive.writeSpeeds.clear();

    std::vector<std::uint32_t> seen;

    for (std::sregex_iterator it(
             output.begin(),
             output.end(),
             speedPattern),
         end;
         it != end;
         ++it) {
        const std::uint32_t kbps =
            static_cast<std::uint32_t>(
                std::stoul(
                    (*it)[1].str()));

        if (std::find(
                seen.begin(),
                seen.end(),
                kbps) !=
            seen.end()) {
            continue;
        }

        seen.push_back(kbps);

        WriteSpeed speed;
        speed.kilobytesPerSecond =
            kbps;
        speed.cdMultiplier =
            static_cast<float>(
                kbps) /
            176.4F;
        speed.rotation =
            "media/firmware default";

        drive.writeSpeeds.push_back(
            std::move(speed));
    }

    drive.speedQueryMessage =
        drive.writeSpeeds.empty()
            ? "Use Automatic or refresh with writable media inserted."
            : "RetroBeam reported " +
                  std::to_string(
                      drive.writeSpeeds.size()) +
                  " current-media write speeds.";

    drive.advancedCapabilityMessage =
        drive.retrobeamCapabilitiesKnown
            ? "RetroBeam capability probe loaded."
            : "Capability fingerprint not available.";
}

void RefreshDrives(
    AppState& state)
{
    std::wstring previousRoot;

    if (state.selectedDrive >= 0 &&
        state.selectedDrive <
            static_cast<int>(
                state.drives.size())) {
        previousRoot =
            state.drives[
                static_cast<std::size_t>(
                    state.selectedDrive)]
                .rootPath;
    }

    state.drives =
        EnumerateOpticalDrives();

    for (OpticalDrive& drive :
         state.drives) {
        EnrichDriveWithRetroBeam(
            drive);
    }

    state.selectedDrive = 0;
    state.selectedSpeed = 0;

    if (!previousRoot.empty()) {
        const auto match =
            std::find_if(
                state.drives.begin(),
                state.drives.end(),
                [&previousRoot](
                    const OpticalDrive& drive) {
                    return
                        drive.rootPath ==
                        previousRoot;
                });

        if (match !=
            state.drives.end()) {
            state.selectedDrive =
                static_cast<int>(
                    std::distance(
                        state.drives.begin(),
                        match));
        }
    }

    if (state.drives.empty()) {
        state.status =
            "No optical burners detected. Connect a drive and press Refresh.";
    } else if (
        state.drives.size() == 1) {
        state.status =
            "Detected 1 optical drive.";
    } else {
        state.status =
            "Detected " +
            std::to_string(
                state.drives.size()) +
            " optical drives.";
    }
}

[[nodiscard]] std::string FormatSpeed(
    const WriteSpeed& speed,
    const bool dvdSpeed)
{
    const float multiplierValue =
        dvdSpeed
            ? static_cast<float>(
                  speed.kilobytesPerSecond) /
                  1385.0F
            : speed.cdMultiplier;

    const long roundedMultiplier =
        std::lround(
            multiplierValue);

    std::string multiplier;

    if (std::abs(
            multiplierValue -
            static_cast<float>(
                roundedMultiplier)) <
        0.08F) {
        multiplier =
            std::to_string(
                roundedMultiplier) +
            "x";
    } else {
        char buffer[32]{};

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%.1fx",
            multiplierValue);

        multiplier =
            buffer;
    }

    return
        multiplier +
        "  (" +
        std::to_string(
            speed.kilobytesPerSecond) +
        " KB/s)";
}

[[nodiscard]] ImTextureID LinuxTextureId(
    const LinuxTexture& texture)
{
    return
        static_cast<ImTextureID>(
            static_cast<std::uintptr_t>(
                texture.id));
}

void DrawRotatedImage(
    const LinuxTexture& texture,
    const ImVec2 center,
    const float size,
    const float angleRadians)
{
    if (!texture.IsValid())
        return;

    const float half =
        size * 0.5F;
    const float cosine =
        std::cos(
            angleRadians);
    const float sine =
        std::sin(
            angleRadians);

    const auto rotate =
        [center,
         cosine,
         sine](
            const float x,
            const float y) {
            return ImVec2(
                center.x +
                    x * cosine -
                    y * sine,
                center.y +
                    x * sine +
                    y * cosine);
        };

    ImGui::GetWindowDrawList()->
        AddImageQuad(
            LinuxTextureId(texture),
            rotate(-half, -half),
            rotate(half, -half),
            rotate(half, half),
            rotate(-half, half),
            ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 0.0F),
            ImVec2(1.0F, 1.0F),
            ImVec2(0.0F, 1.0F));
}

void DrawCenteredSuccessText(
    const float availableWidth)
{
    constexpr const char* text =
        "Burn complete!";
    constexpr float tickSize =
        13.0F;
    constexpr float gap =
        7.0F;

    const float textWidth =
        ImGui::CalcTextSize(
            text).x;

    const float totalWidth =
        tickSize +
        gap +
        textWidth;

    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX() +
        std::max(
            0.0F,
            (availableWidth -
             totalWidth) *
                0.5F));

    const ImVec2 tickTopLeft =
        ImGui::GetCursorScreenPos();

    const float lineHeight =
        ImGui::GetTextLineHeight();

    const float centerY =
        tickTopLeft.y +
        lineHeight *
            0.50F;

    ImDrawList* drawList =
        ImGui::GetWindowDrawList();

    const ImU32 tickColor =
        ImGui::GetColorU32(
            ImGuiCol_Text);

    drawList->AddLine(
        ImVec2(
            tickTopLeft.x + 1.0F,
            centerY),
        ImVec2(
            tickTopLeft.x + 5.0F,
            centerY + 4.0F),
        tickColor,
        2.0F);

    drawList->AddLine(
        ImVec2(
            tickTopLeft.x + 5.0F,
            centerY + 4.0F),
        ImVec2(
            tickTopLeft.x + 12.0F,
            centerY - 5.0F),
        tickColor,
        2.0F);

    ImGui::Dummy(
        ImVec2(
            tickSize,
            lineHeight));

    ImGui::SameLine(
        0.0F,
        gap);

    ImGui::TextUnformatted(
        text);
}

void ConfigureImGuiStyle()
{
    ImGuiStyle& style =
        ImGui::GetStyle();

    style.WindowPadding =
        ImVec2(24.0F, 20.0F);
    style.FramePadding =
        ImVec2(12.0F, 9.0F);
    style.ItemSpacing =
        ImVec2(10.0F, 10.0F);
    style.ItemInnerSpacing =
        ImVec2(8.0F, 6.0F);
    style.WindowRounding = 0.0F;
    style.ChildRounding = 5.0F;
    style.FrameRounding = 4.0F;
    style.PopupRounding = 4.0F;
    style.ScrollbarRounding = 4.0F;
    style.GrabRounding = 3.0F;
    style.WindowBorderSize = 0.0F;
    style.ChildBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;

    ImVec4* colors =
        style.Colors;

    colors[ImGuiCol_Text] =
        ImVec4(1.00F, 1.00F, 1.00F, 1.00F);
    colors[ImGuiCol_TextDisabled] =
        ImVec4(0.52F, 0.52F, 0.52F, 1.00F);
    colors[ImGuiCol_WindowBg] =
        ImVec4(0.00F, 0.00F, 0.00F, 1.00F);
    colors[ImGuiCol_ChildBg] =
        ImVec4(0.00F, 0.00F, 0.00F, 1.00F);
    colors[ImGuiCol_PopupBg] =
        ImVec4(0.035F, 0.035F, 0.035F, 0.98F);
    colors[ImGuiCol_Border] =
        ImVec4(0.28F, 0.28F, 0.28F, 1.00F);
    colors[ImGuiCol_FrameBg] =
        ImVec4(0.055F, 0.055F, 0.055F, 1.00F);
    colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.11F, 0.11F, 0.11F, 1.00F);
    colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.17F, 0.17F, 0.17F, 1.00F);
    colors[ImGuiCol_TitleBg] =
        ImVec4(0.00F, 0.00F, 0.00F, 1.00F);
    colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.00F, 0.00F, 0.00F, 1.00F);
    colors[ImGuiCol_Button] =
        ImVec4(0.18F, 0.055F, 0.01F, 1.00F);
    colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.90F, 0.22F, 0.01F, 1.00F);
    colors[ImGuiCol_ButtonActive] =
        ImVec4(1.00F, 0.36F, 0.02F, 1.00F);
    colors[ImGuiCol_Header] =
        ImVec4(0.18F, 0.055F, 0.01F, 1.00F);
    colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.75F, 0.16F, 0.01F, 1.00F);
    colors[ImGuiCol_HeaderActive] =
        ImVec4(0.95F, 0.26F, 0.01F, 1.00F);
    colors[ImGuiCol_CheckMark] =
        ImVec4(1.00F, 0.32F, 0.02F, 1.00F);
    colors[ImGuiCol_SliderGrab] =
        ImVec4(1.00F, 0.25F, 0.01F, 1.00F);
    colors[ImGuiCol_SliderGrabActive] =
        ImVec4(1.00F, 0.43F, 0.04F, 1.00F);
    colors[ImGuiCol_Separator] =
        ImVec4(0.72F, 0.14F, 0.01F, 1.00F);
    colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.94F, 0.20F, 0.01F, 1.00F);
}

void DrawDriveDetails(
    const OpticalDrive& drive)
{
    ImGui::TextDisabled(
        "Firmware");
    ImGui::SameLine(
        110.0F);
    ImGui::TextUnformatted(
        drive.firmware.empty()
            ? "Not reported"
            : drive.firmware.c_str());

    ImGui::TextDisabled(
        "Connection");
    ImGui::SameLine(
        110.0F);
    ImGui::TextUnformatted(
        drive.bus.empty()
            ? "Unknown"
            : drive.bus.c_str());

    ImGui::TextDisabled(
        "Media");
    ImGui::SameLine(
        110.0F);

    ImVec4 mediaColor(
        1.00F,
        0.62F,
        0.24F,
        1.00F);

    if (drive.mediaPresent &&
        drive.blankMediaKnown &&
        drive.blankMedia &&
        (drive.currentProfile == 0 ||
         drive.currentProfile == 0x0009 ||
         drive.currentProfile == 0x0011 ||
         drive.currentProfile == 0x0015 ||
         drive.currentProfile == 0x0016 ||
         drive.currentProfile == 0x001B ||
         drive.currentProfile == 0x002B)) {
        mediaColor =
            ImVec4(
                0.45F,
                0.95F,
                0.50F,
                1.00F);
    } else if (
        drive.mediaPresent &&
        drive.blankMediaKnown &&
        !drive.blankMedia) {
        mediaColor =
            ImVec4(
                1.00F,
                0.32F,
                0.24F,
                1.00F);
    }

    ImGui::TextColored(
        mediaColor,
        "%s",
        drive.mediaDescription.empty()
            ? (drive.mediaPresent
                ? "Disc present"
                : "No disc / not ready")
            : drive.mediaDescription.c_str());
}

[[nodiscard]] int SelectedSpeedX(
    const AppState& state,
    const OpticalDrive* drive)
{
    if (drive == nullptr ||
        state.selectedSpeed <= 0 ||
        state.selectedSpeed >
            static_cast<int>(
                drive->writeSpeeds.size())) {
        return 0;
    }

    const WriteSpeed& speed =
        drive->writeSpeeds[
            static_cast<std::size_t>(
                state.selectedSpeed - 1)];

    if (IsDvdProfile(
            state.selectedConsole)) {
        return
            std::max(
                1,
                static_cast<int>(
                    std::lround(
                        static_cast<double>(
                            speed.kilobytesPerSecond) /
                        1385.0)));
    }

    return
        std::max(
            1,
            static_cast<int>(
                std::lround(
                    speed.cdMultiplier)));
}

[[nodiscard]] RetroBeamAdvancedOptions
EffectiveAdvancedOptions(
    const AppState& state,
    const OpticalDrive* drive)
{
    RetroBeamAdvancedOptions options =
        state.advanced;

    if (drive == nullptr) {
        options.burnFree = false;
        options.forceSpeed = false;
        options.useStreamingPolicy =
            false;
        return options;
    }

    if (!drive->burnFreeSupported)
        options.burnFree = false;

    if (!drive->forceSpeedSupported)
        options.forceSpeed = false;

    if (!IsDvdProfile(
            state.selectedConsole) ||
        !drive->realTimeStreamingKnown ||
        !drive->streamRecordingSupported ||
        (!drive->getPerformanceWriteSpeedSupported &&
         !drive->modePage2AWriteSpeedSupported)) {
        options.useStreamingPolicy =
            false;
    }

    return options;
}

[[nodiscard]] std::string FormatKiB(
    const std::uint32_t bytes)
{
    if (bytes >=
        1024U * 1024U) {
        char text[32]{};

        std::snprintf(
            text,
            sizeof(text),
            "%.1f MiB",
            static_cast<double>(
                bytes) /
                (1024.0 * 1024.0));

        return text;
    }

    return
        std::to_string(
            bytes / 1024U) +
        " KiB";
}

[[nodiscard]] const char*
OpcPolicyDisplayName(
    const RetroBeamOpcPolicy policy)
{
    switch (policy) {
    case RetroBeamOpcPolicy::Force:
        return "Force explicit OPC";
    case RetroBeamOpcPolicy::Skip:
        return "Skip explicit OPC";
    case RetroBeamOpcPolicy::Automatic:
    default:
        return "Automatic";
    }
}

// UI SOURCE OF TRUTH:
// This file is generated directly from src/main.cpp::DrawApp().
#include "draw_app_linux.generated.inl"

[[nodiscard]] bool ApplyRetroBurnerWindowIcon(
    SDL_Window* window)
{
    const fs::path iconPath =
        MaterializeEmbeddedLinuxResource(
            "RetroBurner.png");

    if (iconPath.empty())
        return false;

    SDL_Surface* icon =
        IMG_Load(
            iconPath.c_str());

    if (icon == nullptr)
        return false;

    const bool applied =
        SDL_SetWindowIcon(
            window,
            icon);

    SDL_DestroySurface(icon);
    return applied;
}
[[nodiscard]] bool LoadArtworkSet(
    std::array<
        LinuxTexture,
        kConsoleProfileCount>& artworks,
    LinuxTexture& disc)
{
    static constexpr std::array<
        const char*,
        kConsoleProfileCount> names = {
            "BurningDC.png",
            "BurningPS1.png",
            "BurningPS2.png",
            "BurningPS2.png",
            "BurningSaturn.png",
            "BurningXB360.png",
        };

    bool okay = true;

    for (std::size_t i = 0;
         i < names.size();
         ++i) {
        const fs::path imagePath =
            MaterializeEmbeddedLinuxResource(
                names[i]);

        if (imagePath.empty()) {
            okay = false;
            continue;
        }

        okay =
            LoadLinuxTexture(
                imagePath,
                artworks[i]) &&
            okay;
    }

    const fs::path discPath =
        MaterializeEmbeddedLinuxResource(
            "makeSpinDC.png");

    if (discPath.empty())
        return false;

    okay =
        LoadLinuxTexture(
            discPath,
            disc) &&
        okay;

    return okay;
}
void DestroyBurnCompleteSound()
{
    if (g_burnCompleteAudioStream == nullptr)
        return;

    SDL_DestroyAudioStream(
        g_burnCompleteAudioStream);

    g_burnCompleteAudioStream =
        nullptr;
}

[[nodiscard]] bool PlayBurnCompleteSound()
{
    DestroyBurnCompleteSound();

    if ((SDL_WasInit(
            SDL_INIT_AUDIO) &
         SDL_INIT_AUDIO) == 0) {
        return false;
    }

    const fs::path wavPath =
        MaterializeEmbeddedLinuxResource(
            "BurnComplete.wav");

    if (wavPath.empty())
        return false;

    SDL_AudioSpec sourceSpec{};
    Uint8* audioData = nullptr;
    Uint32 audioLength = 0;

    if (!SDL_LoadWAV(
            wavPath.c_str(),
            &sourceSpec,
            &audioData,
            &audioLength)) {
        return false;
    }

    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
            &sourceSpec,
            nullptr,
            nullptr);

    if (stream == nullptr) {
        SDL_free(
            audioData);
        return false;
    }

    const bool queued =
        SDL_PutAudioStreamData(
            stream,
            audioData,
            static_cast<int>(
                audioLength));

    SDL_free(
        audioData);

    if (!queued) {
        SDL_DestroyAudioStream(
            stream);
        return false;
    }

    if (!SDL_FlushAudioStream(
            stream)) {
        SDL_DestroyAudioStream(
            stream);
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(
            stream)) {
        SDL_DestroyAudioStream(
            stream);
        return false;
    }

    g_burnCompleteAudioStream =
        stream;

    return true;
}

void ServiceBurnCompleteSound()
{
    if (g_burnCompleteAudioStream == nullptr)
        return;

    const int queued =
        SDL_GetAudioStreamQueued(
            g_burnCompleteAudioStream);

    if (queued == 0)
        DestroyBurnCompleteSound();
}

int RunBurnCompleteSoundTest()
{
    if (!SDL_Init(
            SDL_INIT_AUDIO)) {
        std::fprintf(
            stderr,
            "SDL audio init failed: %s\n",
            SDL_GetError());
        return 60;
    }

    if (!PlayBurnCompleteSound()) {
        std::fprintf(
            stderr,
            "Burn-complete sound could not start: %s\n",
            SDL_GetError());
        SDL_Quit();
        return 61;
    }

    std::printf(
        "burn_complete_sound=start\n");

    const Uint64 started =
        SDL_GetTicks();

    while (g_burnCompleteAudioStream != nullptr &&
           SDL_GetTicks() - started < 15000ULL) {
        ServiceBurnCompleteSound();
        SDL_Delay(10);
    }

    const bool finished =
        g_burnCompleteAudioStream == nullptr;

    DestroyBurnCompleteSound();
    SDL_Quit();

    std::printf(
        "burn_complete_sound=%s\n",
        finished
            ? "finished"
            : "timeout");

    return
        finished
            ? 0
            : 62;
}

int RunGui(
    const bool smoke)
{
    SDL_SetAppMetadata(
        "Retro Burner",
        "0.4.0",
        "io.github.L10N37.RetroBurner");

    if (!SDL_Init(
            SDL_INIT_VIDEO)) {
        std::fprintf(
            stderr,
            "SDL_Init failed: %s\n",
            SDL_GetError());
        return 10;
    }

    const bool audioAvailable =
        SDL_InitSubSystem(
            SDL_INIT_AUDIO);

    if (!audioAvailable) {
        std::fprintf(
            stderr,
            "Retro Burner audio disabled: %s\n",
            SDL_GetError());
    }

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_MAJOR_VERSION,
        3);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_MINOR_VERSION,
        0);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(
        SDL_GL_DOUBLEBUFFER,
        1);

    SDL_WindowFlags flags =
        static_cast<SDL_WindowFlags>(
            SDL_WINDOW_OPENGL |
            SDL_WINDOW_RESIZABLE);

    if (smoke) {
        flags =
            static_cast<SDL_WindowFlags>(
                flags |
                SDL_WINDOW_HIDDEN);
    }

    SDL_Window* window =
        SDL_CreateWindow(
            "Retro Burner",
            1120,
            760,
            flags);

    if (window == nullptr) {
        std::fprintf(
            stderr,
            "SDL_CreateWindow failed: %s\n",
            SDL_GetError());
        SDL_Quit();
        return 11;
    }

    g_pickerWindow = window;

    const bool iconOkay =
        ApplyRetroBurnerWindowIcon(
            window);

    SDL_GLContext gl =
        SDL_GL_CreateContext(
            window);

    if (gl == nullptr) {
        SDL_DestroyWindow(
            window);
        SDL_Quit();
        return 12;
    }

    SDL_GL_MakeCurrent(
        window,
        gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureImGuiStyle();

    ImGuiIO& io =
        ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui_ImplSDL3_InitForOpenGL(
        window,
        gl);
    ImGui_ImplOpenGL3_Init(
        "#version 130");

    std::array<
        LinuxTexture,
        kConsoleProfileCount> artworks;
    LinuxTexture disc;

    const bool artworkOkay =
        LoadArtworkSet(
            artworks,
            disc);

    AppState state;
    BurnEngine burnEngine;
    RefreshDrives(state);

    BurnStage lastCompletionSoundStage =
        BurnStage::Idle;

    bool running = true;
    int smokeFrames = 0;

    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(
                   &event)) {
            ImGui_ImplSDL3_ProcessEvent(
                &event);

            if (event.type ==
                SDL_EVENT_QUIT) {
                running = false;
            } else if (
                event.type ==
                    SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID ==
                    SDL_GetWindowID(
                        window)) {
                running = false;
            } else if (
                event.type ==
                    SDL_EVENT_DROP_FILE &&
                event.drop.data !=
                    nullptr &&
                !burnEngine.Snapshot().busy) {
                SelectCdi(
                    state,
                    burnEngine,
                    Utf8ToWide(
                        event.drop.data));
            }
        }

        ConsumeDialogResult(
            state,
            burnEngine);

        const BurnSnapshot completionSoundSnapshot =
            burnEngine.Snapshot();

        if (completionSoundSnapshot.stage ==
                BurnStage::Complete &&
            lastCompletionSoundStage !=
                BurnStage::Complete) {
            if (!PlayBurnCompleteSound()) {
                std::fprintf(
                    stderr,
                    "Burn-complete sound could not start: %s\n",
                    SDL_GetError());
            }
        }

        lastCompletionSoundStage =
            completionSoundSnapshot.stage;

        ServiceBurnCompleteSound();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        DrawApp(
            state,
            burnEngine,
            artworks,
            disc);

        ImGui::Render();

        int width = 0;
        int height = 0;

        SDL_GetWindowSizeInPixels(
            window,
            &width,
            &height);

        glViewport(
            0,
            0,
            width,
            height);
        glClearColor(
            0.0F,
            0.0F,
            0.0F,
            1.0F);
        glClear(
            GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(
            ImGui::GetDrawData());

        SDL_GL_SwapWindow(
            window);

        if (smoke &&
            ++smokeFrames >= 4) {
            running = false;
        }
    }

    for (LinuxTexture& texture :
         artworks) {
        texture.Reset();
    }

    disc.Reset();

    const char* driver =
        SDL_GetCurrentVideoDriver();

    const std::string driverText =
        driver != nullptr
            ? driver
            : "unknown";

    DestroyBurnCompleteSound();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    g_pickerWindow = nullptr;

    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (smoke) {
        std::printf(
            "SDL video driver: %s\n",
            driverText.c_str());

        std::printf(
            "Windows DrawApp parity: generated\n");

        std::printf(
            "Original-style assets: %s\n",
            artworkOkay
                ? "loaded"
                : "FAILED");

        std::printf(
            "Static-disc app icon: %s\n",
            iconOkay
                ? "loaded"
                : "FAILED");
    }

    return
        artworkOkay && iconOkay
            ? 0
            : 13;
}

[[nodiscard]] fs::path FindCommand(
    const std::string& name)
{
    const char* raw =
        std::getenv("PATH");

    if (raw == nullptr)
        return {};

    std::stringstream paths(raw);
    std::string part;

    while (std::getline(
               paths,
               part,
               ':')) {
        if (part.empty())
            continue;

        const fs::path candidate =
            fs::path(part) /
            name;

        if (::access(
                candidate.c_str(),
                X_OK) == 0) {
            return candidate;
        }
    }

    return {};
}

int RunGrowisofsProbe()
{
    const fs::path growisofs =
        FindCommand(
            "growisofs");

    if (growisofs.empty())
        return 30;

    const CapturedProcessResult result =
        RunProcessCapture(
            growisofs,
            {"-version"});

    std::fwrite(
        result.output.data(),
        1,
        result.output.size(),
        stdout);

    std::printf(
        "growisofs_path=%s\n",
        growisofs.c_str());

    std::printf(
        "growisofs_exit=%d\n",
        result.exitCode);

    return
        result.started &&
        result.exitCode == 0
            ? 0
            : 31;
}

int RunBurnEnginePreflight(
    const std::string& device)
{
    BurnEngine engine;
    BurnRequest request;

    request.cdrecordDevice =
        device;
    request.checkOnly = true;

    if (!engine.Start(
            std::move(request))) {
        return 20;
    }

    for (;;) {
        const BurnSnapshot snapshot =
            engine.Snapshot();

        if (!snapshot.busy) {
            std::printf(
                "burnengine_stage=%d\n",
                static_cast<int>(
                    snapshot.stage));

            std::printf(
                "burnengine_status=%s\n",
                snapshot.status.c_str());

            return
                snapshot.stage ==
                        BurnStage::Complete
                    ? 0
                    : 21;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(
                20));
    }
}

int RunBurnerMaxCli(
    const std::string& blockDevice)
{
    BurnEngine engine;

    if (!engine.StartBurnerMaxTest(
            Utf8ToWide(
                blockDevice))) {
        return 40;
    }

    for (;;) {
        const BurnSnapshot snapshot =
            engine.Snapshot();

        if (!snapshot.busy) {
            std::printf(
                "burnermax_stage=%d\n",
                static_cast<int>(
                    snapshot.stage));

            std::printf(
                "burnermax_status=%s\n",
                snapshot.status.c_str());

            std::printf(
                "burnermax_log_begin\n%s\n"
                "burnermax_log_end\n",
                snapshot.log.c_str());

            return
                snapshot.stage ==
                        BurnStage::Ready
                    ? 0
                    : 41;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(
                25));
    }
}

} // namespace

int main(
    int argc,
    char** argv)
{
    bool smoke = false;

    for (int i = 1;
         i < argc;
         ++i) {
        if (std::strcmp(
                argv[i],
                "--version") == 0) {
            std::printf(
                "Retro Burner 0.4.0 Linux\n");
            return 0;
        }

        if (std::strcmp(
                argv[i],
                "--smoke-gui") == 0) {
            smoke = true;
            continue;
        }

        if (std::strcmp(
                argv[i],
                "--test-complete-sound") == 0) {
            return
                RunBurnCompleteSoundTest();
        }

        if (std::strcmp(
                argv[i],
                "--growisofs-probe") == 0) {
            return
                RunGrowisofsProbe();
        }

        if (std::strcmp(
                argv[i],
                "--burnengine-preflight") == 0 &&
            i + 1 < argc) {
            return
                RunBurnEnginePreflight(
                    argv[++i]);
        }

        if (std::strcmp(
                argv[i],
                "--burnermax-test") == 0 &&
            i + 1 < argc) {
            return
                RunBurnerMaxCli(
                    argv[++i]);
        }

        if (std::strcmp(
                argv[i],
                "--verify-raw") == 0 &&
            i + 2 < argc) {
            const std::string device =
                argv[++i];

            const fs::path source =
                argv[++i];

            const OpticalVerifyResult result =
                VerifyOpticalSectors(
                    device,
                    source,
                    0);

            const std::string text =
                result.ToText();

            std::fwrite(
                text.data(),
                1,
                text.size(),
                stdout);

            return
                result.success
                    ? 0
                    : 32;
        }
    }

    return
        RunGui(smoke);
}
