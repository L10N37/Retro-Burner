#include <windows.h>
#include <commdlg.h>
#include <dbt.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "burn_engine.h"
#include "drive_manager.h"
#include "resource.h"
#include "texture_loader.h"

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_deviceContext;
ComPtr<IDXGISwapChain> g_swapChain;
ComPtr<ID3D11RenderTargetView> g_renderTarget;
bool g_driveRefreshRequested = false;
bool g_jobInProgress = false;
std::wstring g_droppedPath;

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

constexpr std::uint64_t kDvdSingleLayerBytes = 4707319808ULL;

[[nodiscard]] const char* ConsoleName(const ConsoleProfile profile) {
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

[[nodiscard]] const char* ConsoleSubtitle(const ConsoleProfile profile) {
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
    const ConsoleProfile profile) {
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
    const ConsoleProfile profile) {
    return profile == ConsoleProfile::PlayStation2Dvd ||
           profile == ConsoleProfile::Xbox360;
}

[[nodiscard]] const char* Xbox360DiscTypeName(
    const Xbox360DiscType type) {
    return type == Xbox360DiscType::Xgd3
        ? "XGD3 - BurnerMAX"
        : "XGD2 / standard Xbox 360";
}

[[nodiscard]] const char* ImageHint(
    const ConsoleProfile profile) {
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
    const ConsoleProfile profile) {
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
    int selectedSpeed = 0; // 0 means Automatic.
    ConsoleProfile selectedConsole = ConsoleProfile::Dreamcast;
    Xbox360DiscType xbox360DiscType = Xbox360DiscType::Xgd2;
    // RetroBeam is the default DVD backend. growisofs remains selectable.
    bool useGrowisofsForDvd = false;
    RetroBeamAdvancedOptions advanced;
    std::string status = "Choose a disc image and insert compatible blank media.";
    BurnStage lastBurnStage = BurnStage::Idle;
};

[[nodiscard]] std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int byteCount = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(byteCount), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), byteCount, nullptr, nullptr);
    return result;
}

[[nodiscard]] std::wstring LowerExtension(
    const std::wstring& path) {
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) {
        return {};
    }

    std::wstring extension = path.substr(dot);
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return extension;
}

[[nodiscard]] bool IsSupportedImagePath(
    const ConsoleProfile profile,
    const std::wstring& path) {
    const std::wstring extension = LowerExtension(path);

    switch (profile) {
    case ConsoleProfile::Dreamcast:
        return extension == L".cdi";
    case ConsoleProfile::PlayStation:
    case ConsoleProfile::Saturn:
        return extension == L".cue";
    case ConsoleProfile::PlayStation2Cd:
        return extension == L".cue" ||
               extension == L".iso";
    case ConsoleProfile::PlayStation2Dvd:
    case ConsoleProfile::Xbox360:
        return extension == L".iso";
    default:
        return false;
    }
}

[[nodiscard]] bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

[[nodiscard]] std::uint64_t FileSizeBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(
            path.c_str(),
            GetFileExInfoStandard,
            &data)) {
        return 0;
    }
    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    return size.QuadPart;
}

[[nodiscard]] bool SelectedPs2ImageNeedsDualLayer(
    const AppState& state) {
    return state.selectedConsole == ConsoleProfile::PlayStation2Dvd &&
           FileSizeBytes(state.selectedCdi) > kDvdSingleLayerBytes;
}

void SelectCdi(
    AppState& state,
    BurnEngine& burnEngine,
    std::wstring path) {
    if (!IsSupportedImagePath(
            state.selectedConsole,
            path)) {
        state.status =
            std::string("Unsupported image for ") +
            ConsoleName(state.selectedConsole) +
            ".";
        return;
    }

    if (!FileExists(path)) {
        state.status =
            "Windows cannot find the selected disc image.";
        return;
    }

    if (state.selectedConsole ==
            ConsoleProfile::Dreamcast &&
        path.size() >= MAX_PATH) {
        state.status =
            "CDIrip requires a Dreamcast CDI path shorter than 260 characters. "
            "Move the image to a shorter path and retry.";
        return;
    }

    state.selectedCdi =
        std::move(path);
    state.selectedSpeed = 0;
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

void ShowCdiPicker(
    AppState& state,
    BurnEngine& burnEngine) {
    ComPtr<IFileOpenDialog> dialog;

    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        state.status =
            "Windows could not open the file picker.";
        return;
    }

    static const COMDLG_FILTERSPEC dreamcastFilters[] = {
        {L"Dreamcast CDI images", L"*.cdi"},
        {L"All files", L"*.*"},
    };

    static const COMDLG_FILTERSPEC playStationFilters[] = {
        {L"PlayStation CUE sheets", L"*.cue"},
        {L"All files", L"*.*"},
    };

    static const COMDLG_FILTERSPEC ps2CdFilters[] = {
        {L"PS2 CD images", L"*.cue;*.iso"},
        {L"CUE sheets", L"*.cue"},
        {L"ISO images", L"*.iso"},
        {L"All files", L"*.*"},
    };

    static const COMDLG_FILTERSPEC ps2DvdFilters[] = {
        {L"PS2 DVD ISO images", L"*.iso"},
        {L"All files", L"*.*"},
    };

    static const COMDLG_FILTERSPEC saturnFilters[] = {
        {L"Sega Saturn CUE sheets", L"*.cue"},
        {L"All files", L"*.*"},
    };

    static const COMDLG_FILTERSPEC xbox360Filters[] = {
        {L"Xbox 360 ISO images", L"*.iso"},
        {L"All files", L"*.*"},
    };

    switch (state.selectedConsole) {
    case ConsoleProfile::Dreamcast:
        dialog->SetFileTypes(
            static_cast<UINT>(
                std::size(
                    dreamcastFilters)),
            dreamcastFilters);
        dialog->SetTitle(
            L"Choose a Dreamcast CDI image");
        break;

    case ConsoleProfile::PlayStation:
        dialog->SetFileTypes(
            static_cast<UINT>(
                std::size(
                    playStationFilters)),
            playStationFilters);
        dialog->SetTitle(
            L"Choose a PlayStation CUE sheet");
        break;

    case ConsoleProfile::PlayStation2Cd:
        dialog->SetFileTypes(
            static_cast<UINT>(
                std::size(
                    ps2CdFilters)),
            ps2CdFilters);
        dialog->SetTitle(
            L"Choose a PlayStation 2 CD image");
        break;

    case ConsoleProfile::PlayStation2Dvd:
        dialog->SetFileTypes(
            static_cast<UINT>(
                std::size(
                    ps2DvdFilters)),
            ps2DvdFilters);
        dialog->SetTitle(
            L"Choose a PlayStation 2 DVD ISO");
        break;

    case ConsoleProfile::Saturn:
        dialog->SetFileTypes(
            static_cast<UINT>(
                std::size(
                    saturnFilters)),
            saturnFilters);
        dialog->SetTitle(
            L"Choose a Sega Saturn CUE sheet");
        break;

    case ConsoleProfile::Xbox360:
        dialog->SetFileTypes(
            static_cast<UINT>(
                std::size(
                    xbox360Filters)),
            xbox360Filters);
        dialog->SetTitle(
            L"Choose an Xbox 360 ISO image");
        break;

    default:
        break;
    }

    dialog->SetFileTypeIndex(1);

    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(
            dialog->GetOptions(
                &options))) {
        dialog->SetOptions(
            options |
            FOS_FORCEFILESYSTEM |
            FOS_FILEMUSTEXIST);
    }

    const HRESULT showResult =
        dialog->Show(nullptr);

    if (showResult ==
        HRESULT_FROM_WIN32(
            ERROR_CANCELLED)) {
        return;
    }

    if (FAILED(showResult)) {
        state.status =
            "The image picker closed with an error.";
        return;
    }

    ComPtr<IShellItem> selected;
    if (FAILED(
            dialog->GetResult(
                &selected))) {
        state.status =
            "Windows did not return the selected file.";
        return;
    }

    PWSTR path = nullptr;
    if (SUCCEEDED(
            selected->GetDisplayName(
                SIGDN_FILESYSPATH,
                &path)) &&
        path != nullptr) {
        SelectCdi(
            state,
            burnEngine,
            path);
        CoTaskMemFree(path);
    }
}
void RefreshDrives(AppState& state) {
    std::wstring previousRoot;
    if (state.selectedDrive >= 0 &&
        state.selectedDrive < static_cast<int>(state.drives.size())) {
        previousRoot = state.drives[static_cast<std::size_t>(state.selectedDrive)].rootPath;
    }

    state.drives = EnumerateOpticalDrives();
    state.selectedDrive = 0;
    state.selectedSpeed = 0;
    if (!previousRoot.empty()) {
        const auto match = std::find_if(
            state.drives.begin(),
            state.drives.end(),
            [&previousRoot](const OpticalDrive& drive) {
                return drive.rootPath == previousRoot;
            });
        if (match != state.drives.end()) {
            state.selectedDrive = static_cast<int>(
                std::distance(state.drives.begin(), match));
        }
    }

    if (state.drives.empty()) {
        state.status = "No optical burners detected. Connect a drive and press Refresh.";
    } else if (state.drives.size() == 1) {
        state.status = "Detected 1 optical drive.";
    } else {
        state.status = "Detected " + std::to_string(state.drives.size()) +
            " optical drives.";
    }
}

