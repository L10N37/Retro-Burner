#include "retrobeam_linux.h"
#include "embedded_bundle_linux.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>

namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool IsExecutableFile(const fs::path& path)
{
    std::error_code error;
    return fs::is_regular_file(path, error) &&
           !error &&
           ::access(path.c_str(), X_OK) == 0;
}

[[nodiscard]] fs::path ExecutableDirectory()
{
    std::error_code error;
    const fs::path self = fs::read_symlink("/proc/self/exe", error);
    if (error || self.empty())
        return {};
    return self.parent_path();
}

[[nodiscard]] fs::path FindOnPath(const std::string& name)
{
    const char* pathValue = std::getenv("PATH");
    if (pathValue == nullptr)
        return {};

    std::string_view remaining(pathValue);
    while (true) {
        const std::size_t separator = remaining.find(':');
        const std::string_view part =
            separator == std::string_view::npos
                ? remaining
                : remaining.substr(0, separator);

        const fs::path directory =
            part.empty() ? fs::current_path() : fs::path(std::string(part));
        const fs::path candidate = directory / name;
        if (IsExecutableFile(candidate))
            return candidate;

        if (separator == std::string_view::npos)
            break;
        remaining.remove_prefix(separator + 1U);
    }

    return {};
}

[[nodiscard]] bool ExactOpticalSgNode(
    const std::string& device,
    std::string& message)
{
    const fs::path path(device);

    if (path.parent_path() != "/dev") {
        message = "RetroBeam device is not an exact /dev/sgX node.";
        return false;
    }

    const std::string name = path.filename().string();
    if (name.size() < 3U || name.rfind("sg", 0) != 0) {
        message = "RetroBeam device is not an exact /dev/sgX node.";
        return false;
    }

    if (!std::all_of(
            name.begin() + 2,
            name.end(),
            [](const unsigned char c) { return std::isdigit(c) != 0; })) {
        message = "RetroBeam SG node contains an invalid suffix.";
        return false;
    }

    const fs::path typePath =
        fs::path("/sys/class/scsi_generic") / name / "device/type";

    std::ifstream typeFile(typePath);
    int type = -1;
    typeFile >> type;

    if (!typeFile || (type != 4 && type != 5)) {
        message =
            "The exact SG node is not sysfs SCSI type 4/5 optical; "
            "RetroBeam diagnostics refused to open it.";
        return false;
    }

    if (::access(path.c_str(), R_OK | W_OK) != 0) {
        message =
            "The current desktop session does not have read/write access "
            "to this optical SG node.";
        return false;
    }

    message =
        "Validated exact optical SCSI-generic node (sysfs type " +
        std::to_string(type) + ").";
    return true;
}

[[nodiscard]] RetroBeamReadOnlyCommand RunReadOnly(
    const fs::path& executable,
    const std::string& device,
    const std::string& label,
    const std::string& option)
{
    RetroBeamReadOnlyCommand command;
    command.label = label;
    command.process = RunProcessCapture(
        executable,
        {"dev=" + device, option},
        executable.parent_path());
    return command;
}

} // namespace

bool RetroBeamReadOnlyDiagnostics::Success() const noexcept
{
    if (!opticalNodeValidated || executable.empty() || commands.empty())
        return false;

    for (const auto& command : commands) {
        if (!command.process.started || command.process.exitCode != 0)
            return false;
    }
    return true;
}

std::string RetroBeamReadOnlyDiagnostics::CombinedText() const
{
    std::ostringstream text;

    text << "RetroBeam: "
         << (executable.empty() ? "(not found)" : executable.string())
         << '\n';
    text << "Device: " << device << '\n';
    text << "Guard: " << validationMessage << "\n\n";

    for (const auto& command : commands) {
        text << "===== " << command.label << " =====\n";
        text << "started=" << (command.process.started ? "yes" : "no")
             << " exit=" << command.process.exitCode << '\n';

        if (!command.process.error.empty())
            text << "error=" << command.process.error << '\n';

        text << command.process.output;
        if (!command.process.output.empty() &&
            command.process.output.back() != '\n') {
            text << '\n';
        }
        text << '\n';
    }

    text << "diagnostics_success=" << (Success() ? "yes" : "no") << '\n';
    return text.str();
}

std::filesystem::path FindRetroBeamExecutable()
{
    const std::filesystem::path embedded =
        MaterializeEmbeddedLinuxResource(
            "retrobeam");

    if (!embedded.empty())
        return embedded;

    return {};
}
RetroBeamReadOnlyDiagnostics RunRetroBeamReadOnlyDiagnostics(
    const std::string& exactSgDevice)
{
    RetroBeamReadOnlyDiagnostics diagnostics;
    diagnostics.device = exactSgDevice;
    diagnostics.executable = FindRetroBeamExecutable();

    diagnostics.opticalNodeValidated =
        ExactOpticalSgNode(exactSgDevice, diagnostics.validationMessage);

    if (!diagnostics.opticalNodeValidated)
        return diagnostics;

    if (diagnostics.executable.empty()) {
        diagnostics.validationMessage +=
            " Native RetroBeam executable was not found.";
        return diagnostics;
    }

    // Fixed allow-list only. No caller can pass an arbitrary RetroBeam option
    // through this diagnostics API.
    diagnostics.commands.push_back(
        RunReadOnly(
            diagnostics.executable, exactSgDevice, "INQUIRY", "-inq"));
    diagnostics.commands.push_back(
        RunReadOnly(
            diagnostics.executable, exactSgDevice, "CHECKDRIVE", "-checkdrive"));
    diagnostics.commands.push_back(
        RunReadOnly(
            diagnostics.executable, exactSgDevice, "PRCAP", "-prcap"));

    return diagnostics;
}
