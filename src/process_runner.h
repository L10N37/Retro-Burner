#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

using ProcessOutputCallback =
    std::function<void(std::string_view)>;

struct CapturedProcessResult final {
    bool started = false;
    int exitCode = -1;
    bool outputTruncated = false;
    std::string output;
    std::string error;
};

[[nodiscard]] CapturedProcessResult RunProcessCapture(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& workingDirectory = {},
    const ProcessOutputCallback& onOutput = {});