[[nodiscard]] std::string FormatSpeed(
    const WriteSpeed& speed,
    const bool dvdSpeed) {
    const float multiplierValue =
        dvdSpeed
            ? static_cast<float>(speed.kilobytesPerSecond) / 1385.0F
            : speed.cdMultiplier;
    const long roundedMultiplier =
        std::lround(multiplierValue);

    std::string multiplier;
    if (std::abs(
            multiplierValue -
            static_cast<float>(roundedMultiplier)) < 0.08F) {
        multiplier =
            std::to_string(roundedMultiplier) + "x";
    } else {
        char buffer[32]{};
        snprintf(
            buffer,
            sizeof(buffer),
            "%.1fx",
            multiplierValue);
        multiplier = buffer;
    }

    return multiplier + "  (" +
        std::to_string(speed.kilobytesPerSecond) +
        " KB/s)";
}

void DrawRotatedImage(
    const Texture& texture,
    const ImVec2 center,
    const float size,
    const float angleRadians) {
    if (!texture.IsValid()) {
        return;
    }

    const float half = size * 0.5F;
    const float cosine = std::cos(angleRadians);
    const float sine = std::sin(angleRadians);
    const auto rotate = [center, cosine, sine](const float x, const float y) {
        return ImVec2(
            center.x + x * cosine - y * sine,
            center.y + x * sine + y * cosine);
    };

    const ImVec2 topLeft = rotate(-half, -half);
    const ImVec2 topRight = rotate(half, -half);
    const ImVec2 bottomRight = rotate(half, half);
    const ImVec2 bottomLeft = rotate(-half, half);
    ImGui::GetWindowDrawList()->AddImageQuad(
        reinterpret_cast<ImTextureID>(texture.view.Get()),
        topLeft,
        topRight,
        bottomRight,
        bottomLeft,
        ImVec2(0.0F, 0.0F),
        ImVec2(1.0F, 0.0F),
        ImVec2(1.0F, 1.0F),
        ImVec2(0.0F, 1.0F));
}

void DrawCenteredSuccessText(const float availableWidth) {
    constexpr const char* text = "Burn complete!";
    constexpr float tickSize = 13.0F;
    constexpr float gap = 7.0F;

    const float textWidth = ImGui::CalcTextSize(text).x;
    const float totalWidth = tickSize + gap + textWidth;
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX() +
        std::max(0.0F, (availableWidth - totalWidth) * 0.5F));

    const ImVec2 tickTopLeft = ImGui::GetCursorScreenPos();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float centerY = tickTopLeft.y + lineHeight * 0.50F;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 tickColor = ImGui::GetColorU32(ImGuiCol_Text);
    drawList->AddLine(
        ImVec2(tickTopLeft.x + 1.0F, centerY),
        ImVec2(tickTopLeft.x + 5.0F, centerY + 4.0F),
        tickColor,
        2.0F);
    drawList->AddLine(
        ImVec2(tickTopLeft.x + 5.0F, centerY + 4.0F),
        ImVec2(tickTopLeft.x + 12.0F, centerY - 5.0F),
        tickColor,
        2.0F);

    ImGui::Dummy(ImVec2(tickSize, lineHeight));
    ImGui::SameLine(0.0F, gap);
    ImGui::TextUnformatted(text);
}

void ConfigureImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(24.0F, 20.0F);
    style.FramePadding = ImVec2(12.0F, 9.0F);
    style.ItemSpacing = ImVec2(10.0F, 10.0F);
    style.ItemInnerSpacing = ImVec2(8.0F, 6.0F);
    style.WindowRounding = 0.0F;
    style.ChildRounding = 5.0F;
    style.FrameRounding = 4.0F;
    style.PopupRounding = 4.0F;
    style.ScrollbarRounding = 4.0F;
    style.GrabRounding = 3.0F;
    style.WindowBorderSize = 0.0F;
    style.ChildBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(1.00F, 1.00F, 1.00F, 1.00F);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.52F, 0.52F, 0.52F, 1.00F);
    colors[ImGuiCol_WindowBg] = ImVec4(0.00F, 0.00F, 0.00F, 1.00F);
    colors[ImGuiCol_ChildBg] = ImVec4(0.00F, 0.00F, 0.00F, 1.00F);
    colors[ImGuiCol_PopupBg] = ImVec4(0.035F, 0.035F, 0.035F, 0.98F);
    colors[ImGuiCol_Border] = ImVec4(0.28F, 0.28F, 0.28F, 1.00F);
    colors[ImGuiCol_FrameBg] = ImVec4(0.055F, 0.055F, 0.055F, 1.00F);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.11F, 0.11F, 0.11F, 1.00F);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.17F, 0.17F, 0.17F, 1.00F);
    colors[ImGuiCol_TitleBg] = ImVec4(0.00F, 0.00F, 0.00F, 1.00F);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.00F, 0.00F, 0.00F, 1.00F);
    colors[ImGuiCol_Button] = ImVec4(0.18F, 0.055F, 0.01F, 1.00F);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.90F, 0.22F, 0.01F, 1.00F);
    colors[ImGuiCol_ButtonActive] = ImVec4(1.00F, 0.36F, 0.02F, 1.00F);
    colors[ImGuiCol_Header] = ImVec4(0.18F, 0.055F, 0.01F, 1.00F);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.75F, 0.16F, 0.01F, 1.00F);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.95F, 0.26F, 0.01F, 1.00F);
    colors[ImGuiCol_CheckMark] = ImVec4(1.00F, 0.32F, 0.02F, 1.00F);
    colors[ImGuiCol_SliderGrab] = ImVec4(1.00F, 0.25F, 0.01F, 1.00F);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00F, 0.43F, 0.04F, 1.00F);
    colors[ImGuiCol_Separator] = ImVec4(0.72F, 0.14F, 0.01F, 1.00F);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.94F, 0.20F, 0.01F, 1.00F);
}

void DrawDriveDetails(const OpticalDrive& drive) {
    ImGui::TextDisabled("Firmware");
    ImGui::SameLine(110.0F);
    ImGui::TextUnformatted(drive.firmware.empty() ? "Not reported" : drive.firmware.c_str());
    ImGui::TextDisabled("Connection");
    ImGui::SameLine(110.0F);
    ImGui::TextUnformatted(drive.bus.empty() ? "Unknown" : drive.bus.c_str());
    ImGui::TextDisabled("Media");
    ImGui::SameLine(110.0F);
    ImVec4 mediaColor(1.00F, 0.62F, 0.24F, 1.00F);
    if (drive.mediaPresent && drive.blankMediaKnown && drive.blankMedia &&
        (drive.currentProfile == 0 ||
         drive.currentProfile == 0x0009 ||
         drive.currentProfile == 0x0011 ||
         drive.currentProfile == 0x0015 ||
         drive.currentProfile == 0x0016 ||
         drive.currentProfile == 0x001B ||
         drive.currentProfile == 0x002B)) {
        mediaColor = ImVec4(0.45F, 0.95F, 0.50F, 1.00F);
    } else if (drive.mediaPresent && drive.blankMediaKnown && !drive.blankMedia) {
        mediaColor = ImVec4(1.00F, 0.32F, 0.24F, 1.00F);
    }
    ImGui::TextColored(
        mediaColor,
        "%s",
        drive.mediaDescription.empty()
            ? (drive.mediaPresent ? "Disc present" : "No disc / not ready")
            : drive.mediaDescription.c_str());
}

[[nodiscard]] int SelectedSpeedX(
    const AppState& state,
    const OpticalDrive* drive) {
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

    if (IsDvdProfile(state.selectedConsole)) {
        return std::max(
            1,
            static_cast<int>(
                std::lround(
                    static_cast<double>(
                        speed.kilobytesPerSecond) /
                    1385.0)));
    }

    return std::max(
        1,
        static_cast<int>(
            std::lround(
                speed.cdMultiplier)));
}

[[nodiscard]] RetroBeamAdvancedOptions EffectiveAdvancedOptions(
    const AppState& state,
    const OpticalDrive* drive) {
    RetroBeamAdvancedOptions options = state.advanced;

    if (drive == nullptr) {
        options.burnFree = false;
        options.forceSpeed = false;
        options.useStreamingPolicy = false;
        return options;
    }

    if (!drive->burnFreeSupported) {
        options.burnFree = false;
    }
    if (!drive->forceSpeedSupported) {
        options.forceSpeed = false;
    }
    if (!IsDvdProfile(state.selectedConsole) ||
        !drive->realTimeStreamingKnown ||
        !drive->streamRecordingSupported ||
        (!drive->getPerformanceWriteSpeedSupported &&
         !drive->modePage2AWriteSpeedSupported)) {
        options.useStreamingPolicy = false;
    }

    return options;
}

