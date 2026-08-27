#pragma once

#include <string>
#include <string_view>

struct RetroBeamProgressUpdate final {
    float progress = -1.0F;
    int fifoPercent = -1;
    int bufferPercent = -1;
    std::string speed;
    std::string phaseStatus;

    [[nodiscard]] bool HasTelemetry() const noexcept {
        return progress >= 0.0F ||
               fifoPercent >= 0 ||
               bufferPercent >= 0 ||
               !speed.empty() ||
               !phaseStatus.empty();
    }
};

[[nodiscard]] RetroBeamProgressUpdate ParseRetroBeamProgressText(
    std::string_view text);
