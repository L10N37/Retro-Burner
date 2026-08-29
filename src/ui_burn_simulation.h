#pragma once

// RB_STAGE44X_UI_BURN_SIMULATION
//
// Hidden engineering mode for visual parity testing only.
//
// Set:
//   RETROBURNER_UI_SIM=all
//
// or one scenario:
//   dreamcast
//   ps1
//   ps2cd
//   ps2cd-verify
//   ps2dvd-retrobeam
//   ps2dvd-growisofs
//   saturn
//   xgd2-retrobeam
//   xgd2-growisofs
//   xgd3-retrobeam
//   xgd3-growisofs
//   ps2cd-failure
//
// This mode synthesizes BurnSnapshot + OpticalDrive data. It never calls
// BurnEngine::Start(), never executes RetroBeam/growisofs, and never sends an
// optical WRITE command.

#include "burn_engine.h"
#include "drive_manager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

struct UiBurnSimulationFrame final {
    std::string scenario;
    std::string label;
    BurnTarget target = BurnTarget::Dreamcast;
    Xbox360DiscType xbox360DiscType = Xbox360DiscType::Xgd2;
    bool useGrowisofsForDvd = false;
    std::wstring imagePath;
    OpticalDrive drive;
    BurnSnapshot burn;
};

// RB_STAGE44Z_MSVC_SAFE_ENV
//
// MSVC deprecates getenv() and this project builds with warnings-as-errors.
// Use _dupenv_s() on native Windows; retain getenv() on Linux.
[[nodiscard]] inline std::string UiBurnSimulationEnvironmentValue()
{
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;

    if (_dupenv_s(
            &value,
            &length,
            "RETROBURNER_UI_SIM") != 0 ||
        value == nullptr) {
        return {};
    }

    const std::string result(value);
    std::free(value);
    return result;
#else
    const char* value =
        std::getenv("RETROBURNER_UI_SIM");

    return value != nullptr
        ? std::string(value)
        : std::string{};
#endif
}

[[nodiscard]] inline bool UiBurnSimulationRequested()
{
    return !UiBurnSimulationEnvironmentValue().empty();
}

[[nodiscard]] inline std::string UiBurnSimulationRequest()
{
    return UiBurnSimulationEnvironmentValue();
}

[[nodiscard]] inline std::string UiBurnSimulationLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

[[nodiscard]] inline WriteSpeed UiBurnSimulationCdSpeed(const int x)
{
    WriteSpeed speed;
    speed.kilobytesPerSecond =
        static_cast<std::uint32_t>(
            std::lround(
                static_cast<double>(x) * 176.4));
    speed.cdMultiplier = static_cast<float>(x);
    speed.exactForWholeMedia = true;
    speed.rotation = "CLV/default";
    return speed;
}

[[nodiscard]] inline WriteSpeed UiBurnSimulationDvdSpeed(const int x)
{
    WriteSpeed speed;
    speed.kilobytesPerSecond =
        static_cast<std::uint32_t>(x * 1385);
    speed.cdMultiplier =
        static_cast<float>(speed.kilobytesPerSecond) / 176.4F;
    speed.exactForWholeMedia = true;
    speed.rotation = "CLV/default";
    return speed;
}

[[nodiscard]] inline std::string UiBurnSimulationClock(
    const int seconds)
{
    const int clamped = std::max(0, seconds);
    const int minutes = clamped / 60;
    const int remainder = clamped % 60;

    std::string result =
        std::to_string(minutes) + ":";

    if (remainder < 10)
        result += "0";

    result += std::to_string(remainder);
    return result;
}

