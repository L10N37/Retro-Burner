#pragma once

#include "process_runner.h"

#include <filesystem>
#include <string>
#include <vector>

struct RetroBeamReadOnlyCommand final {
    std::string label;
    CapturedProcessResult process;
};

struct RetroBeamReadOnlyDiagnostics final {
    std::filesystem::path executable;
    std::string device;
    bool opticalNodeValidated = false;
    std::string validationMessage;
    std::vector<RetroBeamReadOnlyCommand> commands;

    [[nodiscard]] bool Success() const noexcept;
    [[nodiscard]] std::string CombinedText() const;
};

[[nodiscard]] std::filesystem::path FindRetroBeamExecutable();

[[nodiscard]] RetroBeamReadOnlyDiagnostics RunRetroBeamReadOnlyDiagnostics(
    const std::string& exactSgDevice);
