#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

using OpticalVerifyProgressCallback =
    std::function<void(std::uint32_t completedSectors,
                       std::uint32_t totalSectors)>;

struct OpticalVerifyResult final {
    bool success = false;
    std::string device;
    std::filesystem::path sourcePath;
    std::uint32_t startLba = 0;
    std::uint32_t sectorCount = 0;
    std::uint64_t bytesCompared = 0;
    std::string error;

    [[nodiscard]] std::string ToText() const;
};

[[nodiscard]] OpticalVerifyResult VerifyOpticalSectors(
    const std::string& exactSgDevice,
    const std::filesystem::path& sourcePath,
    std::uint32_t startLba = 0,
    OpticalVerifyProgressCallback progressCallback = {});