[[nodiscard]] inline UiBurnSimulationFrame BuildUiBurnSimulationFrame(
    std::string requested,
    double elapsedSeconds)
{
    static constexpr std::array<const char*, 12> kScenarios = {
        "dreamcast",
        "ps1",
        "ps2cd",
        "ps2cd-verify",
        "ps2dvd-retrobeam",
        "ps2dvd-growisofs",
        "saturn",
        "xgd2-retrobeam",
        "xgd2-growisofs",
        "xgd3-retrobeam",
        "xgd3-growisofs",
        "ps2cd-failure",
    };

    requested = UiBurnSimulationLower(std::move(requested));

    constexpr double kScenarioSeconds = 12.0;

    std::string scenario = requested;
    double localSeconds =
        std::fmod(
            std::max(0.0, elapsedSeconds),
            kScenarioSeconds);

    if (scenario.empty() || scenario == "all") {
        const auto index =
            static_cast<std::size_t>(
                static_cast<unsigned long long>(
                    std::max(0.0, elapsedSeconds) /
                    kScenarioSeconds) %
                kScenarios.size());

        scenario = kScenarios[index];

        localSeconds =
            std::fmod(
                std::max(0.0, elapsedSeconds),
                kScenarioSeconds);
    }

    UiBurnSimulationFrame frame;
    frame.scenario = scenario;

    const bool growisofs =
        scenario.find("growisofs") != std::string::npos;
    const bool xgd3 =
        scenario.find("xgd3") != std::string::npos;
    const bool xgd2 =
        scenario.find("xgd2") != std::string::npos;
    const bool verify =
        scenario == "ps2cd-verify";
    const bool failure =
        scenario == "ps2cd-failure";

    frame.useGrowisofsForDvd = growisofs;
    frame.xbox360DiscType =
        xgd3
            ? Xbox360DiscType::Xgd3
            : Xbox360DiscType::Xgd2;

    if (scenario == "dreamcast") {
        frame.target = BurnTarget::Dreamcast;
        frame.label = "Dreamcast CDI / RetroBeam";
        frame.imagePath = L"/UI-SIM/game.cdi";
    } else if (scenario == "ps1") {
        frame.target = BurnTarget::PlayStation;
        frame.label = "PlayStation BIN/CUE / RetroBeam";
        frame.imagePath = L"/UI-SIM/game.cue";
    } else if (
        scenario == "ps2cd" ||
        scenario == "ps2cd-verify" ||
        scenario == "ps2cd-failure") {
        frame.target = BurnTarget::PlayStation2Cd;
        frame.label =
            verify
                ? "PlayStation 2 CD ISO / RetroBeam / Verify"
                : (failure
                    ? "PlayStation 2 CD ISO / Failure UI"
                    : "PlayStation 2 CD ISO / RetroBeam");
        frame.imagePath = L"/UI-SIM/game.iso";
    } else if (scenario.find("ps2dvd") == 0) {
        frame.target = BurnTarget::PlayStation2Dvd;
        frame.label =
            growisofs
                ? "PlayStation 2 DVD / growisofs"
                : "PlayStation 2 DVD / RetroBeam";
        frame.imagePath = L"/UI-SIM/game.iso";
    } else if (scenario == "saturn") {
        frame.target = BurnTarget::Saturn;
        frame.label = "Sega Saturn BIN/CUE / RetroBeam";
        frame.imagePath = L"/UI-SIM/game.cue";
    } else if (xgd2 || xgd3) {
        frame.target = BurnTarget::Xbox360;
        frame.label =
            std::string(xgd3 ? "Xbox 360 XGD3" : "Xbox 360 XGD2") +
            (growisofs ? " / growisofs" : " / RetroBeam");
        frame.imagePath = L"/UI-SIM/game.iso";
    } else {
        frame.target = BurnTarget::PlayStation2Cd;
        frame.scenario = "ps2cd";
        frame.label = "PlayStation 2 CD ISO / RetroBeam";
        frame.imagePath = L"/UI-SIM/game.iso";
    }

    const bool dvd =
        frame.target == BurnTarget::PlayStation2Dvd ||
        frame.target == BurnTarget::Xbox360;

    OpticalDrive drive;
    drive.rootPath = L"UI-SIM:";
    drive.devicePath = L"UI-SIM";
    drive.vendor = "Retro Burner";
    drive.product = "UI Simulation Drive";
    drive.firmware = "SIM";
    drive.bus = "virtual";
    drive.mediaPresent = true;
    drive.blankMediaKnown = true;
    drive.blankMedia = true;
    drive.cdWriteCapabilityKnown = true;
    drive.canWriteCdR = true;
    drive.cdrecordDevice = "UI-SIM";
    drive.retrobeamCapabilitiesKnown = true;
    drive.burnFreeSupported = true;
    drive.forceSpeedSupported = true;
    drive.realTimeStreamingKnown = true;
    drive.realTimeStreamingCurrent = true;
    drive.realTimeStreamingPersistent = false;
    drive.streamRecordingSupported = true;
    drive.getPerformanceWriteSpeedSupported = true;
    drive.modePage2AWriteSpeedSupported = true;
    drive.setCdSpeedSupported = true;
    drive.readBufferCapacitySupported = true;
    drive.opcDescriptorCountKnown = true;
    drive.opcDescriptorCount = 1;
    drive.driveBufferCapacityKnown = true;
    drive.driveBufferCapacityBytes = 2U * 1024U * 1024U;
    drive.driveBufferAvailableBytes = 2U * 1024U * 1024U;
    drive.advancedCapabilityMessage =
        "UI simulation: capabilities synthesized; no optical device is accessed.";

    if (frame.target == BurnTarget::Xbox360) {
        drive.currentProfile = 0x002B;
        drive.mediaDescription = "UI simulation blank DVD+R DL";
        drive.writeSpeeds = {
            UiBurnSimulationDvdSpeed(4),
            UiBurnSimulationDvdSpeed(6),
            UiBurnSimulationDvdSpeed(8),
        };
    } else if (dvd) {
        drive.currentProfile = 0x0011;
        drive.mediaDescription = "UI simulation blank DVD-R";
        drive.writeSpeeds = {
            UiBurnSimulationDvdSpeed(4),
            UiBurnSimulationDvdSpeed(6),
            UiBurnSimulationDvdSpeed(8),
        };
    } else {
        drive.currentProfile = 0x0009;
        drive.mediaDescription = "UI simulation blank CD-R";
        drive.writeSpeeds = {
            UiBurnSimulationCdSpeed(8),
            UiBurnSimulationCdSpeed(16),
            UiBurnSimulationCdSpeed(24),
        };
    }

    frame.drive = std::move(drive);

    BurnSnapshot burn;
    burn.busy = true;
    burn.writing = true;
    burn.stage = BurnStage::BurningSession1;
    burn.session = 1;
    burn.progress = 0.0F;
    burn.ringBufferPercent = 100;
    burn.driveBufferPercent = 100;
    burn.bufferPercent = 100;
    burn.actualSpeed =
        dvd
            ? (growisofs ? "6.0x" : "5.9x")
            : "15.8x";
    burn.remainingTime.clear();
    burn.layout = frame.label;

    if (xgd3) {
        burn.xgd3Prepared = true;
        burn.preparedXgd3SourcePath =
            L"/UI-SIM/original-xgd3.iso";
        burn.preparedXgd3WorkingPath =
            L"/UI-SIM/verified-xgd3.iso";
    }

    if (failure) {
        burn.stage = BurnStage::Failed;
        burn.busy = false;
        burn.writing = false;
        burn.progress = 0.17F;
        burn.ringBufferPercent = -1;
        burn.driveBufferPercent = -1;
        burn.bufferPercent = -1;
        burn.actualSpeed.clear();
        burn.status =
            "Drive rejected the requested track layout before data was written.";
        frame.burn = std::move(burn);
        return frame;
    }

    if (frame.target == BurnTarget::Dreamcast) {
        if (localSeconds < 1.0) {
            burn.stage = BurnStage::BurningSession1;
            burn.session = 1;
            burn.progress = 0.01F;
            burn.status = "Writing Lead-In...";
        } else if (localSeconds < 4.0) {
            burn.stage = BurnStage::BurningSession1;
            burn.session = 1;
            burn.progress =
                static_cast<float>(
                    0.05 +
                    ((localSeconds - 1.0) / 3.0) * 0.40);
            burn.status = "Writing Sectors...";
        } else if (localSeconds < 5.0) {
            burn.stage = BurnStage::BurningSession2;
            burn.session = 2;
            burn.progress = 0.50F;
            burn.status = "Writing Lead-In...";
        } else if (localSeconds < 9.0) {
            burn.stage = BurnStage::BurningSession2;
            burn.session = 2;
            burn.progress =
                static_cast<float>(
                    0.52 +
                    ((localSeconds - 5.0) / 4.0) * 0.43);
            burn.status = "Writing Sectors...";
        } else if (localSeconds < 10.5) {
            burn.stage = BurnStage::BurningSession2;
            burn.session = 2;
            burn.progress = 0.999F;
            burn.status = "Finalising Disc...";
        } else {
            burn.stage = BurnStage::Complete;
            burn.busy = false;
            burn.writing = false;
            burn.progress = 1.0F;
            burn.ringBufferPercent = -1;
            burn.driveBufferPercent = -1;
            burn.bufferPercent = -1;
            burn.actualSpeed.clear();
            burn.status = "Burn complete!";
        }

        frame.burn = std::move(burn);
        return frame;
    }

    if (verify && localSeconds >= 9.0 && localSeconds < 11.0) {
        burn.stage = BurnStage::BurningSession1;
        burn.busy = true;
        burn.writing = false;
        burn.progress = 0.999F;
        burn.ringBufferPercent = -1;
        burn.driveBufferPercent = -1;
        burn.bufferPercent = -1;
        burn.actualSpeed.clear();

        const int verifyPercent =
            static_cast<int>(
                std::clamp(
                    ((localSeconds - 9.0) / 2.0) * 100.0,
                    0.0,
                    100.0));

        burn.status =
            "Verifying Disc... " +
            std::to_string(verifyPercent) +
            "%";

        frame.burn = std::move(burn);
        return frame;
    }

    if (localSeconds < 2.0) {
        burn.progress = 0.01F;
        burn.status = "Writing Lead-In...";
    } else if (localSeconds < 8.0) {
        const double fraction =
            (localSeconds - 2.0) / 6.0;

        burn.progress =
            static_cast<float>(
                0.04 + fraction * 0.91);
        burn.status = "Writing Sectors...";

        burn.ringBufferPercent =
            96 +
            static_cast<int>(
                std::fmod(
                    std::floor(localSeconds * 2.0),
                    5.0));

        burn.driveBufferPercent =
            97 +
            static_cast<int>(
                std::fmod(
                    std::floor(localSeconds),
                    4.0));

        burn.bufferPercent =
            burn.driveBufferPercent;

        if (growisofs) {
            burn.remainingTime =
                UiBurnSimulationClock(
                    static_cast<int>(
                        std::lround(
                            (8.0 - localSeconds) * 24.0)));
        }
    } else if (localSeconds < (verify ? 9.0 : 10.0)) {
        burn.progress = 0.999F;
        burn.status = "Finalising Disc...";
        burn.remainingTime.clear();
    } else {
        burn.stage = BurnStage::Complete;
        burn.busy = false;
        burn.writing = false;
        burn.progress = 1.0F;
        burn.ringBufferPercent = -1;
        burn.driveBufferPercent = -1;
        burn.bufferPercent = -1;
        burn.actualSpeed.clear();
        burn.remainingTime.clear();
        burn.status = "Burn complete!";
    }

    frame.burn = std::move(burn);
    return frame;
}