[[nodiscard]] std::string FormatKiB(const std::uint32_t bytes) {
    if (bytes >= 1024U * 1024U) {
        char text[32]{};
        snprintf(
            text,
            sizeof(text),
            "%.1f MiB",
            static_cast<double>(bytes) / (1024.0 * 1024.0));
        return text;
    }
    return std::to_string(bytes / 1024U) + " KiB";
}

[[nodiscard]] const char* OpcPolicyDisplayName(
    const RetroBeamOpcPolicy policy) {
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

void DrawApp(
    AppState& state,
    BurnEngine& burnEngine,
    const std::array<Texture, kConsoleProfileCount>& artworks,
    const Texture& disc) {
    const ImGuiIO& io = ImGui::GetIO();
    const BurnSnapshot burn = burnEngine.Snapshot();
    const int consoleIndex = std::clamp(
        static_cast<int>(state.selectedConsole),
        0,
        kConsoleProfileCount - 1);
    const Texture& artwork =
        artworks[static_cast<std::size_t>(consoleIndex)];
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);
    constexpr ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("Retro Burner", nullptr, windowFlags);

    ImGui::SetWindowFontScale(1.55F);
    ImGui::TextUnformatted("RETRO BURNER");
    ImGui::SetWindowFontScale(1.0F);
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", ConsoleSubtitle(state.selectedConsole));
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTable(
            "MainLayout",
            2,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Artwork", ImGuiTableColumnFlags_WidthFixed, 390.0F);
        ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();

        const float artSize = std::min(372.0F, ImGui::GetContentRegionAvail().x);
        if (artwork.IsValid()) {
            ImGui::Image(
                reinterpret_cast<ImTextureID>(artwork.view.Get()),
                ImVec2(artSize, artSize));
        } else {
            ImGui::Dummy(ImVec2(artSize, artSize));
            ImGui::TextDisabled("Artwork could not be loaded.");
        }

        constexpr float discSize = 142.0F;
        const float discStart = ImGui::GetCursorPosX() +
            (artSize - discSize) * 0.5F;
        ImGui::SetCursorPosX(discStart);
        const ImVec2 discTopLeft = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(discSize, discSize));
        static float discAngle = 0.0F;
        if (burn.writing) {
            discAngle = std::fmod(discAngle + io.DeltaTime * 3.4F, 6.283185307F);
        }
        DrawRotatedImage(
            disc,
            ImVec2(
                discTopLeft.x + discSize * 0.5F,
                discTopLeft.y + discSize * 0.5F),
            discSize,
            discAngle);

        if (burn.writing || burn.stage == BurnStage::Complete ||
            (burn.stage == BurnStage::Failed && burn.progress > 0.0F)) {
            const int percent = static_cast<int>(std::lround(burn.progress * 100.0F));
            char progressLabel[32]{};
            snprintf(progressLabel, sizeof(progressLabel), "%d%%", percent);
            ImGui::SetNextItemWidth(artSize);
            ImGui::ProgressBar(
                std::clamp(burn.progress, 0.0F, 1.0F),
                ImVec2(artSize, 23.0F),
                progressLabel);

            // RB_IMGBURN_STYLE_BUFFER_BARS_V84B
            if (burn.writing && burn.ringBufferPercent >= 0) {
                ImGui::TextDisabled("Buffer");
                char fifoLabel[16]{};
                snprintf(
                    fifoLabel,
                    sizeof(fifoLabel),
                    "%d%%",
                    std::clamp(burn.ringBufferPercent, 0, 100));
                ImGui::ProgressBar(
                    static_cast<float>(
                        std::clamp(burn.ringBufferPercent, 0, 100)) / 100.0F,
                    ImVec2(artSize, 15.0F),
                    fifoLabel);
            }

            if (burn.writing && burn.driveBufferPercent >= 0) {
                ImGui::TextDisabled("Device Buffer");
                char deviceLabel[16]{};
                snprintf(
                    deviceLabel,
                    sizeof(deviceLabel),
                    "%d%%",
                    std::clamp(burn.driveBufferPercent, 0, 100));
                ImGui::ProgressBar(
                    static_cast<float>(
                        std::clamp(burn.driveBufferPercent, 0, 100)) / 100.0F,
                    ImVec2(artSize, 15.0F),
                    deviceLabel);
            }

            if (burn.stage == BurnStage::Complete) {
                DrawCenteredSuccessText(artSize);
            } else {
                std::string burnDetails;
            // RB_FIXED_WIDTH_BURN_SPEED_V2
            // Reserve five columns for live optical write speed so
            // 9.x -> 10.x/48.x never moves the following status fields.
            const auto FixedBurnSpeed =
                [](const std::string& speed) {
                    if (speed.size() >= 5) {
                        return speed;
                    }
                    return std::string(
                               5 - speed.size(),
                               ' ') +
                           speed;
                };
                if (burn.writing) {
                    if (IsDvdProfile(state.selectedConsole)) {
                        // RB_COMPACT_DVD_BURN_STATUS_V84L
                        // Buffer health already has dedicated graphical bars.
                        // Keep the compact text row to the two values that are
                        // not otherwise obvious at a glance.
                        if (!burn.actualSpeed.empty()) {
                            burnDetails =
                                "Speed " +
                                FixedBurnSpeed(burn.actualSpeed);
                        }
                        if (!burn.remainingTime.empty()) {
                            if (!burnDetails.empty()) {
                                burnDetails += "  |  ";
                            }
                            burnDetails +=
                                "Remaining " +
                                burn.remainingTime;
                        }
                    } else if (state.selectedConsole ==
                               ConsoleProfile::Dreamcast) {
                        burnDetails =
                            "Session " +
                            std::to_string(burn.session) +
                            " of 2";
                        if (!burn.actualSpeed.empty()) {
                            burnDetails +=
                                "  |  " +
                                FixedBurnSpeed(burn.actualSpeed);
                        }
                // RB_UNIVERSAL_BURN_METRICS
                if (burn.ringBufferPercent >= 0) {
                    if (!burnDetails.empty()) {
                        burnDetails += "  |  ";
                    }
                    burnDetails +=
                        "FIFO / Read Buffer " +
                        (std::string(
    burn.ringBufferPercent < 10
        ? "  "
        : (burn.ringBufferPercent < 100 ? " " : "")) +
 std::to_string(burn.ringBufferPercent)) +
                        "%";
                }

                if (burn.driveBufferPercent >= 0) {
                    if (!burnDetails.empty()) {
                        burnDetails += "  |  ";
                    }
                    burnDetails +=
                        "Drive Buffer " +
                        (std::string(
    burn.driveBufferPercent < 10
        ? "  "
        : (burn.driveBufferPercent < 100 ? " " : "")) +
 std::to_string(burn.driveBufferPercent)) +
                        "%";
                } else if (burn.bufferPercent >= 0) {
                    if (!burnDetails.empty()) {
                        burnDetails += "  |  ";
                    }
                    burnDetails +=
                        "Buffer " +
                        (std::string(
    burn.bufferPercent < 10
        ? "  "
        : (burn.bufferPercent < 100 ? " " : "")) +
 std::to_string(burn.bufferPercent)) +
                        "%";
                }
                        if (burn.bufferPercent >= 0) {
                            burnDetails +=
                                "  |  buffer " +
                                (std::string(
    burn.bufferPercent < 10
        ? "  "
        : (burn.bufferPercent < 100 ? " " : "")) +
 std::to_string(burn.bufferPercent)) +
                                "%";
                        }
                    } else {
                        burnDetails = FixedBurnSpeed(burn.actualSpeed);
                        if (burn.bufferPercent >= 0) {
                            if (!burnDetails.empty()) {
                                burnDetails += "  |  ";
                            }
                            burnDetails +=
                                "buffer " +
                                (std::string(
    burn.bufferPercent < 10
        ? "  "
        : (burn.bufferPercent < 100 ? " " : "")) +
 std::to_string(burn.bufferPercent)) +
                                "%";
                        }
                    }
                } else {
                    burnDetails = "Burn failed";
                }

                const float detailWidth =
                    ImGui::CalcTextSize(burnDetails.c_str()).x;
                ImGui::SetCursorPosX(
                    ImGui::GetCursorPosX() +
                    std::max(
                        0.0F,
                        (artSize - detailWidth) * 0.5F));
                ImGui::TextUnformatted(burnDetails.c_str());
            }
        }

        ImGui::TableNextColumn();
        ImGui::PushID("Controls");

        ImGui::TextDisabled("TARGET CONSOLE");
        ImGui::BeginDisabled(burn.busy);
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::BeginCombo(
                "##TargetConsole",
                ConsoleName(state.selectedConsole))) {
            for (int index = 0; index < kConsoleProfileCount; ++index) {
                const ConsoleProfile profile =
                    static_cast<ConsoleProfile>(index);
                const bool selected =
                    profile == state.selectedConsole;

                if (ImGui::Selectable(
                        ConsoleName(profile),
                        selected)) {
                    if (state.selectedConsole != profile) {
                        state.selectedConsole = profile;
                        state.selectedCdi.clear();
                        state.selectedSpeed = 0;
                        burnEngine.Reset();

                        state.status =
                            std::string(ConsoleName(profile)) +
                            " selected. Choose an image, run Check Image, then insert a " +
                            ExpectedMediaName(profile) +
                            ".";
                    }
                }

                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        if (state.selectedConsole == ConsoleProfile::Xbox360) {
            ImGui::Spacing();
            ImGui::TextDisabled("XBOX 360 DISC FORMAT");
            ImGui::BeginDisabled(burn.busy);
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::BeginCombo(
                    "##Xbox360DiscType",
                    Xbox360DiscTypeName(state.xbox360DiscType))) {
                for (int mode = 0; mode < 2; ++mode) {
                    const Xbox360DiscType type =
                        mode == 0
                            ? Xbox360DiscType::Xgd2
                            : Xbox360DiscType::Xgd3;
                    const bool selected =
                        state.xbox360DiscType == type;
                    if (ImGui::Selectable(
                            Xbox360DiscTypeName(type),
                            selected)) {
                        state.xbox360DiscType = type;
                        state.selectedSpeed = 0;
                        burnEngine.Reset();
                        state.status =
                            type == Xbox360DiscType::Xgd3
                                ? "XGD3 selected. ABGX360 AutoFix Level 3 and verified BurnerMAX capacity are required before writing."
                                : "XGD2 selected. Use blank DVD+R DL media.";
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();

            if (state.xbox360DiscType == Xbox360DiscType::Xgd3) {
                ImGui::TextColored(
                    ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                    "XGD3 prerequisite: ABGX360 AutoFix Level 3 runs on a temporary ISO copy, then BurnerMAX expanded DVD+R DL capacity is verified.");
            }
        }

        ImGui::Spacing();

        ImGui::TextDisabled("DISC IMAGE");
        ImGui::BeginChild("CdiPath", ImVec2(0.0F, 58.0F), ImGuiChildFlags_Borders);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0F);
        if (state.selectedCdi.empty()) {
            ImGui::TextDisabled("%s", ImageHint(state.selectedConsole));
        } else {
            const std::string path = WideToUtf8(state.selectedCdi);
            ImGui::TextWrapped("%s", path.c_str());
        }
        ImGui::EndChild();

        ImGui::BeginDisabled(burn.busy);
        if (ImGui::Button("Browse...", ImVec2(128.0F, 0.0F))) {
            ShowCdiPicker(state, burnEngine);
        }
        ImGui::SameLine();
        const bool canCheck = !state.selectedCdi.empty();
        ImGui::BeginDisabled(!canCheck);
        if (ImGui::Button("Check Image", ImVec2(128.0F, 0.0F))) {
            BurnRequest request;
            request.cdiPath = state.selectedCdi;
            request.target = ToBurnTarget(state.selectedConsole);
            request.xbox360DiscType = state.xbox360DiscType;
            request.checkOnly = true;
            (void)burnEngine.Start(std::move(request));
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::TextDisabled("OPTICAL BURNER");
        const std::string drivePreview = state.drives.empty()
            ? "No optical burners found"
            : state.drives[static_cast<std::size_t>(state.selectedDrive)]
                  .DisplayName();
        ImGui::BeginDisabled(burn.busy);
        ImGui::SetNextItemWidth(-92.0F);
        if (ImGui::BeginCombo("##Burner", drivePreview.c_str())) {
            for (int index = 0; index < static_cast<int>(state.drives.size()); ++index) {
                const bool selected = index == state.selectedDrive;
                const std::string name = state.drives[static_cast<std::size_t>(index)]
                                             .DisplayName();
                if (ImGui::Selectable(name.c_str(), selected)) {
                    state.selectedDrive = index;
                    state.selectedSpeed = 0;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(82.0F, 0.0F))) {
            RefreshDrives(state);
        }
        ImGui::EndDisabled();

        const OpticalDrive* drive = state.drives.empty()
            ? nullptr
            : &state.drives[static_cast<std::size_t>(state.selectedDrive)];
        if (drive != nullptr) {
            DrawDriveDetails(*drive);
        }

        ImGui::Spacing();

        // RB_RECORDING_BACKEND_SELECTOR_V84H
        //
        // Keep backend selection visible as a first-class part of Retro Burner.
        // The project intentionally integrates multiple open-source recording
        // engines and more may be added later. Availability is profile-aware,
        // but the selector itself must not disappear.
        ImGui::TextDisabled("RECORDING BACKEND");

        const bool dvdBackendProfile =
            IsDvdProfile(state.selectedConsole);

        const bool growisofsEffective =
            dvdBackendProfile &&
            state.useGrowisofsForDvd;

        const char* backendPreview =
            growisofsEffective
                ? "growisofs"
                : "RetroBeam (default)";

        ImGui::BeginDisabled(burn.busy);
        ImGui::SetNextItemWidth(-1.0F);

        if (ImGui::BeginCombo(
                "##RecordingBackend",
                backendPreview)) {
            if (ImGui::Selectable(
                    "RetroBeam (default)",
                    !growisofsEffective)) {
                state.useGrowisofsForDvd = false;
            }

            ImGui::BeginDisabled(!dvdBackendProfile);
            if (ImGui::Selectable(
                    dvdBackendProfile
                        ? "growisofs"
                        : "growisofs (DVD profiles only)",
                    growisofsEffective)) {
                state.useGrowisofsForDvd = true;
            }
            ImGui::EndDisabled();

            ImGui::EndCombo();
        }

        ImGui::EndDisabled();

        if (!dvdBackendProfile) {
            ImGui::TextDisabled(
                "Current CD profile uses RetroBeam; growisofs becomes "
                "selectable for PS2 DVD and Xbox 360.");
        } else if (state.useGrowisofsForDvd) {
            ImGui::TextDisabled(
                "growisofs selected; console DVD writes are explicitly DAO.");
        } else {
            ImGui::TextDisabled(
                "RetroBeam selected (default); Advanced Settings apply to it.");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("WRITE SPEED");

        // RB_STAGE44C_RECOMMENDED_SPEED_DEFAULTS
        //
        // One shared Windows/Linux UI policy:
        //   * actual mounted MMC profile gates the list and selects x-units;
        //   * console-specific conservative speed becomes the initial default;
        //   * user can still explicitly choose another speed or Automatic;
        //   * the recommendation is re-applied only when drive/media/profile
        //     context changes, never every frame.
        const bool blankWritableMedia =
            drive != nullptr &&
            drive->mediaPresent &&
            drive->blankMediaKnown &&
            drive->blankMedia;

        const std::uint16_t mountedProfile =
            drive != nullptr
                ? drive->currentProfile
                : 0;

        const bool mountedCdR =
            blankWritableMedia &&
            mountedProfile == 0x0009;

        const bool mountedDvdRecordable =
            blankWritableMedia &&
            (mountedProfile == 0x0011 ||
             mountedProfile == 0x0015 ||
             mountedProfile == 0x0016 ||
             mountedProfile == 0x001B ||
             mountedProfile == 0x002B);

        const bool mountedDvdDualLayer =
            blankWritableMedia &&
            (mountedProfile == 0x0015 ||
             mountedProfile == 0x0016 ||
             mountedProfile == 0x002B);

        const bool mountedDvdPlusRDl =
            blankWritableMedia &&
            mountedProfile == 0x002B;

        const bool speedMediaCompatible =
            state.selectedConsole == ConsoleProfile::PlayStation2Dvd
                ? mountedDvdRecordable
                : (state.selectedConsole == ConsoleProfile::Xbox360
                    ? mountedDvdPlusRDl
                    : mountedCdR);

        const bool dvdSpeedMode =
            mountedProfile == 0x0011 ||
            mountedProfile == 0x0015 ||
            mountedProfile == 0x0016 ||
            mountedProfile == 0x001B ||
            mountedProfile == 0x002B;

        const char* speedMediaPrompt =
            state.selectedConsole == ConsoleProfile::Xbox360
                ? "Insert blank DVD+R DL"
                : (state.selectedConsole == ConsoleProfile::PlayStation2Dvd
                    ? "Insert blank DVD-R / DVD+R / DVD-DL"
                    : "Insert blank CD-R");

        const auto chooseRecommendedSpeed =
            [&]() -> int {
                if (!speedMediaCompatible ||
                    drive == nullptr ||
                    drive->writeSpeeds.empty()) {
                    return 0;
                }

                int lowestIndex = 0;
                std::uint32_t lowestKbps =
                    drive->writeSpeeds[0].kilobytesPerSecond;

                for (int index = 1;
                     index <
                         static_cast<int>(
                             drive->writeSpeeds.size());
                     ++index) {
                    const std::uint32_t kbps =
                        drive->writeSpeeds[
                            static_cast<std::size_t>(
                                index)]
                            .kilobytesPerSecond;

                    if (kbps < lowestKbps) {
                        lowestKbps = kbps;
                        lowestIndex = index;
                    }
                }

                // CD-based consoles intentionally prefer the lowest actual
                // speed the inserted CD-R reports.
                if (!dvdSpeedMode) {
                    return lowestIndex + 1;
                }

                int preferredX = 0;

                if (state.selectedConsole ==
                        ConsoleProfile::Xbox360 ||
                    mountedDvdDualLayer) {
                    preferredX = 4;
                } else if (
                    state.selectedConsole ==
                        ConsoleProfile::PlayStation2Dvd) {
                    preferredX = 6;
                }

                if (preferredX <= 0) {
                    return lowestIndex + 1;
                }

                const std::uint32_t targetKbps =
                    static_cast<std::uint32_t>(
                        preferredX * 1385);

                int preferredIndex = -1;
                std::uint32_t preferredDistance =
                    0xFFFFFFFFU;

                for (int index = 0;
                     index <
                         static_cast<int>(
                             drive->writeSpeeds.size());
                     ++index) {
                    const std::uint32_t kbps =
                        drive->writeSpeeds[
                            static_cast<std::size_t>(
                                index)]
                            .kilobytesPerSecond;

                    const long roundedX =
                        std::lround(
                            static_cast<float>(kbps) /
                            1385.0F);

                    if (roundedX != preferredX) {
                        continue;
                    }

                    const std::uint32_t distance =
                        kbps >= targetKbps
                            ? kbps - targetKbps
                            : targetKbps - kbps;

                    if (preferredIndex < 0 ||
                        distance < preferredDistance) {
                        preferredIndex = index;
                        preferredDistance = distance;
                    }
                }

                return preferredIndex >= 0
                    ? preferredIndex + 1
                    : lowestIndex + 1;
            };

        const int recommendedSpeedIndex =
            chooseRecommendedSpeed();

        // Fingerprint the speed/media context so recommendation is applied
        // once when the drive/media/profile changes. This preserves a user's
        // later explicit choice, including Automatic (drive/media).
        std::uint64_t speedContextFingerprint =
            static_cast<std::uint64_t>(
                mountedProfile) +
            0x9E3779B97F4A7C15ULL;

        if (drive != nullptr) {
            speedContextFingerprint ^=
                static_cast<std::uint64_t>(
                    drive->mediaPresent ? 1U : 0U)
                << 48U;
            speedContextFingerprint ^=
                static_cast<std::uint64_t>(
                    drive->blankMedia ? 1U : 0U)
                << 49U;

            for (const WriteSpeed& speed :
                 drive->writeSpeeds) {
                speedContextFingerprint ^=
                    static_cast<std::uint64_t>(
                        speed.kilobytesPerSecond) +
                    0x9E3779B97F4A7C15ULL +
                    (speedContextFingerprint << 6U) +
                    (speedContextFingerprint >> 2U);
            }
        }

        const std::wstring currentSpeedDrive =
            drive == nullptr
                ? std::wstring{}
                : (!drive->rootPath.empty()
                    ? drive->rootPath
                    : drive->devicePath);

        static bool speedDefaultContextKnown = false;
        static std::wstring speedDefaultDrive;
        static std::uint16_t speedDefaultProfile = 0xFFFFU;
        static ConsoleProfile speedDefaultConsole =
            ConsoleProfile::Count;
        static std::uint64_t speedDefaultFingerprint = 0;

        const bool speedContextChanged =
            !speedDefaultContextKnown ||
            speedDefaultDrive != currentSpeedDrive ||
            speedDefaultProfile != mountedProfile ||
            speedDefaultConsole != state.selectedConsole ||
            speedDefaultFingerprint != speedContextFingerprint;

        if (speedContextChanged) {
            speedDefaultContextKnown = true;
            speedDefaultDrive = currentSpeedDrive;
            speedDefaultProfile = mountedProfile;
            speedDefaultConsole = state.selectedConsole;
            speedDefaultFingerprint =
                speedContextFingerprint;

            state.selectedSpeed =
                speedMediaCompatible
                    ? recommendedSpeedIndex
                    : 0;
        }

        if (!speedMediaCompatible) {
            state.selectedSpeed = 0;
        } else if (
            state.selectedSpeed >
            static_cast<int>(
                drive->writeSpeeds.size())) {
            state.selectedSpeed =
                recommendedSpeedIndex;
        }

        std::string speedPreview =
            drive == nullptr
                ? "Select optical writer"
                : (speedMediaCompatible
                    ? "Automatic (drive/media)"
                    : speedMediaPrompt);

        if (speedMediaCompatible &&
            drive != nullptr &&
            state.selectedSpeed > 0 &&
            state.selectedSpeed <=
                static_cast<int>(
                    drive->writeSpeeds.size())) {
            speedPreview =
                FormatSpeed(
                    drive->writeSpeeds[
                        static_cast<std::size_t>(
                            state.selectedSpeed - 1)],
                    dvdSpeedMode);

            if (state.selectedSpeed ==
                recommendedSpeedIndex) {
                speedPreview +=
                    " - Recommended";
            }
        }

        ImGui::BeginDisabled(
            burn.busy ||
            !speedMediaCompatible);

        ImGui::SetNextItemWidth(-1.0F);

        if (ImGui::BeginCombo(
                "##WriteSpeed",
                speedPreview.c_str())) {
            if (ImGui::Selectable(
                    "Automatic (drive/media)",
                    state.selectedSpeed == 0)) {
                state.selectedSpeed = 0;
            }

            if (drive != nullptr) {
                for (int index = 0;
                     index <
                         static_cast<int>(
                             drive->writeSpeeds.size());
                     ++index) {
                    std::string label =
                        FormatSpeed(
                            drive->writeSpeeds[
                                static_cast<std::size_t>(
                                    index)],
                            dvdSpeedMode);

                    if (index + 1 ==
                        recommendedSpeedIndex) {
                        label +=
                            " - Recommended";
                    }

                    const bool selected =
                        state.selectedSpeed ==
                        index + 1;

                    if (ImGui::Selectable(
                            label.c_str(),
                            selected)) {
                        state.selectedSpeed =
                            index + 1;
                    }

                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
            }

            ImGui::EndCombo();
        }

        ImGui::EndDisabled();

        if (drive == nullptr) {
            ImGui::TextDisabled(
                "Connect a USB or internal CD/DVD writer, then press Refresh.");
        } else if (!speedMediaCompatible) {
            ImGui::TextDisabled(
                "%s",
                speedMediaPrompt);
        } else if (
            recommendedSpeedIndex > 0) {
            if (state.selectedConsole ==
                    ConsoleProfile::PlayStation2Dvd &&
                !mountedDvdDualLayer) {
                ImGui::TextDisabled(
                    "Console-safe default: prefer 6x when the current media advertises it.");
            } else if (
                state.selectedConsole ==
                    ConsoleProfile::Xbox360 ||
                mountedDvdDualLayer) {
                ImGui::TextDisabled(
                    "Console-safe default: prefer 4x when the current dual-layer media advertises it.");
            } else {
                ImGui::TextDisabled(
                    "Console-safe default: lowest speed advertised by the current CD-R.");
            }
        } else {
            ImGui::TextDisabled(
                "%s",
                drive->speedQueryMessage.c_str());
        }

        ImGui::Spacing();
                // RB_ADVANCED_SETTINGS_EXPLICIT_STATE_V84I
        // Advanced means opt-in: always begin a fresh RetroBurner process
        // collapsed, then preserve the user's open/closed choice normally
        // for the rest of that process.
        static bool advancedSettingsOpen = false;
        ImGui::SetNextItemOpen(
            advancedSettingsOpen,
            ImGuiCond_Always);

        const bool advancedSettingsVisible =
            ImGui::CollapsingHeader("ADVANCED SETTINGS");

        advancedSettingsOpen = advancedSettingsVisible;

        if (advancedSettingsVisible) {
            if (drive == nullptr) {
                ImGui::TextDisabled(
                    "Select an optical writer to load RetroBeam capabilities.");
            } else {
                const bool growisofsDvdSelected =
                    IsDvdProfile(state.selectedConsole) &&
                    state.useGrowisofsForDvd;

                if (growisofsDvdSelected) {
                    ImGui::TextDisabled(
                        "RetroBeam Advanced Settings are not applied while growisofs is selected.");
                }

                ImGui::BeginDisabled(growisofsDvdSelected);

                ImGui::TextDisabled(
                    "%s",
                    drive->advancedCapabilityMessage.empty()
                        ? "Capability fingerprint not available."
                        : drive->advancedCapabilityMessage.c_str());

                ImGui::BeginDisabled(burn.busy);
                if (ImGui::SmallButton("Reset advanced defaults")) {
                    state.advanced = RetroBeamAdvancedOptions{};
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("default: BURN-Free off, explicit OPC skipped");

                ImGui::BeginDisabled(burn.busy || !drive->burnFreeSupported);
                ImGui::Checkbox(
                    "BURN-Free",
                    &state.advanced.burnFree);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled(
                    drive->burnFreeSupported
                        ? "detected by current capability probe"
                        : "not detected by current capability probe");

                if (state.advanced.burnFree) {
                    ImGui::TextColored(
                        ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                        "Warning: BURN-Free allows underrun recovery/linking. "
                        "RetroBurner's default is continuous write.");
                }

                ImGui::BeginDisabled(burn.busy || !drive->forceSpeedSupported);
                ImGui::Checkbox(
                    "Force speed",
                    &state.advanced.forceSpeed);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled(
                    drive->forceSpeedSupported
                        ? "available"
                        : "not advertised");

                if (IsDvdProfile(state.selectedConsole)) {
                    ImGui::Separator();
                    ImGui::TextDisabled("OPC POLICY");
                const char* opcPreview =
                    state.advanced.opcPolicy == RetroBeamOpcPolicy::Force
                        ? "Force explicit OPC"
                        : (state.advanced.opcPolicy == RetroBeamOpcPolicy::Skip
                            ? "Skip explicit OPC (default / recommended)"
                            : "Automatic");
                ImGui::BeginDisabled(burn.busy);
                ImGui::SetNextItemWidth(-1.0F);
                if (ImGui::BeginCombo("##OpcPolicy", opcPreview)) {
                    if (ImGui::Selectable(
                            "Automatic",
                            state.advanced.opcPolicy == RetroBeamOpcPolicy::Automatic)) {
                        state.advanced.opcPolicy = RetroBeamOpcPolicy::Automatic;
                    }
                    if (ImGui::Selectable(
                            "Force explicit OPC",
                            state.advanced.opcPolicy == RetroBeamOpcPolicy::Force)) {
                        state.advanced.opcPolicy = RetroBeamOpcPolicy::Force;
                    }
                    if (ImGui::Selectable(
                            "Skip explicit OPC (default / recommended)",
                            state.advanced.opcPolicy == RetroBeamOpcPolicy::Skip)) {
                        state.advanced.opcPolicy = RetroBeamOpcPolicy::Skip;
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();

                if (drive->opcDescriptorCountKnown) {
                    ImGui::TextDisabled(
                        "OPC descriptors currently recorded: %u",
                        drive->opcDescriptorCount);
                } else {
                    ImGui::TextDisabled(
                        "OPC descriptor count: unavailable for current media.");
                }

                    ImGui::Separator();
                    ImGui::TextDisabled("MMC STREAMING / SPEED CONTROL");

                    const bool streamPolicyAvailable =
                        drive->realTimeStreamingKnown &&
                        drive->streamRecordingSupported &&
                        (drive->getPerformanceWriteSpeedSupported ||
                         drive->modePage2AWriteSpeedSupported);

                    ImGui::BeginDisabled(
                        burn.busy || !streamPolicyAvailable);
                    ImGui::Checkbox(
                        "Use explicit SET STREAMING policy",
                        &state.advanced.useStreamingPolicy);
                    ImGui::EndDisabled();

                    ImGui::TextDisabled(
                        "SET STREAMING: %s  |  SET CD SPEED: %s",
                        streamPolicyAvailable ? "available" : "not available",
                        drive->setCdSpeedSupported ? "available" : "not reported");

                    if (drive->realTimeStreamingKnown) {
                        ImGui::TextDisabled(
                            "Realtime streaming state: current=%s, persistent=%s",
                            drive->realTimeStreamingCurrent ? "on" : "off",
                            drive->realTimeStreamingPersistent ? "on" : "off");
                    }
                    ImGui::TextDisabled(
                        "Write-speed descriptors: GET PERFORMANCE=%s, Mode Page 2Ah=%s",
                        drive->getPerformanceWriteSpeedSupported ? "yes" : "no",
                        drive->modePage2AWriteSpeedSupported ? "yes" : "no");

                    if (state.advanced.useStreamingPolicy && streamPolicyAvailable) {
                        ImGui::Indent();
                        const char* rotationPreview =
                            state.advanced.streamRotation == RetroBeamStreamRotation::Cav
                                ? "CAV"
                                : "Drive/media default";
                        ImGui::BeginDisabled(burn.busy);
                        ImGui::SetNextItemWidth(-1.0F);
                        if (ImGui::BeginCombo("Rotation##Streaming", rotationPreview)) {
                            if (ImGui::Selectable(
                                    "Drive/media default",
                                    state.advanced.streamRotation == RetroBeamStreamRotation::Default)) {
                                state.advanced.streamRotation = RetroBeamStreamRotation::Default;
                            }
                            if (ImGui::Selectable(
                                    "CAV",
                                    state.advanced.streamRotation == RetroBeamStreamRotation::Cav)) {
                                state.advanced.streamRotation = RetroBeamStreamRotation::Cav;
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::Checkbox(
                            "Require exact selected streaming speed",
                            &state.advanced.streamExact);
                        ImGui::Checkbox(
                            "Restore streaming defaults after burn",
                            &state.advanced.restoreStreamingDefaults);
                        ImGui::EndDisabled();
                        ImGui::Unindent();
                    }
                }

                ImGui::Separator();
                ImGui::TextDisabled("DRIVE BUFFER");
                if (drive->driveBufferCapacityKnown) {
                    const std::string total =
                        FormatKiB(drive->driveBufferCapacityBytes);
                    const std::string available =
                        FormatKiB(drive->driveBufferAvailableBytes);
                    ImGui::Text(
                        "%s total  |  %s currently available",
                        total.c_str(),
                        available.c_str());
                } else if (drive->readBufferCapacitySupported) {
                    ImGui::TextDisabled(
                        "READ BUFFER CAPACITY supported; size was not returned during refresh.");
                } else {
                    ImGui::TextDisabled(
                        "Drive buffer capacity not reported.");
                }

                ImGui::EndDisabled();
            }
        }

        ImGui::Spacing();
        const std::string& currentStatus =
            burn.stage == BurnStage::Idle ? state.status : burn.status;
        ImGui::TextWrapped("%s", currentStatus.c_str());

        const bool dvdProfile =
            IsDvdProfile(state.selectedConsole);
        const bool xboxProfile =
            state.selectedConsole == ConsoleProfile::Xbox360;
        const bool ps2DvdProfile =
            state.selectedConsole == ConsoleProfile::PlayStation2Dvd;
        const bool ps2NeedsDualLayer =
            SelectedPs2ImageNeedsDualLayer(state);

        const bool profileSupported =
            drive == nullptr ||
            (xboxProfile
                ? drive->currentProfile == 0x002B
                : (ps2DvdProfile
                    ? (ps2NeedsDualLayer
                        ? (drive->currentProfile == 0x0015 ||
                           drive->currentProfile == 0x0016 ||
                           drive->currentProfile == 0x002B)
                        : (drive->currentProfile == 0x0011 ||
                           drive->currentProfile == 0x001B ||
                           drive->currentProfile == 0x0015 ||
                           drive->currentProfile == 0x0016 ||
                           drive->currentProfile == 0x002B))
                    : (drive->currentProfile == 0 ||
                       drive->currentProfile == 0x0009)));

        const bool blankSupported =
            drive == nullptr ||
            (dvdProfile
                ? (drive->blankMediaKnown && drive->blankMedia)
                : (!drive->blankMediaKnown || drive->blankMedia));

        const bool writerSupported =
            drive == nullptr ||
            dvdProfile ||
            !drive->cdWriteCapabilityKnown ||
            drive->canWriteCdR;

        const bool backendReady =
            drive != nullptr &&
            (dvdProfile
                ? ((!drive->rootPath.empty() ||
                    drive->devicePath.size() >= 6) &&
                   (state.useGrowisofsForDvd ||
                    !drive->cdrecordDevice.empty()))
                : !drive->cdrecordDevice.empty());

        const bool xgd3Selected =
            xboxProfile &&
            state.xbox360DiscType == Xbox360DiscType::Xgd3;

        const bool preparedXgd3ForSelection =
            xgd3Selected &&
            burn.xgd3Prepared &&
            !burn.preparedXgd3SourcePath.empty() &&
            _wcsicmp(
                burn.preparedXgd3SourcePath.c_str(),
                state.selectedCdi.c_str()) == 0;

        const bool canTestBurnerMax =
            xgd3Selected &&
            !burn.busy &&
            drive != nullptr &&
            drive->mediaPresent &&
            (!drive->rootPath.empty() || drive->devicePath.size() >= 6);

        const bool canBurn =
            !burn.busy &&
            !state.selectedCdi.empty() &&
            drive != nullptr &&
            drive->mediaPresent &&
            profileSupported &&
            blankSupported &&
            writerSupported &&
            backendReady;

        if (xgd3Selected) {
            if (preparedXgd3ForSelection && !burn.busy) {
                ImGui::TextColored(
                    ImVec4(0.42F, 0.88F, 0.52F, 1.00F),
                    "XGD3 READY: successful preflight/preparation copy is cached.");
                ImGui::TextWrapped(
                    "BURN XBOX 360 will reuse the ABGX360-verified working ISO. "
                    "The ISO copy, AutoFix and verification passes will not run again. "
                    "DVD+R DL and BurnerMAX checks are still repeated before writing.");
            } else {
                ImGui::TextColored(
                    ImVec4(1.00F, 0.72F, 0.28F, 1.00F),
                    "RECOMMENDED: run FULL XGD3 PREFLIGHT before a real burn.");
                ImGui::TextWrapped(
                    "If preflight passes, Retro Burner keeps that prepared working copy in a session cache so Burn can reuse it without repeating ABGX360.");
            }

            if (burn.busy && !burn.writing) {
                ImGui::Spacing();
                ImGui::TextDisabled("XGD3 PREPARATION");
                ImGui::TextWrapped("%s", burn.status.c_str());

                char preparationLabel[32]{};
                snprintf(
                    preparationLabel,
                    sizeof(preparationLabel),
                    "%d%%",
                    static_cast<int>(
                        std::lround(
                            std::clamp(
                                burn.progress,
                                0.0F,
                                1.0F) *
                            100.0F)));

                ImGui::ProgressBar(
                    std::clamp(burn.progress, 0.0F, 1.0F),
                    ImVec2(-1.0F, 20.0F),
                    preparationLabel);
                ImGui::Spacing();
            }

            ImGui::BeginDisabled(!canTestBurnerMax);
            if (ImGui::Button(
                    "TEST / ENABLE BURNERMAX",
                    ImVec2(-1.0F, 38.0F))) {
                if (drive != nullptr) {
                    std::wstring opticalDriveRoot =
                        drive->rootPath.size() >= 2
                            ? drive->rootPath.substr(0, 2)
                            : (drive->devicePath.size() >= 6
                                ? drive->devicePath.substr(4, 2)
                                : drive->rootPath);

                    (void)burnEngine.StartBurnerMaxTest(
                        std::move(opticalDriveRoot));
                }
            }
            ImGui::EndDisabled();

            if (!canTestBurnerMax && !burn.busy) {
                ImGui::TextDisabled(
                    "Select a drive with a DVD+R DL inserted to test BurnerMAX.");
            } else if (!burn.busy) {
                ImGui::TextDisabled(
                    "No disc sectors are written. The payload is verified by layer boundary and expanded writable capacity.");
            }

            ImGui::Spacing();
        }

        const char* burnButtonLabel =
            xboxProfile
                ? "BURN XBOX 360"
                : (ps2DvdProfile ? "BURN DVD" : "BURN DISC");

        ImGui::BeginDisabled(!canBurn);
        if (ImGui::Button(
                burnButtonLabel,
                ImVec2(-1.0F, 48.0F))) {
            ImGui::OpenPopup("Confirm burn");
        }
        ImGui::EndDisabled();

        if (dvdProfile) {
            ImGui::BeginDisabled(!canBurn);

            const char* dryRunLabel =
                xboxProfile
                    ? (state.xbox360DiscType == Xbox360DiscType::Xgd3
                        ? "RECOMMENDED: FULL XGD3 PREFLIGHT - NO DISC WRITE"
                        : "DRY RUN DVD+R DL - NO WRITE")
                    : "DRY RUN DVD - NO WRITE";

            if (ImGui::Button(
                    dryRunLabel,
                    ImVec2(-1.0F, 35.0F))) {
                if (drive != nullptr) {
                    BurnRequest request;
                    request.cdiPath = state.selectedCdi;
                    request.target =
                        ToBurnTarget(state.selectedConsole);
                    request.xbox360DiscType =
                        state.xbox360DiscType;
                    request.cdrecordDevice =
                        drive->cdrecordDevice;
                    request.opticalDriveRoot =
                        drive->rootPath.size() >= 2
                            ? drive->rootPath.substr(0, 2)
                            : (drive->devicePath.size() >= 6
                                ? drive->devicePath.substr(4, 2)
                                : drive->rootPath);
                    request.requestedSpeedX =
                        SelectedSpeedX(state, drive);
                    request.advanced =
                        EffectiveAdvancedOptions(state, drive);
                    request.useGrowisofsForDvd =
                        state.useGrowisofsForDvd;
                    request.checkOnly = false;
                    request.simulate = true;

                    (void)burnEngine.Start(
                        std::move(request));
                }
            }

            ImGui::EndDisabled();

            if (xgd3Selected && !burn.busy) {
                ImGui::TextDisabled(
                    preparedXgd3ForSelection
                        ? (state.useGrowisofsForDvd
                            ? "Prepared ISO is cached. Re-running preflight reuses it and repeats media/BurnerMAX + growisofs dry-run checks."
                            : "Prepared ISO is cached. Re-running preflight reuses it and repeats media/BurnerMAX + RetroBeam no-write checks.")
                        : (state.useGrowisofsForDvd
                            ? "Full preflight: ABGX360 AutoFix + verification, DVD+R DL/BurnerMAX checks and growisofs dry run."
                            : "Full preflight: ABGX360 AutoFix + verification, DVD+R DL/BurnerMAX checks and RetroBeam no-write preflight."));
            }
        }

        if (!canBurn && !burn.busy) {
            if (state.selectedCdi.empty()) {
                ImGui::TextDisabled(
                    "Choose a compatible disc image first.");
            } else if (drive == nullptr) {
                ImGui::TextDisabled(
                    "Connect and select an optical writer.");
            } else if (!drive->mediaPresent) {
                if (xboxProfile) {
                    ImGui::TextDisabled(
                        "Insert a blank DVD+R DL, then press Refresh.");
                } else if (ps2DvdProfile) {
                    ImGui::TextDisabled(
                        ps2NeedsDualLayer
                            ? "Insert a blank dual-layer DVD, then press Refresh."
                            : "Insert a blank DVD-R / DVD+R, then press Refresh.");
                } else {
                    ImGui::TextDisabled(
                        "Insert a blank CD-R, then press Refresh.");
                }
            } else if (!profileSupported) {
                if (xboxProfile) {
                    ImGui::TextDisabled(
                        "Xbox 360 burning requires blank DVD+R DL media.");
                } else if (ps2DvdProfile) {
                    ImGui::TextDisabled(
                        ps2NeedsDualLayer
                            ? "This PS2 image requires DVD-R DL or DVD+R DL media."
                            : "PS2 DVD supports blank DVD-R, DVD+R, DVD-R DL or DVD+R DL media.");
                } else {
                    ImGui::TextDisabled(
                        "This console profile currently requires CD-R media.");
                }
            } else if (!blankSupported) {
                ImGui::TextDisabled(
                    dvdProfile
                        ? "The inserted DVD must be positively reported as blank."
                        : "The inserted CD-R is not blank.");
            } else if (!writerSupported) {
                ImGui::TextDisabled(
                    "The selected optical drive cannot write CD-R media.");
            } else if (!backendReady) {
                ImGui::TextDisabled(
                    dvdProfile
                        ? (state.useGrowisofsForDvd
                            ? "Could not access the selected Windows DVD writer for growisofs. Press Refresh."
                            : "Could not map this drive to RetroBeam. Press Refresh and check the Burn log.")
                        : "Could not map this drive to RetroBeam. Press Refresh and check the Burn log.");
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("BURN LOG");
        ImGui::SameLine();

        ImGui::BeginDisabled(burn.log.empty());
        if (ImGui::SmallButton("Copy log")) {
            ImGui::SetClipboardText(burn.log.c_str());
        }
        ImGui::EndDisabled();

        ImGui::BeginChild(
            "BurnLogText",
            ImVec2(0.0F, 145.0F),
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_HorizontalScrollbar);

        if (burn.log.empty()) {
            ImGui::TextDisabled("CDIrip / RetroBeam / ABGX360 / BurnerMAX output will appear here.");
        } else {
            ImGui::TextUnformatted(burn.log.c_str());
        }

        ImGui::EndChild();
        state.lastBurnStage = burn.stage;

        if (ImGui::BeginPopupModal(
                "Confirm burn",
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            const char* mediaLabel =
                xboxProfile
                    ? "DVD+R DL"
                    : (ps2DvdProfile ? "DVD" : "CD-R");

            ImGui::Text(
                "This will permanently write the selected %s.",
                mediaLabel);

            ImGui::Text(
                "Target: %s",
                ConsoleName(state.selectedConsole));

            if (xboxProfile) {
                ImGui::Text(
                    "Format: %s",
                    Xbox360DiscTypeName(state.xbox360DiscType));
            }

            if (drive != nullptr) {
                ImGui::Text(
                    "Burner: %s",
                    drive->DisplayName().c_str());

                const int selectedSpeed =
                    SelectedSpeedX(state, drive);

                ImGui::Text(
                    "Speed: %s",
                    selectedSpeed == 0
                        ? (dvdProfile
                            ? "Automatic - DVD drive/media negotiates"
                            : "Automatic - firmware negotiates")
                        : (std::to_string(selectedSpeed) +
                           "x requested")
                              .c_str());

                const RetroBeamAdvancedOptions effectiveAdvanced =
                    EffectiveAdvancedOptions(state, drive);

                if (dvdProfile) {
                    ImGui::Text(
                        "Backend: %s",
                        state.useGrowisofsForDvd
                            ? "growisofs"
                            : "RetroBeam");

                    if (state.useGrowisofsForDvd) {
                        ImGui::Text(
                            "growisofs: DAO | dvd-compat | selected write speed");
                    } else {
                        ImGui::Text(
                            "RetroBeam: BURN-Free %s | Force speed %s",
                            effectiveAdvanced.burnFree ? "ON" : "OFF",
                            effectiveAdvanced.forceSpeed ? "ON" : "OFF");
                        ImGui::Text(
                            "OPC: %s | SET STREAMING: %s",
                            OpcPolicyDisplayName(effectiveAdvanced.opcPolicy),
                            effectiveAdvanced.useStreamingPolicy
                                ? (effectiveAdvanced.streamRotation == RetroBeamStreamRotation::Cav
                                    ? "CAV"
                                    : "explicit/default rotation")
                                : "automatic");

                        if (effectiveAdvanced.burnFree) {
                            ImGui::TextColored(
                                ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                                "BURN-Free is ENABLED: underrun recovery/linking is allowed.");
                        }
                    }
                } else {
                    ImGui::Text(
                        "RetroBeam: BURN-Free %s | Force speed %s",
                        effectiveAdvanced.burnFree ? "ON" : "OFF",
                        effectiveAdvanced.forceSpeed ? "ON" : "OFF");
                }

                if (!drive->blankMediaKnown) {
                    ImGui::TextColored(
                        ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                        "The drive did not report blank state; the recording backend will verify it before writing.");
                }
            }

            if (ps2DvdProfile) {
                ImGui::TextColored(
                    ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                    ps2NeedsDualLayer
                        ? "Dual-layer PS2 image selected. The inserted DL medium must have enough writable capacity."
                        : "Single-layer PS2 DVD burning is proven; DVD+R and dual-layer media are also accepted when compatible.");
            }

            if (xboxProfile) {
                if (state.xbox360DiscType == Xbox360DiscType::Xgd3) {
                    ImGui::TextColored(
                        preparedXgd3ForSelection
                            ? ImVec4(0.42F, 0.88F, 0.52F, 1.00F)
                            : ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                        preparedXgd3ForSelection
                            ? "Using the ABGX360-verified working copy from the successful preflight. ABGX360 will not repeat; BurnerMAX/media checks still run before writing."
                            : "No prepared XGD3 copy is cached. Retro Burner will create a temporary copy, run ABGX360 AutoFix + verification, then test/enable BurnerMAX before writing.");
                } else {
                    ImGui::TextColored(
                        ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                        "Xbox 360 XGD2 requires blank DVD+R DL media and uses the 1913760 layer break.");
                }
            }

            ImGui::Spacing();

            const char* confirmButtonLabel =
                xboxProfile
                    ? "Burn Xbox 360 now"
                    : (ps2DvdProfile ? "Burn DVD now" : "Burn now");

            if (ImGui::Button(
                    confirmButtonLabel,
                    ImVec2(170.0F, 0.0F))) {
                if (drive != nullptr) {
                    BurnRequest request;
                    request.cdiPath = state.selectedCdi;
                    request.target =
                        ToBurnTarget(state.selectedConsole);
                    request.xbox360DiscType =
                        state.xbox360DiscType;
                    request.cdrecordDevice =
                        drive->cdrecordDevice;
                    request.opticalDriveRoot =
                        drive->rootPath.size() >= 2
                            ? drive->rootPath.substr(0, 2)
                            : (drive->devicePath.size() >= 6
                                ? drive->devicePath.substr(4, 2)
                                : drive->rootPath);
                    request.requestedSpeedX =
                        SelectedSpeedX(state, drive);
                    request.advanced =
                        EffectiveAdvancedOptions(state, drive);
                    request.useGrowisofsForDvd =
                        state.useGrowisofsForDvd;
                    request.checkOnly = false;
                    request.simulate = false;

                    if (burnEngine.Start(
                            std::move(request))) {
                        ImGui::CloseCurrentPopup();
                    }
                }
            }

            ImGui::SameLine();

            if (ImGui::Button(
                    "Cancel",
                    ImVec2(120.0F, 0.0F))) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::EndTable();
    }

    ImGui::End();
}

bool CreateDeviceD3D(const HWND window) {
    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    swapChainDescription.BufferCount = 2;
    swapChainDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.OutputWindow = window;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.Windowed = TRUE;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL createdLevel{};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        requestedLevels,
        static_cast<UINT>(std::size(requestedLevels)),
        D3D11_SDK_VERSION,
        &swapChainDescription,
        &g_swapChain,
        &g_device,
        &createdLevel,
        &g_deviceContext);
    return SUCCEEDED(result);
}

void CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (g_swapChain != nullptr &&
        SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_renderTarget);
    }
}

void CleanupDeviceD3D() {
    g_renderTarget.Reset();
    g_swapChain.Reset();
    g_deviceContext.Reset();
    g_device.Reset();
}

} // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam);

LRESULT WINAPI WindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
        return TRUE;
    }

    switch (message) {
    case WM_SIZE:
        if (g_device != nullptr && wParam != SIZE_MINIMIZED) {
            g_renderTarget.Reset();
            g_swapChain->ResizeBuffers(
                0,
                static_cast<UINT>(LOWORD(lParam)),
                static_cast<UINT>(HIWORD(lParam)),
                DXGI_FORMAT_UNKNOWN,
                0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL ||
            wParam == DBT_DEVICEREMOVECOMPLETE ||
            wParam == DBT_DEVNODES_CHANGED) {
            g_driveRefreshRequested = true;
        }
        return 0;
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        if (length > 0) {
            std::wstring path(static_cast<std::size_t>(length) + 1U, L'\0');
            DragQueryFileW(drop, 0, path.data(), length + 1U);
            path.resize(length);
            g_droppedPath = std::move(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0U) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_jobInProgress) {
            MessageBoxW(
                window,
                L"Retro Burner is still working. Keep the app open until the current operation finishes; closing during a write can ruin the disc.",
                L"Operation in progress",
                MB_OK | MB_ICONWARNING);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI wWinMain(
    const HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"RetroBurnerWindow";
    RegisterClassExW(&windowClass);

    RECT desiredSize{0, 0, 1120, 760};
    AdjustWindowRectEx(&desiredSize, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const HWND window = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"Retro Burner",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        desiredSize.right - desiredSize.left,
        desiredSize.bottom - desiredSize.top,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (window == nullptr || !CreateDeviceD3D(window)) {
        CleanupDeviceD3D();
        if (window != nullptr) {
            DestroyWindow(window);
        }
        UnregisterClassW(windowClass.lpszClassName, instance);
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
        MessageBoxW(
            nullptr,
            L"Direct3D 11 could not be initialized.",
            L"Retro Burner",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    CreateRenderTarget();
    // The burner runs elevated for direct SPTI access. Permit Explorer's
    // lower-integrity file-drop messages so drag-and-drop still works.
    ChangeWindowMessageFilterEx(window, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(window, 0x0049, MSGFLT_ALLOW, nullptr); // WM_COPYGLOBALDATA
    DragAcceptFiles(window, TRUE);
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureImGuiStyle();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device.Get(), g_deviceContext.Get());

    const std::array<Texture, kConsoleProfileCount> artworks = {
        LoadPngResource(
            g_device.Get(),
            IDR_PNG_BURNING_DC),
        LoadPngResource(
            g_device.Get(),
            IDR_PNG_BURNING_PS1),
        LoadPngResource(
            g_device.Get(),
            IDR_PNG_BURNING_PS2),
        LoadPngResource(
            g_device.Get(),
            IDR_PNG_BURNING_PS2),
        LoadPngResource(
            g_device.Get(),
            IDR_PNG_BURNING_SATURN),
        LoadPngResource(
            g_device.Get(),
            IDR_PNG_BURNING_XBOX360),
    };
    const Texture disc =
        LoadPngResource(g_device.Get(), IDR_PNG_SPINNING_DISC);

    AppState state;
    BurnEngine burnEngine;
    RefreshDrives(state);

    bool running = true;
    while (running) {
        g_jobInProgress = burnEngine.Snapshot().busy;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        if (g_driveRefreshRequested) {
            if (!burnEngine.Snapshot().busy) {
                g_driveRefreshRequested = false;
                RefreshDrives(state);
            }
        }
        if (!g_droppedPath.empty()) {
            if (!burnEngine.Snapshot().busy) {
                SelectCdi(state, burnEngine, std::move(g_droppedPath));
            }
            g_droppedPath.clear();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawApp(state, burnEngine, artworks, disc);

        ImGui::Render();
        constexpr float clearColor[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        g_deviceContext->OMSetRenderTargets(1, g_renderTarget.GetAddressOf(), nullptr);
        g_deviceContext->ClearRenderTargetView(g_renderTarget.Get(), clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, instance);
    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return 0;
}
