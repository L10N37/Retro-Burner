#include "burn_engine.h"
#include "retrobeam_failure.h"
#include "ps2_media_probe.h"
#include "retrobeam_progress.h"
#include "embedded_bundle_linux.h"
#include "burnermax.h"

#include "optical_verify_linux.h"
#include "process_runner.h"
#include "retrobeam_linux.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaximumLogBytes = 2U * 1024U * 1024U;
constexpr std::uintmax_t kCdSectorBytes = 2048ULL;

[[nodiscard]] std::string Lowercase(std::string value)
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

[[nodiscard]] bool ParseUnsignedField(
    const std::string& output,
    const std::regex& pattern,
    std::uintmax_t& value)
{
    std::smatch match;
    if (!std::regex_search(output, match, pattern))
        return false;

    try {
        value = std::stoull(match[1].str());
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool IsBlankCdR(
    const std::string& output,
    std::uintmax_t& remainingSectors,
    std::string& reason)
{
    if (output.find("Mounted media type:       CD-R") ==
        std::string::npos) {
        reason = "The selected drive does not contain CD-R media.";
        return false;
    }

    if (output.find("disk status:              empty") ==
        std::string::npos ||
        output.find("session status:           empty") ==
        std::string::npos) {
        reason =
            "Recording requires a positively identified blank CD-R "
            "(empty disc and empty session).";
        return false;
    }

    static const std::regex nwaPattern(
        R"(Next writable address:\s+([0-9]+))");
    static const std::regex remainingPattern(
        R"(Remaining writable size:\s+([0-9]+))");

    std::uintmax_t nextWritable = 0;
    if (!ParseUnsignedField(output, nwaPattern, nextWritable) ||
        nextWritable != 0) {
        reason =
            "Recording requires next writable address 0 on a blank CD-R.";
        return false;
    }

    if (!ParseUnsignedField(
            output,
            remainingPattern,
            remainingSectors) ||
        remainingSectors == 0) {
        reason =
            "RetroBeam did not report usable remaining CD-R capacity.";
        return false;
    }

    return true;
}

[[nodiscard]] std::vector<std::string> BuildPs2CdArguments(
    const BurnRequest& request,
    const fs::path& imagePath)
{
    std::vector<std::string> arguments;
    arguments.emplace_back("dev=" + request.cdrecordDevice);

    if (request.requestedSpeedX > 0) {
        arguments.emplace_back(
            "speed=" + std::to_string(request.requestedSpeedX));
    }

    arguments.emplace_back("fs=8m");

    std::vector<std::string> driverOptions;
    if (request.advanced.burnFree)
        driverOptions.emplace_back("burnfree");
    if (request.advanced.forceSpeed)
        driverOptions.emplace_back("forcespeed");

    if (!driverOptions.empty()) {
        std::string combined;
        for (std::size_t i = 0; i < driverOptions.size(); ++i) {
            if (i != 0)
                combined.push_back(',');
            combined += driverOptions[i];
        }
        arguments.emplace_back("driveropts=" + combined);
    }

    arguments.emplace_back("gracetime=2");
    arguments.emplace_back("-v");

    // On Linux, simulate=true is implemented as MMC test-write for this
    // CD path. It uses the same image/layout without recording user data.
    if (request.simulate)
        arguments.emplace_back("-dummy");

    arguments.emplace_back("-sao");
    arguments.emplace_back("-data");
    arguments.emplace_back(imagePath.string());
    return arguments;
}

[[nodiscard]] std::string CommandPreview(
    const fs::path& executable,
    const std::vector<std::string>& arguments)
{
    std::ostringstream text;
    text << executable.string();
    for (const std::string& argument : arguments)
        text << " [" << argument << "]";
    return text.str();
}

[[nodiscard]] std::string FormatRemainingSeconds(double seconds)
{
    if (seconds < 0.0)
        seconds = 0.0;

    const int whole = static_cast<int>(seconds + 0.5);
    const int minutes = whole / 60;
    const int remainder = whole % 60;

    std::ostringstream text;
    text << minutes << ':';
    if (remainder < 10)
        text << '0';
    text << remainder;
    return text.str();
}

[[nodiscard]] fs::path FindExecutableInPath(
    const std::string& name)
{
    const char* pathValue = std::getenv("PATH");
    if (pathValue == nullptr)
        return {};

    std::stringstream pathStream(pathValue);
    std::string entry;

    while (std::getline(pathStream, entry, ':')) {
        if (entry.empty())
            continue;

        const fs::path candidate =
            fs::path(entry) / name;

        std::error_code error;
        const auto status = fs::status(candidate, error);
        if (!error &&
            fs::is_regular_file(status) &&
            ::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }

    return {};
}


[[nodiscard]] std::string Stage29FindSgForBlockRoot(
    const fs::path& blockRoot)
{
    if (blockRoot.parent_path() != "/dev")
        return {};

    const std::string blockName =
        blockRoot.filename().string();

    if (blockName.rfind("sr", 0) != 0)
        return {};

    std::error_code error;
    const fs::path root(
        "/sys/class/scsi_generic");

    for (const fs::directory_entry& entry :
         fs::directory_iterator(root, error)) {
        if (error)
            break;

        const std::string sgName =
            entry.path().filename().string();

        std::ifstream typeFile(
            entry.path() / "device/type");

        int type = -1;
        typeFile >> type;

        if (!typeFile || type != 5)
            continue;

        if (fs::exists(
                entry.path() /
                "device/block" /
                blockName)) {
            return
                (fs::path("/dev") / sgName).string();
        }
    }

    return {};
}

[[nodiscard]] fs::path Stage29FindExecutable(
    const char* const name)
{
    if (name == nullptr || *name == '\0')
        return {};

    const char* const rawPath =
        std::getenv("PATH");

    if (rawPath == nullptr)
        return {};

    std::string pathList(rawPath);
    std::size_t start = 0;

    while (start <= pathList.size()) {
        const std::size_t separator =
            pathList.find(':', start);

        const std::string directory =
            pathList.substr(
                start,
                separator == std::string::npos
                    ? std::string::npos
                    : separator - start);

        const fs::path candidate =
            (directory.empty()
                ? fs::current_path()
                : fs::path(directory)) /
            name;

        if (::access(
                candidate.c_str(),
                X_OK) == 0) {
            return candidate;
        }

        if (separator == std::string::npos)
            break;

        start = separator + 1;
    }

    return {};
}


[[nodiscard]] fs::path Stage30ExecutableDirectory()
{
    std::array<char, 4096> path{};

    const ssize_t length =
        ::readlink(
            "/proc/self/exe",
            path.data(),
            path.size() - 1U);

    if (length <= 0)
        return fs::current_path();

    path[static_cast<std::size_t>(length)] = '\0';

    return
        fs::path(path.data())
            .parent_path();
}

[[nodiscard]] fs::path Stage30SiblingExecutable(
    const char* const name)
{
    if (name == nullptr ||
        *name == '\0') {
        return {};
    }

    const std::string_view requested(
        name);

    if (requested == "retrobeam" ||
        requested == "abgx360" ||
        requested == "cdirip") {
        const fs::path embedded =
            MaterializeEmbeddedLinuxResource(
                requested);

        if (!embedded.empty())
            return embedded;
    }

    const char* rawPath =
        std::getenv("PATH");

    if (rawPath == nullptr)
        return {};

    std::stringstream paths(
        rawPath);

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
[[nodiscard]] std::string Stage30Narrow(
    const std::wstring& value)
{
    if (value.empty())
        return {};

    std::string result;
    result.reserve(value.size());

    for (const wchar_t ch : value) {
        if (ch >= 0 &&
            ch <= 0x7F) {
            result.push_back(
                static_cast<char>(ch));
        } else {
            // Linux GUI supplies UTF-8 filesystem paths. The current shared
            // API remains std::wstring for Windows compatibility. Non-ASCII
            // paths are handled by filesystem's locale conversion below.
            return fs::path(value).string();
        }
    }

    return result;
}

[[nodiscard]] fs::path Stage30Path(
    const std::wstring& value)
{
    return fs::path(
        Stage30Narrow(value));
}

[[nodiscard]] bool Stage30ContainsCaseInsensitive(
    const std::string& text,
    const std::string& needle)
{
    std::string lowerText = text;
    std::string lowerNeedle = needle;

    std::transform(
        lowerText.begin(),
        lowerText.end(),
        lowerText.begin(),
        [](unsigned char c) {
            return static_cast<char>(
                std::tolower(c));
        });

    std::transform(
        lowerNeedle.begin(),
        lowerNeedle.end(),
        lowerNeedle.begin(),
        [](unsigned char c) {
            return static_cast<char>(
                std::tolower(c));
        });

    return
        lowerText.find(lowerNeedle) !=
        std::string::npos;
}

[[nodiscard]] std::string Stage30FindSgForBlockRoot(
    const fs::path& blockRoot)
{
    if (blockRoot.parent_path() != "/dev")
        return {};

    const std::string blockName =
        blockRoot.filename().string();

    std::error_code error;

    for (const fs::directory_entry& entry :
         fs::directory_iterator(
             "/sys/class/scsi_generic",
             error)) {
        if (error)
            break;

        std::ifstream typeFile(
            entry.path() /
            "device/type");

        int type = -1;
        typeFile >> type;

        if (!typeFile ||
            type != 5) {
            continue;
        }

        if (fs::exists(
                entry.path() /
                "device/block" /
                blockName)) {
            return
                (fs::path("/dev") /
                 entry.path().filename())
                    .string();
        }
    }

    return {};
}

[[nodiscard]] bool Stage30FileIdentity(
    const fs::path& path,
    std::uint64_t& bytes,
    std::uint64_t& writeTime)
{
    std::error_code error;

    const std::uintmax_t size =
        fs::file_size(
            path,
            error);

    if (error)
        return false;

    const auto time =
        fs::last_write_time(
            path,
            error);

    if (error)
        return false;

    bytes =
        static_cast<std::uint64_t>(
            size);

    writeTime =
        static_cast<std::uint64_t>(
            time.time_since_epoch()
                .count());

    return true;
}


[[nodiscard]] std::string Stage33RetroBeamDriverOptions(
    const BurnRequest& request,
    const std::uint64_t layerBreak)
{
    std::vector<std::string> options;

    options.emplace_back(
        request.advanced.burnFree
            ? "burnfree"
            : "noburnfree");

    switch (request.advanced.opcPolicy) {
    case RetroBeamOpcPolicy::Force:
        options.emplace_back("opc=force");
        break;

    case RetroBeamOpcPolicy::Skip:
        options.emplace_back("opc=skip");
        break;

    case RetroBeamOpcPolicy::Automatic:
    default:
        options.emplace_back("opc=auto");
        break;
    }

    if (request.advanced.forceSpeed)
        options.emplace_back("forcespeed");

    if (request.advanced.useStreamingPolicy) {
        options.emplace_back(
            request.advanced.streamRotation ==
                    RetroBeamStreamRotation::Cav
                ? "streamwrc=cav"
                : "streamwrc=default");

        options.emplace_back(
            request.advanced.streamExact
                ? "streamexact"
                : "nostreamexact");
    }

    if (layerBreak != 0) {
        options.emplace_back(
            "layerbreak=" +
            std::to_string(
                layerBreak));
    }

    std::string joined =
        "driveropts=";

    for (std::size_t i = 0;
         i < options.size();
         ++i) {
        if (i != 0)
            joined.push_back(',');

        joined +=
            options[i];
    }

    return joined;
}

[[nodiscard]] std::string Stage33RetroBeamPolicyText(
    const BurnRequest& request)
{
    std::string text =
        "RetroBeam advanced policy: BURN-Free=";

    text +=
        request.advanced.burnFree
            ? "enabled"
            : "disabled";

    text +=
        ", ForceSpeed=";

    text +=
        request.advanced.forceSpeed
            ? "enabled"
            : "disabled";

    text +=
        ", OPC=";

    switch (request.advanced.opcPolicy) {
    case RetroBeamOpcPolicy::Force:
        text += "force";
        break;

    case RetroBeamOpcPolicy::Skip:
        text += "skip";
        break;

    case RetroBeamOpcPolicy::Automatic:
    default:
        text += "automatic";
        break;
    }

    if (request.advanced.useStreamingPolicy) {
        text +=
            ", SET STREAMING=";

        text +=
            request.advanced.streamRotation ==
                    RetroBeamStreamRotation::Cav
                ? "CAV"
                : "firmware/default rotation";

        text +=
            request.advanced.streamExact
                ? " exact"
                : " closest-supported";

        text +=
            request.advanced.restoreStreamingDefaults
                ? " (restore defaults after burn)"
                : " (leave runtime streaming state)";
    } else {
        text +=
            ", SET STREAMING=automatic";
    }

    text.push_back('\n');
    return text;
}

[[nodiscard]] bool Stage33EjectOpticalDrive(
    const fs::path& blockDevice)
{
    const int handle =
        ::open(
            blockDevice.c_str(),
            O_RDONLY |
                O_NONBLOCK);

    if (handle < 0)
        return false;

    const int result =
        ::ioctl(
            handle,
            CDROMEJECT);

    ::close(handle);

    return result == 0;
}


[[nodiscard]] std::string Stage36Lowercase(
    std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char c) {
            return static_cast<char>(
                std::tolower(c));
        });

    return value;
}

[[nodiscard]] const char* Stage36TargetName(
    const BurnTarget target)
{
    switch (target) {
    case BurnTarget::Dreamcast:
        return "Dreamcast";
    case BurnTarget::PlayStation:
        return "PlayStation";
    case BurnTarget::PlayStation2Cd:
        return "PlayStation 2 CD";
    case BurnTarget::PlayStation2Dvd:
        return "PlayStation 2 DVD";
    case BurnTarget::Saturn:
        return "Sega Saturn";
    case BurnTarget::Xbox360:
        return "Xbox 360";
    default:
        return "Unknown";
    }
}

[[nodiscard]] std::string Stage36RetroBeamDriverOptions(
    const BurnRequest& request,
    const bool dvdTarget,
    const std::uint64_t layerBreak = 0)
{
    std::vector<std::string> options;

    options.emplace_back(
        request.advanced.burnFree
            ? "burnfree"
            : "noburnfree");

    if (dvdTarget) {
        switch (request.advanced.opcPolicy) {
        case RetroBeamOpcPolicy::Force:
            options.emplace_back("opc=force");
            break;
        case RetroBeamOpcPolicy::Skip:
            options.emplace_back("opc=skip");
            break;
        case RetroBeamOpcPolicy::Automatic:
        default:
            options.emplace_back("opc=auto");
            break;
        }
    }

    if (request.advanced.forceSpeed)
        options.emplace_back("forcespeed");

    if (request.advanced.useStreamingPolicy) {
        options.emplace_back(
            request.advanced.streamRotation ==
                    RetroBeamStreamRotation::Cav
                ? "streamwrc=cav"
                : "streamwrc=default");

        options.emplace_back(
            request.advanced.streamExact
                ? "streamexact"
                : "nostreamexact");
    }

    if (layerBreak != 0) {
        options.emplace_back(
            "layerbreak=" +
            std::to_string(layerBreak));
    }

    std::string result =
        "driveropts=";

    for (std::size_t i = 0;
         i < options.size();
         ++i) {
        if (i != 0)
            result.push_back(',');

        result += options[i];
    }

    return result;
}

[[nodiscard]] bool Stage36Eject(
    const fs::path& blockDevice)
{
    const int fd =
        ::open(
            blockDevice.c_str(),
            O_RDONLY |
                O_NONBLOCK);

    if (fd < 0)
        return false;

    const int rc =
        ::ioctl(
            fd,
            CDROMEJECT);

    ::close(fd);

    return rc == 0;
}

[[nodiscard]] bool Stage36ReadCue(
    const fs::path& cuePath,
    int& trackCount,
    std::string& errorMessage)
{
    std::ifstream stream(
        cuePath,
        std::ios::binary);

    if (!stream) {
        errorMessage =
            "Could not open the selected CUE sheet.";
        return false;
    }

    static const std::regex trackPattern(
        R"(^\s*TRACK\s+[0-9]+\s+\S+)",
        std::regex::icase);

    static const std::regex quotedFilePattern(
        R"RB(^\s*FILE\s+"([^"]+)"\s+\S+)RB",
        std::regex::icase);

    static const std::regex plainFilePattern(
        R"(^\s*FILE\s+([^\s]+)\s+\S+)",
        std::regex::icase);

    trackCount = 0;
    int fileCount = 0;

    std::string line;

    while (std::getline(
               stream,
               line)) {
        if (std::regex_search(
                line,
                trackPattern)) {
            ++trackCount;
        }

        std::smatch match;
        std::string referenced;

        if (std::regex_search(
                line,
                match,
                quotedFilePattern)) {
            referenced =
                match[1].str();
        } else if (
            std::regex_search(
                line,
                match,
                plainFilePattern)) {
            referenced =
                match[1].str();
        }

        if (referenced.empty())
            continue;

        ++fileCount;

        std::error_code existsError;

        if (!fs::is_regular_file(
                cuePath.parent_path() /
                    referenced,
                existsError)) {
            errorMessage =
                "CUE references a file that is missing: " +
                referenced;
            return false;
        }
    }

    if (fileCount == 0) {
        errorMessage =
            "The CUE sheet contains no FILE entry.";
        return false;
    }

    if (trackCount == 0) {
        errorMessage =
            "The CUE sheet contains no TRACK entries.";
        return false;
    }

    return true;
}

struct Stage36TemporaryDirectory final {
    fs::path path;

    ~Stage36TemporaryDirectory()
    {
        if (!path.empty()) {
            std::error_code ignored;
            fs::remove_all(
                path,
                ignored);
        }
    }
};

[[nodiscard]] Stage36TemporaryDirectory
Stage36MakeTemporaryDirectory()
{
    std::error_code error;

    fs::path root =
        fs::temp_directory_path(
            error);

    if (error)
        root = "/tmp";

    for (unsigned attempt = 0;
         attempt < 100;
         ++attempt) {
        const fs::path candidate =
            root /
            ("RetroBurner-CDI-" +
             std::to_string(
                 static_cast<unsigned long>(
                     ::getpid())) +
             "-" +
             std::to_string(
                 static_cast<unsigned long long>(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count())) +
             "-" +
             std::to_string(attempt));

        if (fs::create_directory(
                candidate,
                error)) {
            return {candidate};
        }

        error.clear();
    }

    return {};
}

struct Stage36DreamcastLayout final {
    bool audioData = false;
    std::vector<fs::path> firstSession;
    std::vector<fs::path> secondSession;
    std::uintmax_t firstBytes = 0;
    std::uintmax_t secondBytes = 0;
    std::string description;
};

[[nodiscard]] std::uintmax_t
Stage36SumFileSizes(
    const std::vector<fs::path>& files)
{
    std::uintmax_t total = 0;

    for (const fs::path& path : files) {
        std::error_code error;

        const std::uintmax_t size =
            fs::file_size(
                path,
                error);

        if (!error)
            total += size;
    }

    return total;
}

[[nodiscard]] bool Stage36DetectDreamcastLayout(
    const fs::path& directory,
    Stage36DreamcastLayout& layout,
    std::string& errorMessage)
{
    std::vector<fs::path> audio;
    std::vector<fs::path> data;

    std::error_code error;

    for (const fs::directory_entry& entry :
         fs::directory_iterator(
             directory,
             error)) {
        if (error ||
            !entry.is_regular_file()) {
            continue;
        }

        const std::string ext =
            Stage36Lowercase(
                entry.path()
                    .extension()
                    .string());

        if (ext == ".wav") {
            audio.emplace_back(
                entry.path());
        } else if (
            ext == ".iso" ||
            ext == ".bin") {
            data.emplace_back(
                entry.path());
        }
    }

    if (error) {
        errorMessage =
            "Could not inspect the extracted CDI tracks: " +
            error.message();
        return false;
    }

    std::sort(
        audio.begin(),
        audio.end());

    std::sort(
        data.begin(),
        data.end());

    if (!audio.empty() &&
        !data.empty()) {
        layout.audioData = true;
        layout.firstSession =
            std::move(audio);
        layout.secondSession =
            std::move(data);
        layout.description =
            "Audio+Data self-boot CDI";
    } else if (
        audio.empty() &&
        data.size() >= 2U) {
        layout.firstSession.push_back(
            data.front());

        layout.secondSession.assign(
            data.begin() + 1,
            data.end());

        layout.description =
            "Data+Data self-boot CDI";
    } else {
        errorMessage =
            "CDIrip did not produce a supported two-session Dreamcast layout.";
        return false;
    }

    layout.firstBytes =
        Stage36SumFileSizes(
            layout.firstSession);

    layout.secondBytes =
        Stage36SumFileSizes(
            layout.secondSession);

    if (layout.firstBytes == 0 ||
        layout.secondBytes == 0) {
        errorMessage =
            "One of the extracted Dreamcast sessions is empty.";
        return false;
    }

    return true;
}

} // namespace

BurnEngine::~BurnEngine()
{
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

bool BurnEngine::Start(BurnRequest request)
{
    {
        std::lock_guard lock(mutex_);
        if (state_.busy)
            return false;

        state_ = BurnSnapshot{};
        state_.stage = BurnStage::Preparing;
        state_.busy = true;
        state_.writing = false;
        state_.progress = 0.0F;
        state_.status =
            request.checkOnly
                ? "Running Linux optical preflight..."
                : (request.simulate
                    ? "Preparing Linux dummy write..."
                    : "Validating image and blank CD-R...");
        state_.log =
            "Retro Burner Linux BurnEngine\n"
            "Shared BurnRequest/BurnSnapshot API active.\n";
        logPendingCarriageReturn_ = false;
    }

    if (worker_.joinable())
        worker_.join();

    worker_ = std::jthread(
        [this, request = std::move(request)]() mutable {
            Run(std::move(request));
        });
    return true;
}

bool BurnEngine::StartBurnerMaxTest(
    std::wstring opticalDriveRoot)
{
    {
        std::lock_guard lock(mutex_);

        if (state_.busy)
            return false;

        state_ = BurnSnapshot{};
        logPendingCarriageReturn_ = false;
        state_.busy = true;
        state_.stage =
            BurnStage::Preparing;
        state_.status =
            "Testing / enabling BurnerMAX...";
    }

    if (worker_.joinable())
        worker_.join();

    worker_ = std::jthread(
        [this,
         opticalDriveRoot =
             std::move(opticalDriveRoot)]() mutable {
            BurnRequest request;
            request.target =
                BurnTarget::Xbox360;
            request.xbox360DiscType =
                Xbox360DiscType::Xgd3;
            request.opticalDriveRoot =
                std::move(
                    opticalDriveRoot);
            request.cdrecordDevice =
                Stage30FindSgForBlockRoot(
                    Stage30Path(
                        request.opticalDriveRoot));
            request.burnerMaxOnly =
                true;

            AppendLog(
                "BurnerMAX standalone test\n"
                "Image validation: BYPASSED (not required)\n"
                "Disc sector writes: DISABLED\n");

            if (request.cdrecordDevice.empty()) {
                SetFailure(
                    "Could not map the selected Linux DVD writer "
                    "to an exact optical /dev/sgX.");
                return;
            }

            RunBurnerMaxOnly(
                std::move(request),
                L"");
        });

    return true;
}

BurnSnapshot BurnEngine::Snapshot() const
{
    std::lock_guard lock(
        mutex_);

    BurnSnapshot snapshot =
        state_;

    snapshot.xgd3Prepared =
        preparedXgd3_.valid;

    snapshot.preparedXgd3SourcePath =
        preparedXgd3_.sourcePath;

    snapshot.preparedXgd3WorkingPath =
        preparedXgd3_.workingImagePath;

    return snapshot;
}

void BurnEngine::Reset()
{
    if (worker_.joinable())
        worker_.join();

    std::lock_guard lock(mutex_);
    if (!state_.busy) {
        state_ = BurnSnapshot{};
        logPendingCarriageReturn_ = false;
    }
}

void BurnEngine::SetPreparationProgress(
    float progress,
    std::string status)
{
    std::lock_guard lock(mutex_);
    state_.progress = std::clamp(progress, 0.0F, 1.0F);
    state_.status = std::move(status);
    state_.writing = false;
}

void BurnEngine::Run(BurnRequest request)
{
    if (request.burnerMaxOnly) {
        RunBurnerMaxOnly(
            std::move(request),
            L"");
        return;
    }

    // All console profiles now use one Linux BurnEngine routing function.
    // Platform-specific mechanics remain in the native helpers/backends.
    RunStandardImage(
        std::move(request),
        L"",
        L"",
        L"",
        L"");
}

void BurnEngine::RunStandardImage(
    BurnRequest request,
    const std::wstring&,
    const std::wstring&,
    const std::wstring&,
    const std::wstring&)
{
    const fs::path imagePath =
        Stage30Path(
            request.cdiPath);

    // ============================================================
    // Xbox 360 XGD3 - FULL PREFLIGHT
    // ============================================================
    if (request.target ==
            BurnTarget::Xbox360 &&
        request.xbox360DiscType ==
            Xbox360DiscType::Xgd3) {

        std::error_code error;

        if (!fs::is_regular_file(
                imagePath,
                error)) {
            SetFailure(
                "Linux cannot find the selected Xbox 360 ISO.");
            return;
        }

        const std::uintmax_t imageBytes =
            fs::file_size(
                imagePath,
                error);

        constexpr std::uintmax_t
            kXgd3FullImageBytes =
                8738846720ULL;

        if (error ||
            imageBytes == 0 ||
            (imageBytes %
             2048ULL) != 0ULL) {
            SetFailure(
                "The Xbox 360 ISO is empty or is not aligned to "
                "2048-byte sectors.");
            return;
        }

        if (imageBytes >
            kXgd3FullImageBytes) {
            SetFailure(
                "This XGD3 ISO is larger than the supported full "
                "XGD3 image size.");
            return;
        }

        {
            std::lock_guard lock(mutex_);
            state_.layout =
                "Xbox 360 XGD3 ISO - DVD+R DL / BurnerMAX";
            state_.status =
                "Image validated: Xbox 360 XGD3 ISO - DVD+R DL / BurnerMAX";
        }

        AppendLog(
            "\nRetro Burner image check\n"
            "Target: Xbox 360\n"
            "Image: " +
            imagePath.string() +
            "\n"
            "Layout: Xbox 360 XGD3 ISO - DVD+R DL / BurnerMAX\n");

        if (request.checkOnly) {
            std::lock_guard lock(
                mutex_);

            state_.stage =
                BurnStage::Ready;
            state_.busy = false;
            state_.writing = false;
            state_.progress = 0.0F;
            state_.status =
                "Xbox 360 XGD3 ISO - DVD+R DL / BurnerMAX. "
                "Check complete; no disc was written.";
            return;
        }


        fs::path burnImagePath =
            imagePath;

        std::wstring preparedWorkingImage;

        if (TryReusePreparedXgd3(
                request.cdiPath,
                preparedWorkingImage)) {
            burnImagePath =
                Stage30Path(
                    preparedWorkingImage);

            AppendLog(
                "\nXGD3 prepared-image cache\n"
                "Reusing the ABGX360-verified working copy from the "
                "successful preflight/preparation pass.\n"
                "Original ISO: " +
                imagePath.string() +
                "\n"
                "Prepared ISO: " +
                burnImagePath.string() +
                "\n"
                "ABGX360 copy/AutoFix/verification will not be repeated.\n");

            SetPreparationProgress(
                1.0F,
                "XGD3 already prepared - reusing verified ABGX360 working copy.");
        } else {
            const fs::path abgx360 =
                Stage30SiblingExecutable(
                    "abgx360");

            if (abgx360.empty()) {
                SetFailure(
                    "Native ABGX360 was not found beside RetroBurner. "
                    "XGD3 burns require an ABGX360 AutoFix pass.");
                return;
            }

            const fs::path parent =
                imagePath.has_parent_path()
                    ? imagePath.parent_path()
                    : fs::current_path();

            const fs::space_info space =
                fs::space(
                    parent,
                    error);

            constexpr std::uintmax_t
                kFreeSpaceMargin =
                    256ULL *
                    1024ULL *
                    1024ULL;

            if (!error &&
                space.available <
                    imageBytes +
                    kFreeSpaceMargin) {
                SetFailure(
                    "Not enough free space beside the XGD3 ISO to create "
                    "the temporary ABGX360 working copy. "
                    "The original ISO is never modified.");
                return;
            }

            {
                std::lock_guard lock(mutex_);
                state_.status =
                    "Creating temporary XGD3 working copy for ABGX360...";
                state_.writing = false;
                state_.progress = 0.0F;
            }

            const fs::path tempDirectory =
                parent /
                (".RetroBurner-ABGX360-" +
                 std::to_string(
                     static_cast<long long>(
                         ::getpid())));

            fs::remove_all(
                tempDirectory,
                error);

            error.clear();

            fs::create_directories(
                tempDirectory,
                error);

            if (error) {
                SetFailure(
                    "Could not create a temporary ABGX360 working "
                    "directory beside the selected ISO.");
                return;
            }

            burnImagePath =
                tempDirectory /
                imagePath.filename();

            AppendLog(
                "\nABGX360 XGD3 prerequisite\n"
                "Retro Burner will AutoFix a temporary working copy.\n"
                "The selected original ISO will not be modified.\n"
                "Working image: " +
                burnImagePath.string() +
                "\n");

            std::ifstream input(
                imagePath,
                std::ios::binary);

            std::ofstream output(
                burnImagePath,
                std::ios::binary |
                    std::ios::trunc);

            if (!input ||
                !output) {
                fs::remove_all(
                    tempDirectory,
                    error);

                SetFailure(
                    "Could not create the temporary ABGX360 working copy.");
                return;
            }

            std::vector<char> buffer(
                8U *
                1024U *
                1024U);

            std::uintmax_t copied = 0;

            while (input) {
                input.read(
                    buffer.data(),
                    static_cast<std::streamsize>(
                        buffer.size()));

                const std::streamsize count =
                    input.gcount();

                if (count <= 0)
                    break;

                output.write(
                    buffer.data(),
                    count);

                if (!output) {
                    fs::remove_all(
                        tempDirectory,
                        error);

                    SetFailure(
                        "Writing the temporary ABGX360 working copy failed.");
                    return;
                }

                copied +=
                    static_cast<std::uintmax_t>(
                        count);

                SetPreparationProgress(
                    static_cast<float>(
                        std::min(
                            copied,
                            imageBytes)) /
                        static_cast<float>(
                            imageBytes),
                    "Creating temporary XGD3 working copy for ABGX360...");
            }

            output.close();

            if (copied !=
                imageBytes) {
                fs::remove_all(
                    tempDirectory,
                    error);

                SetFailure(
                    "The temporary ABGX360 working copy is incomplete.");
                return;
            }

            static const std::regex
                percentPattern(
                    R"(([0-9]{1,3})%)");

            auto runAbgx =
                [this,
                 &abgx360,
                 &burnImagePath,
                 &tempDirectory,
                 &percentPattern](
                    const std::vector<std::string>& arguments,
                    const std::string& phase,
                    std::string& captured)
                    -> CapturedProcessResult {
                    captured.clear();

                    return
                        RunProcessCapture(
                            abgx360,
                            arguments,
                            tempDirectory,
                            [this,
                             &captured,
                             &phase,
                             &percentPattern](
                                std::string_view chunk) {
                                const std::string text(
                                    chunk);

                                captured +=
                                    text;

                                AppendLog(text);

                                std::smatch match;

                                if (std::regex_search(
                                        text,
                                        match,
                                        percentPattern)) {
                                    const int percent =
                                        std::clamp(
                                            std::stoi(
                                                match[1].str()),
                                            0,
                                            100);

                                    SetPreparationProgress(
                                        static_cast<float>(
                                            percent) /
                                            100.0F,
                                        phase);
                                }
                            });
                };

            SetPreparationProgress(
                0.0F,
                "ABGX360 AutoFix Level 3 - starting...");

            AppendLog(
                "\nABGX360 AutoFix Level 3\n");

            std::string autoFixOutput;

            const CapturedProcessResult autoFix =
                runAbgx(
                    {
                        "-s",
                        "--af3",
                        "--",
                        burnImagePath.string(),
                    },
                    "ABGX360 AutoFix Level 3...",
                    autoFixOutput);

            if (!autoFix.started ||
                autoFix.exitCode != 0) {
                fs::remove_all(
                    tempDirectory,
                    error);

                SetFailure(
                    "ABGX360 AutoFix could not complete. "
                    "XGD3 burning has been stopped before BurnerMAX "
                    "or disc writing.");
                return;
            }

            if (Stage30ContainsCaseInsensitive(
                    autoFixOutput,
                    "autofix failed") ||
                Stage30ContainsCaseInsensitive(
                    autoFixOutput,
                    "aborting autofix")) {
                fs::remove_all(
                    tempDirectory,
                    error);

                SetFailure(
                    "ABGX360 reported that AutoFix failed. "
                    "XGD3 burning has been stopped; check the ABGX360 log.");
                return;
            }

            SetPreparationProgress(
                0.0F,
                "ABGX360 verification - starting read-only pass...");

            AppendLog(
                "\nABGX360 verification - writes disabled\n");

            std::string verifyOutput;

            const CapturedProcessResult verify =
                runAbgx(
                    {
                        "-s",
                        "-w",
                        "-o",
                        "--af3",
                        "--",
                        burnImagePath.string(),
                    },
                    "ABGX360 verification...",
                    verifyOutput);

            if (!verify.started ||
                verify.exitCode != 0) {
                fs::remove_all(
                    tempDirectory,
                    error);

                SetFailure(
                    "ABGX360 verification could not complete. "
                    "XGD3 burning has been stopped before BurnerMAX "
                    "or disc writing.");
                return;
            }

            if (!Stage30ContainsCaseInsensitive(
                    verifyOutput,
                    "checking topology data")) {
                fs::remove_all(
                    tempDirectory,
                    error);

                SetFailure(
                    "ABGX360 did not report an XGD3 topology-data check. "
                    "Confirm that the selected image is an XGD3 Xbox 360 ISO.");
                return;
            }

            if (Stage30ContainsCaseInsensitive(
                    verifyOutput,
                    "first 12 sectors of topology data are blank") ||
                Stage30ContainsCaseInsensitive(
                    verifyOutput,
                    "topology data is blank") ||
                Stage30ContainsCaseInsensitive(
                    verifyOutput,
                    "autofix failed") ||
                Stage30ContainsCaseInsensitive(
                    verifyOutput,
                    "aborting autofix")) {
                fs::remove_all(
                    tempDirectory,
                    error);

                SetFailure(
                    "ABGX360 verification still reports missing/unfixed "
                    "XGD3 data. The burn has been blocked.");
                return;
            }

            const bool onlineWarning =
                Stage30ContainsCaseInsensitive(
                    verifyOutput,
                    "verification failed");

            AppendLog(
                onlineWarning
                    ? "\nABGX360: AutoFix completed and topology is present. "
                      "Online game verification was not conclusive; review the log.\n"
                    : "\nABGX360: XGD3 AutoFix prerequisite passed.\n");

            SetPreparationProgress(
                1.0F,
                onlineWarning
                    ? "ABGX360 patched XGD3; online verification warning - continuing preflight..."
                    : "ABGX360 XGD3 prerequisite passed.");

            StorePreparedXgd3(
                request.cdiPath,
                fs::path(
                    tempDirectory)
                    .wstring(),
                fs::path(
                    burnImagePath)
                    .wstring());

            AppendLog(
                "\nXGD3 prepared working copy cached for this Retro Burner session.\n"
                "A following Burn will reuse this verified copy instead of "
                "repeating the ISO copy and ABGX360 passes.\n");
        }

        const fs::path blockDevice =
            Stage30Path(
                request.opticalDriveRoot);

        const fs::path mediaInfo =
            Stage30SiblingExecutable(
                "dvd+rw-mediainfo");

        if (blockDevice.empty() ||
            mediaInfo.empty()) {
            SetFailure(
                "The selected Linux DVD writer or dvd+rw-mediainfo "
                "backend is unavailable.");
            return;
        }

        std::string mediaOutput;

        const CapturedProcessResult media =
            RunProcessCapture(
                mediaInfo,
                {blockDevice.string()},
                {});

        mediaOutput =
            media.output;

        AppendLog(
            "\nDVD media preflight (dvd+rw-mediainfo)\n" +
            mediaOutput);

        if (!media.started ||
            media.exitCode != 0) {
            SetFailure(
                "dvd+rw-mediainfo could not validate the selected DVD writer/media.");
            return;
        }

        static const std::regex mountedProfilePattern(
            R"(Mounted Media:\s+([0-9A-Fa-f]+)h,\s*([^\r\n]+))",
            std::regex::icase);
        static const std::regex blankDiscPattern(
            R"(Disc status:\s+blank)",
            std::regex::icase);
        static const std::regex freeBlocksPattern(
            R"(Free Blocks:\s+([0-9]+)\*2KB)",
            std::regex::icase);

        std::smatch match;
        unsigned mediaProfile = 0;

        if (std::regex_search(
                mediaOutput,
                match,
                mountedProfilePattern)) {
            mediaProfile =
                static_cast<unsigned>(
                    std::stoul(
                        match[1].str(),
                        nullptr,
                        16));
        }

        if (mediaProfile != 0x2B) {
            SetFailure(
                "Xbox 360 burning requires DVD+R DL media.");
            return;
        }

        if (!std::regex_search(
                mediaOutput,
                blankDiscPattern)) {
            SetFailure(
                "The inserted DVD+R DL must be positively reported as blank.");
            return;
        }

        if (!request.useGrowisofsForDvd) {
            const fs::path retrobeam =
                Stage30SiblingExecutable(
                    "retrobeam");

            if (retrobeam.empty() ||
                request.cdrecordDevice.empty()) {
                SetFailure(
                    "RetroBeam has no mapped SCSI address for the selected DVD writer. "
                    "Press Refresh and try again.");
                return;
            }

            AppendLog(
                "\nRetroBeam read-only drive preflight\n");

            const CapturedProcessResult check =
                RunProcessCapture(
                    retrobeam,
                    {
                        "dev=" +
                            request.cdrecordDevice,
                        "-checkdrive",
                    },
                    {});

            AppendLog(
                check.output);

            if (!check.started ||
                check.exitCode != 0) {
                SetFailure(
                    "RetroBeam could not validate the selected DVD writer. "
                    "No image data was written.");
                return;
            }
        } else {
            AppendLog(
                "\nDVD backend selected: growisofs\n"
                "RetroBeam drive preflight is intentionally skipped for this A/B path.\n");
        }

        {
            std::lock_guard lock(mutex_);
            state_.status =
                "Testing / enabling BurnerMAX for XGD3...";
        }

        const BurnerMaxResult burnerMax =
            EnableBurnerMax(
                request.cdrecordDevice,
                [this](
                    const std::string& text) {
                    AppendLog(text);
                });

        if (!burnerMax.Success()) {
            SetFailure(
                "XGD3 BurnerMAX preflight failed: " +
                burnerMax.message);
            return;
        }

        const CapturedProcessResult refreshed =
            RunProcessCapture(
                mediaInfo,
                {blockDevice.string()},
                {});

        AppendLog(
            "\nDVD media capacity after BurnerMAX "
            "(dvd+rw-mediainfo)\n" +
            refreshed.output);

        if (!refreshed.started ||
            refreshed.exitCode != 0) {
            SetFailure(
                "BurnerMAX verification succeeded, but Retro Burner "
                "could not re-read the expanded writable capacity. "
                "No image data was written.");
            return;
        }

        std::uintmax_t freeBlocks = 0;

        if (std::regex_search(
                refreshed.output,
                match,
                freeBlocksPattern)) {
            freeBlocks =
                std::stoull(
                    match[1].str());
        }

        constexpr std::uintmax_t
            kBurnerMaxFullCapacitySectors =
                4267040ULL;

        if (freeBlocks <
            kBurnerMaxFullCapacitySectors) {
            SetFailure(
                "BurnerMAX layer-boundary verification passed, but the drive "
                "reports only " +
                std::to_string(
                    freeBlocks) +
                " writable sectors. Full XGD3 capacity requires at least "
                "4267040 sectors.");
            return;
        }

        AppendLog(
            "\nBurnerMAX expanded capacity verified: " +
            std::to_string(
                freeBlocks) +
            " sectors.\n");

        const std::uintmax_t preparedBytes =
            fs::file_size(
                burnImagePath,
                error);

        if (error ||
            preparedBytes >
                freeBlocks *
                    2048ULL) {
            SetFailure(
                "The prepared XGD3 image does not fit the writable "
                "capacity reported after BurnerMAX.");
            return;
        }

        // ========================================================
        // XGD3 BACKEND EXECUTION
        //
        // simulate=true  -> FULL XGD3 PREFLIGHT, no disc-sector write
        // simulate=false -> real physical BURN XBOX 360 request
        // ========================================================
        if (request.useGrowisofsForDvd) {
            const fs::path growisofs =
                Stage30SiblingExecutable(
                    "growisofs");

            if (growisofs.empty()) {
                SetFailure(
                    "growisofs was not found.");
                return;
            }

            std::vector<std::string> arguments;

            if (request.simulate) {
                arguments.emplace_back(
                    "-dry-run");
            }

            arguments.emplace_back(
                "-use-the-force-luke=dao");

            arguments.emplace_back(
                "-use-the-force-luke=break:2133520");

            arguments.emplace_back(
                "-dvd-compat");

            if (request.requestedSpeedX > 0) {
                arguments.emplace_back(
                    "-speed=" +
                    std::to_string(
                        request.requestedSpeedX));
            }

            arguments.emplace_back(
                "-Z");

            arguments.emplace_back(
                blockDevice.string() +
                "=" +
                burnImagePath.string());

            {
                std::lock_guard lock(mutex_);

                state_.stage =
                    BurnStage::BurningSession1;
                state_.busy = true;
                state_.writing =
                    !request.simulate;
                state_.progress = 0.0F;
                state_.session = 1;
                state_.bufferPercent = -1;
                state_.ringBufferPercent = -1;
                state_.driveBufferPercent = -1;
                state_.actualSpeed.clear();
                state_.remainingTime.clear();
                state_.status =
                    request.simulate
                        ? "Xbox 360 XGD3 growisofs dry run - no write..."
                        : "Writing Xbox 360 XGD3 with growisofs...";
            }

            AppendLog(
                request.simulate
                    ? "\nDVD DRY RUN - growisofs -dry-run\n"
                      "Write type: DAO\n"
                      "Xbox layer break: 2133520\n"
                    : "\nDVD WRITE - growisofs\n"
                      "Write type: DAO\n"
                      "Xbox layer break: 2133520\n");

            std::string progressWindow;

            static const std::regex
                growisofsPercentPattern(
                    R"(([0-9]+(?:\.[0-9]+)?)%\s+done)",
                    std::regex::icase);

            const CapturedProcessResult result =
                RunProcessCapture(
                    growisofs,
                    arguments,
                    burnImagePath.parent_path(),
                    [this,
                     &progressWindow](
                        std::string_view chunk) {
                        const std::string text(
                            chunk);

                        AppendLog(text);

                        progressWindow +=
                            text;

                        if (progressWindow.size() >
                            8192U) {
                            progressWindow.erase(
                                0,
                                progressWindow.size() -
                                    8192U);
                        }

                        std::smatch match;

                        if (std::regex_search(
                                progressWindow,
                                match,
                                growisofsPercentPattern)) {
                            const float percent =
                                std::clamp(
                                    std::stof(
                                        match[1].str()),
                                    0.0F,
                                    100.0F);

                            std::lock_guard lock(
                                mutex_);

                            state_.progress =
                                percent /
                                100.0F;
                        }
                    });

            if (!result.started ||
                result.exitCode != 0) {
                SetFailure(
                    request.simulate
                        ? "Xbox 360 XGD3 growisofs dry run failed. "
                          "No image data was intentionally written."
                        : "Xbox 360 XGD3 growisofs write failed. "
                          "The disc may be incomplete; check the Burn Log.");
                return;
            }

            if (request.simulate) {
                std::lock_guard lock(
                    mutex_);

                state_.stage =
                    BurnStage::Ready;
                state_.busy = false;
                state_.writing = false;
                state_.progress = 1.0F;
                state_.status =
                    "XGD3 growisofs preflight passed. "
                    "Prepared ABGX360 copy cached; Burn will reuse it.";
                return;
            }

            AppendLog(
                "\nXGD3 growisofs burn completed successfully. "
                "Removing the cached prepared working copy.\n");

            ClearPreparedXgd3();

            const bool ejected =
                Stage33EjectOpticalDrive(
                    blockDevice);

            if (!ejected) {
                AppendLog(
                    "WARNING: Burn succeeded, but Linux could not "
                    "automatically eject the disc.\n");
            }

            std::lock_guard lock(
                mutex_);

            state_.stage =
                BurnStage::Complete;
            state_.busy = false;
            state_.writing = false;
            state_.progress = 1.0F;
            state_.status =
                ejected
                    ? "Burn complete! Disc ejected. Backend: growisofs."
                    : "Burn complete! Backend: growisofs.";
            return;
        }

        const fs::path retrobeam =
            Stage30SiblingExecutable(
                "retrobeam");

        if (retrobeam.empty() ||
            request.cdrecordDevice.empty()) {
            SetFailure(
                "RetroBeam has no mapped SCSI address for the selected "
                "DVD writer. Press Refresh and try again.");
            return;
        }

        if (request.simulate) {
            AppendLog(
                "\nXbox 360 DVD BACKEND: RetroBeam / libscg\n"
                "Xbox layer break: 2133520\n"
                "Write type: DAO (locked for console DVD media)\n");

            AppendLog(
                Stage33RetroBeamPolicyText(
                    request));

            AppendLog(
                "RetroBeam drive/backend preflight complete. "
                "No media WRITE command was issued and no disc sectors "
                "were written.\n");

            std::lock_guard lock(
                mutex_);

            state_.stage =
                BurnStage::Ready;
            state_.busy = false;
            state_.writing = false;
            state_.progress = 1.0F;
            state_.status =
                "XGD3 RetroBeam preflight passed. "
                "Prepared ABGX360 copy cached; Burn will reuse it.";
            return;
        }

        {
            std::lock_guard lock(
                mutex_);

            state_.stage =
                BurnStage::BurningSession1;
            state_.busy = true;
            state_.writing = true;
            state_.progress = 0.0F;
            state_.session = 1;
            state_.bufferPercent = -1;
            state_.ringBufferPercent = -1;
            state_.driveBufferPercent = -1;
            state_.actualSpeed.clear();
            state_.remainingTime.clear();
            state_.status =
                "Writing Lead-In...";
        }

        std::vector<std::string>
            retrobeamArguments;

        retrobeamArguments.emplace_back(
            "dev=" +
            request.cdrecordDevice);

        retrobeamArguments.emplace_back(
            "-v");

        retrobeamArguments.emplace_back(
            "-dao");

        if (request.requestedSpeedX > 0) {
            retrobeamArguments.emplace_back(
                "speed=" +
                std::to_string(
                    request.requestedSpeedX));
        }

        retrobeamArguments.emplace_back(
            "fs=32m");

        retrobeamArguments.emplace_back(
            Stage33RetroBeamDriverOptions(
                request,
                2133520ULL));

        retrobeamArguments.emplace_back(
            "-data");

        retrobeamArguments.emplace_back(
            burnImagePath.string());

        AppendLog(
            "\nXbox 360 DVD BACKEND: RetroBeam / libscg\n"
            "Xbox layer break: 2133520\n"
            "Write type: DAO (locked for console DVD media)\n");

        AppendLog(
            Stage33RetroBeamPolicyText(
                request));

        std::string progressWindow;

        static const std::regex
            retrobeamProgressPattern(
                R"(Track\s+[0-9]+:\s*([0-9]+)\s+of\s+([0-9]+)\s+MB written.*?\(fifo\s+([0-9]+)%\).*?\[buf\s+([0-9]+)%\].*?([0-9]+(?:\.[0-9]+)?x))",
                std::regex::icase);

        const CapturedProcessResult retrobeamResult =
            RunProcessCapture(
                retrobeam,
                retrobeamArguments,
                burnImagePath.parent_path(),
                [this,
                 &progressWindow](
                    std::string_view chunk) {
                    const std::string text(
                        chunk);

                    AppendLog(text);

                    progressWindow +=
                        text;

                    if (progressWindow.size() >
                        8192U) {
                        progressWindow.erase(
                            0,
                            progressWindow.size() -
                                8192U);
                    }

                    std::smatch match;

                    if (!std::regex_search(
                            progressWindow,
                            match,
                            retrobeamProgressPattern)) {
                        return;
                    }

                    const float written =
                        std::stof(
                            match[1].str());

                    const float total =
                        std::stof(
                            match[2].str());

                    const int fifo =
                        std::clamp(
                            std::stoi(
                                match[3].str()),
                            0,
                            100);

                    const int buffer =
                        std::clamp(
                            std::stoi(
                                match[4].str()),
                            0,
                            100);

                    std::lock_guard lock(
                        mutex_);

                    if (total > 0.0F) {
                        state_.progress =
                            std::clamp(
                                written /
                                    total,
                                0.0F,
                                1.0F);
                    }

                    state_.ringBufferPercent =
                        fifo;

                    state_.bufferPercent =
                        fifo;

                    state_.driveBufferPercent =
                        buffer;

                    state_.actualSpeed =
                        match[5].str();

                    state_.status =
                        "Writing Xbox 360 XGD3 with RetroBeam...";
                });

        if (request.advanced.useStreamingPolicy &&
            request.advanced.restoreStreamingDefaults) {
            AppendLog(
                "\nRestoring RetroBeam MMC streaming defaults...\n");

            const CapturedProcessResult restore =
                RunProcessCapture(
                    retrobeam,
                    {
                        "dev=" +
                            request.cdrecordDevice,
                        "driveropts=streamrestore",
                        "-setdropts",
                    },
                    burnImagePath.parent_path(),
                    [this](
                        std::string_view chunk) {
                        AppendLog(
                            std::string(
                                chunk));
                    });

            if (!restore.started ||
                restore.exitCode != 0) {
                AppendLog(
                    "WARNING: Could not restore RetroBeam streaming "
                    "defaults automatically. Power-cycling the drive "
                    "will clear volatile settings.\n");
            }
        }

        if (!retrobeamResult.started ||
            retrobeamResult.exitCode != 0) {
            SetFailure(
                "Xbox 360 XGD3 RetroBeam write failed. "
                "The disc may be incomplete; check the Burn Log.");
            return;
        }

        AppendLog(
            "\nXGD3 burn completed successfully. "
            "Removing the cached prepared working copy.\n");

        ClearPreparedXgd3();

        const bool ejected =
            Stage33EjectOpticalDrive(
                blockDevice);

        if (!ejected) {
            AppendLog(
                "WARNING: Burn succeeded, but Linux could not "
                "automatically eject the disc.\n");
        }

        std::lock_guard lock(
            mutex_);

        state_.stage =
            BurnStage::Complete;
        state_.busy = false;
        state_.writing = false;
        state_.progress = 1.0F;
        state_.status =
            ejected
                ? "Burn complete! Disc ejected. Backend: RetroBeam."
                : "Burn complete! Backend: RetroBeam.";
        return;

    }

    // ============================================================
    // Stage 36: ALL REMAINING TARGETS ARE REAL ROUTES
    // ============================================================

    std::error_code stage36FileError;

    if (!fs::is_regular_file(
            imagePath,
            stage36FileError)) {
        SetFailure(
            "Linux cannot find the selected disc image.");
        return;
    }

    const std::string extension =
        Stage36Lowercase(
            imagePath.extension().string());

    const fs::path blockDevice =
        Stage30Path(
            request.opticalDriveRoot);

    const fs::path retrobeam =
        Stage30SiblingExecutable(
            "retrobeam");

    if (retrobeam.empty()) {
        SetFailure(
            "Embedded RetroBeam could not be prepared.");
        return;
    }

    // ------------------------------------------------------------
    // DREAMCAST CDI
    // ------------------------------------------------------------
    if (request.target ==
        BurnTarget::Dreamcast) {
        if (extension != ".cdi") {
            SetFailure(
                "Dreamcast burning requires a .cdi image.");
            return;
        }

        const fs::path cdirip =
            Stage30SiblingExecutable(
                "cdirip");

        if (cdirip.empty()) {
            SetFailure(
                "Embedded native cdirip could not be prepared.");
            return;
        }

        Stage36TemporaryDirectory temporary =
            Stage36MakeTemporaryDirectory();

        if (temporary.path.empty()) {
            SetFailure(
                "Linux could not create a temporary CDI extraction directory.");
            return;
        }

        if (!request.checkOnly) {
            if (request.cdrecordDevice.empty()) {
                SetFailure(
                    "RetroBeam has no mapped SCSI address for the selected CD writer.");
                return;
            }

            AppendLog(
                "\nRetroBeam read-only CD-R preflight\n");

            const CapturedProcessResult preflight =
                RunProcessCapture(
                    retrobeam,
                    {
                        "dev=" +
                            request.cdrecordDevice,
                        "-atip",
                    },
                    imagePath.parent_path(),
                    [this](
                        std::string_view chunk) {
                        AppendLog(
                            std::string(chunk));
                    });

            if (!preflight.started ||
                preflight.exitCode != 0) {
                SetFailure(
                    "The selected burner or blank CD-R failed Dreamcast preflight. "
                    "No disc sectors were written.");
                return;
            }
        }

        {
            std::lock_guard lock(
                mutex_);

            state_.status =
                "Extracting and analysing CDI tracks...";
        }

        AppendLog(
            "\nCDIrip extraction\n");

        const CapturedProcessResult extraction =
            RunProcessCapture(
                cdirip,
                {
                    imagePath.string(),
                    temporary.path.string(),
                    "-iso",
                },
                temporary.path,
                [this](
                    std::string_view chunk) {
                    AppendLog(
                        std::string(chunk));
                });

        if (!extraction.started ||
            extraction.exitCode != 0) {
            SetFailure(
                "CDIrip could not extract this Dreamcast image.");
            return;
        }

        Stage36DreamcastLayout layout;
        std::string layoutError;

        if (!Stage36DetectDreamcastLayout(
                temporary.path,
                layout,
                layoutError)) {
            SetFailure(
                std::move(layoutError));
            return;
        }

        {
            std::lock_guard lock(
                mutex_);

            state_.layout =
                layout.description;
        }

        AppendLog(
            "\nDetected: " +
            layout.description +
            "\n");

        if (request.checkOnly) {
            std::lock_guard lock(
                mutex_);

            state_.stage =
                BurnStage::Ready;
            state_.busy = false;
            state_.writing = false;
            state_.status =
                layout.description +
                ". Check complete; no disc was written.";
            return;
        }

        const auto runSession =
            [this,
             &request,
             &retrobeam,
             &temporary](
                const int session,
                const bool audioSession,
                const std::vector<fs::path>& tracks) -> bool {
                {
                    std::lock_guard lock(
                        mutex_);

                    state_.stage =
                        session == 1
                            ? BurnStage::BurningSession1
                            : BurnStage::BurningSession2;
                    state_.busy = true;
                    state_.writing = true;
                    state_.session =
                        session;
                    state_.progress =
                        session == 1
                            ? 0.0F
                            : 0.5F;
                    state_.bufferPercent = -1;
                    state_.ringBufferPercent = -1;
                    state_.driveBufferPercent = -1;
                    state_.actualSpeed.clear();
                    state_.status =
                        "Burning Dreamcast session " +
                        std::to_string(session) +
                        " of 2";
                }

                std::vector<std::string> args;

                args.emplace_back(
                    "dev=" +
                    request.cdrecordDevice);

                args.emplace_back("-v");

                if (request.requestedSpeedX > 0) {
                    args.emplace_back(
                        "speed=" +
                        std::to_string(
                            request.requestedSpeedX));
                }

                args.emplace_back(
                    Stage36RetroBeamDriverOptions(
                        request,
                        false));

                if (session == 1) {
                    args.emplace_back(
                        audioSession
                            ? "-dao"
                            : "-tao");
                    args.emplace_back("-multi");

                    if (!audioSession)
                        args.emplace_back("-xa");
                } else {
                    args.emplace_back("-overburn");
                    args.emplace_back("-tao");
                    args.emplace_back("-xa");
                }

                for (const fs::path& track :
                     tracks) {
                    args.emplace_back(
                        track.filename().string());
                }

                std::string progressWindow;

                const CapturedProcessResult result =
                    RunProcessCapture(
                        retrobeam,
                        args,
                        temporary.path,
                        [this,
                         &progressWindow,
                         session](
                            std::string_view chunk) {
                            const std::string text(
                                chunk);

                            AppendLog(text);

                            progressWindow += text;

                            if (progressWindow.size() >
                                8192U) {
                                progressWindow.erase(
                                    0,
                                    progressWindow.size() -
                                        8192U);
                            }

                            const RetroBeamProgressUpdate update =
                                ParseRetroBeamProgressText(
                                    progressWindow);

                            if (!update.HasTelemetry())
                                return;

                            std::lock_guard lock(
                                mutex_);

                            if (!update.phaseStatus.empty()) {
                                state_.status =
                                    update.phaseStatus;
                            }

                            if (update.progress >= 0.0F) {
                                const float base =
                                    session == 1
                                        ? 0.0F
                                        : 0.5F;

                                state_.progress =
                                    std::max(
                                        state_.progress,
                                        base +
                                            update.progress *
                                                0.5F);

                                if (update.phaseStatus.empty()) {
                                    state_.status =
                                        "Writing Dreamcast session " +
                                        std::to_string(session) +
                                        " - " +
                                        std::to_string(
                                            static_cast<int>(
                                                std::lround(
                                                    state_.progress *
                                                    100.0F))) +
                                        "%";
                                }
                            }

                            if (update.fifoPercent >= 0)
                                state_.ringBufferPercent =
                                    update.fifoPercent;

                            if (update.bufferPercent >= 0) {
                                state_.bufferPercent =
                                    update.bufferPercent;
                                state_.driveBufferPercent =
                                    update.bufferPercent;
                            }

                            if (!update.speed.empty())
                                state_.actualSpeed =
                                    update.speed;
                        });

                if (!result.started ||
                    result.exitCode != 0) {
                    SetFailure(
                        "Dreamcast session " +
                        std::to_string(session) +
                        " failed. The disc is incomplete.");
                    return false;
                }

                return true;
            };

        if (!runSession(
                1,
                layout.audioData,
                layout.firstSession)) {
            return;
        }

        if (!runSession(
                2,
                false,
                layout.secondSession)) {
            return;
        }

        const bool ejected =
            Stage36Eject(
                blockDevice);

        if (!ejected) {
            AppendLog(
                "\nWARNING: Dreamcast burn succeeded, but Linux could not "
                "automatically eject the disc.\n");
        }

        std::lock_guard lock(
            mutex_);

        state_.stage =
            BurnStage::Complete;
        state_.busy = false;
        state_.writing = false;
        state_.progress = 1.0F;
        state_.session = 2;
        state_.remainingTime = "00:00";
        state_.status =
            ejected
                ? "Burn complete! Disc ejected. Backend: RetroBeam."
                : "Burn complete! Backend: RetroBeam.";
        return;
    }

    // ------------------------------------------------------------
    // DVD TARGETS: PS2 DVD5/DVD9 + Xbox 360 XGD2
    // XGD3 is handled by the validated path above.
    // ------------------------------------------------------------
    const bool stage36Ps2Dvd =
        request.target ==
        BurnTarget::PlayStation2Dvd;

    const bool stage36Xgd2 =
        request.target ==
            BurnTarget::Xbox360 &&
        request.xbox360DiscType ==
            Xbox360DiscType::Xgd2;

    if (stage36Ps2Dvd ||
        stage36Xgd2) {
        if (extension != ".iso") {
            SetFailure(
                stage36Ps2Dvd
                    ? "PlayStation 2 DVD burning requires an .iso image."
                    : "Xbox 360 XGD2 burning requires an .iso image.");
            return;
        }

        const std::uintmax_t imageBytes =
            fs::file_size(
                imagePath,
                stage36FileError);

        if (stage36FileError ||
            imageBytes == 0 ||
            (imageBytes % 2048ULL) != 0ULL) {
            SetFailure(
                "The selected DVD ISO is empty or is not aligned to 2048-byte sectors.");
            return;
        }

        constexpr std::uintmax_t
            kDvdSingleLayerBytes =
                4707319808ULL;

        constexpr std::uintmax_t
            kDvdDualLayerBytes =
                8547991552ULL;

        if (imageBytes >
            kDvdDualLayerBytes) {
            SetFailure(
                "The selected ISO is larger than nominal DVD9 capacity.");
            return;
        }

        const bool ps2DualLayer =
            stage36Ps2Dvd &&
            imageBytes >
                kDvdSingleLayerBytes;

        const std::string targetDescription =
            stage36Ps2Dvd
                ? "PlayStation 2 DVD"
                : "Xbox 360 XGD2";

        const std::string layout =
            stage36Ps2Dvd
                ? (ps2DualLayer
                    ? "PlayStation 2 DVD ISO - dual-layer DVD (DVD9)"
                    : "PlayStation 2 DVD ISO - single-layer DVD (DVD5)")
                : "Xbox 360 XGD2 ISO - DVD+R DL";

        {
            std::lock_guard lock(
                mutex_);

            state_.layout =
                layout;
            state_.status =
                "Image validated: " +
                layout;
        }

        AppendLog(
            "\nRetro Burner image check\n"
            "Target: " +
            targetDescription +
            "\nImage: " +
            imagePath.string() +
            "\nLayout: " +
            layout +
            "\n");

        if (request.checkOnly) {
            std::lock_guard lock(
                mutex_);

            state_.stage =
                BurnStage::Ready;
            state_.busy = false;
            state_.writing = false;
            state_.progress = 0.0F;
            state_.status =
                layout +
                ". Check complete; no disc was written.";
            return;
        }

        const fs::path mediaTool =
            Stage30SiblingExecutable(
                "dvd+rw-mediainfo");

        if (mediaTool.empty()) {
            SetFailure(
                "dvd+rw-mediainfo was not found. Install dvd+rw-tools.");
            return;
        }

        AppendLog(
            "\nDVD media preflight (dvd+rw-mediainfo)\n");

        // Stage 40: parse the exact streamed dvd+rw-mediainfo text shown in the log.
        //
        // Do not depend solely on CapturedProcessResult::output for media
        // qualification.  Accumulate the same chunks that are displayed so
        // what the user sees and what the validator parses cannot disagree.
        std::string mediaInfoText;

        const CapturedProcessResult mediaInfo =
            RunProcessCapture(
                mediaTool,
                {
                    blockDevice.string(),
                },
                imagePath.parent_path(),
                [this,
                 &mediaInfoText](
                    std::string_view chunk) {
                    mediaInfoText.append(
                        chunk.data(),
                        chunk.size());

                    AppendLog(
                        std::string(chunk));
                });

        if (mediaInfoText.empty())
            mediaInfoText =
                mediaInfo.output;

        if (!mediaInfo.started ||
            mediaInfo.exitCode != 0) {
            SetFailure(
                "dvd+rw-mediainfo could not validate the selected DVD writer/media.");
            return;
        }

        static const std::regex mountedProfilePattern(
            R"(Mounted Media:\s+([0-9A-Fa-f]+)h,\s*([^\r\n]+))",
            std::regex::icase);

        static const std::regex blankPattern(
            R"(Disc status:\s+blank)",
            std::regex::icase);

        static const std::regex freeBlocksPattern(
            R"(Free Blocks:\s+([0-9]+)\*2KB)",
            std::regex::icase);

        std::smatch mediaMatch;
        unsigned profile = 0;
        std::string mountedDescription;

        if (std::regex_search(
                mediaInfoText,
                mediaMatch,
                mountedProfilePattern)) {
            profile =
                static_cast<unsigned>(
                    std::stoul(
                        mediaMatch[1].str(),
                        nullptr,
                        16));

            mountedDescription =
                mediaMatch[2].str();
        }

        // Numeric MMC profile is authoritative.  The textual description is
        // only a fallback for malformed/old dvd+rw-mediainfo output; READ DVD
        // STRUCTURE "Media Book Type" is deliberately never considered.
        if (profile == 0 &&
            !mountedDescription.empty()) {
            std::string normalized;

            normalized.reserve(
                mountedDescription.size());

            for (const unsigned char c :
                 mountedDescription) {
                if (c == ' ' ||
                    c == '/' ||
                    c == '-' ||
                    c == '_' ||
                    c == '\t') {
                    continue;
                }

                normalized.push_back(
                    static_cast<char>(
                        std::toupper(c)));
            }

            // Dual-layer forms must be tested before their SL prefixes.
            if (normalized.find("DVD+RDL") !=
                std::string::npos) {
                profile = 0x2B;
            } else if (
                normalized.find("DVDRDL") !=
                std::string::npos) {
                profile = 0x15;
            } else if (
                normalized.find("DVD+R") !=
                std::string::npos) {
                profile = 0x1B;
            } else if (
                normalized.find("DVDR") !=
                std::string::npos) {
                profile = 0x11;
            }
        }

        AppendLog(
            "\nResolved mounted MMC profile: " +
            std::to_string(profile) +
            (mountedDescription.empty()
                 ? std::string{}
                 : " (" +
                       mountedDescription +
                       ")") +
            "\n");

        if (!std::regex_search(
                mediaInfoText,
                blankPattern)) {
            SetFailure(
                "The selected DVD media is not positively reported as blank.");
            return;
        }

        const bool singleRecordable =
            profile == 0x11 ||
            profile == 0x1B;

        const bool dualRecordable =
            profile == 0x15 ||
            profile == 0x16 ||
            profile == 0x2B;

        if (stage36Xgd2 &&
            profile != 0x2B) {
            SetFailure(
                "Xbox 360 XGD2 requires blank DVD+R DL media.");
            return;
        }

        if (ps2DualLayer &&
            !dualRecordable) {
            SetFailure(
                "This PS2 ISO requires blank dual-layer DVD media.");
            return;
        }

        if (!ps2DualLayer &&
            stage36Ps2Dvd &&
            !singleRecordable &&
            !dualRecordable) {
            SetFailure(
                "PS2 DVD burning requires blank DVD-R, DVD+R, DVD-R DL or DVD+R DL.");
            return;
        }

        if (std::regex_search(
                mediaInfoText,
                mediaMatch,
                freeBlocksPattern)) {
            const std::uintmax_t freeBlocks =
                std::stoull(
                    mediaMatch[1].str());

            if (imageBytes >
                freeBlocks *
                    2048ULL) {
                SetFailure(
                    "The selected ISO does not fit the writable DVD capacity.");
                return;
            }
        }

        std::uint64_t layerBreak = 0;

        if (stage36Xgd2) {
            layerBreak =
                1913760ULL;
        } else if (ps2DualLayer) {
            const std::uint64_t totalSectors =
                static_cast<std::uint64_t>(
                    imageBytes /
                    2048ULL);

            const std::uint64_t minimumLayer0 =
                (totalSectors + 1ULL) /
                2ULL;

            layerBreak =
                (minimumLayer0 + 15ULL) &
                ~std::uint64_t{15ULL};

            if (layerBreak >=
                totalSectors) {
                SetFailure(
                    "Could not calculate a valid PS2 DVD9 layer break.");
                return;
            }

            AppendLog(
                "\nCalculated PS2 DVD9 layer break: " +
                std::to_string(
                    layerBreak) +
                " sectors\n");
        }

        if (!request.useGrowisofsForDvd) {
            if (request.cdrecordDevice.empty()) {
                SetFailure(
                    "RetroBeam has no mapped SCSI address for the selected DVD writer.");
                return;
            }

            AppendLog(
                "\nRetroBeam read-only drive preflight\n");

            const CapturedProcessResult check =
                RunProcessCapture(
                    retrobeam,
                    {
                        "dev=" +
                            request.cdrecordDevice,
                        "-checkdrive",
                    },
                    imagePath.parent_path(),
                    [this](
                        std::string_view chunk) {
                        AppendLog(
                            std::string(chunk));
                    });

            if (!check.started ||
                check.exitCode != 0) {
                SetFailure(
                    "RetroBeam could not validate the selected DVD writer.");
                return;
            }

            if (request.simulate) {
                std::lock_guard lock(
                    mutex_);

                state_.stage =
                    BurnStage::Ready;
                state_.busy = false;
                state_.writing = false;
                state_.progress = 0.0F;
                state_.status =
                    targetDescription +
                    " RetroBeam preflight passed. No disc sectors were written.";
                return;
            }

            {
                std::lock_guard lock(
                    mutex_);

                state_.stage =
                    BurnStage::BurningSession1;
                state_.busy = true;
                state_.writing = true;
                state_.progress = 0.0F;
                state_.session = 1;
                state_.bufferPercent = -1;
                state_.ringBufferPercent = -1;
                state_.driveBufferPercent = -1;
                state_.actualSpeed.clear();
                state_.remainingTime.clear();
                // Stage 42D: deterministic visible RetroBeam phase lifecycle.
                // Set this BEFORE launching RetroBeam so the GUI cannot miss
                // the lead-in/setup phase just because backend output is fast.
                state_.status =
                    "Writing Lead-In...";
            }

            std::vector<std::string> args;

            args.emplace_back(
                "dev=" +
                request.cdrecordDevice);
            args.emplace_back("-v");
            args.emplace_back("-dao");

            if (request.requestedSpeedX > 0) {
                args.emplace_back(
                    "speed=" +
                    std::to_string(
                        request.requestedSpeedX));
            }

            args.emplace_back("fs=32m");

            args.emplace_back(
                Stage36RetroBeamDriverOptions(
                    request,
                    true,
                    layerBreak));

            args.emplace_back("-data");
            args.emplace_back(
                imagePath.string());

            AppendLog(
                "\nDVD WRITE - RetroBeam\n"
                "Write type: DAO\n");

            if (layerBreak != 0) {
                AppendLog(
                    "Layer break: " +
                    std::to_string(
                        layerBreak) +
                    "\n");
            }

            std::string progressWindow;

            const CapturedProcessResult result =
                RunProcessCapture(
                    retrobeam,
                    args,
                    imagePath.parent_path(),
                    [this,
                     &progressWindow,
                     targetDescription](
                        std::string_view chunk) {
                        const std::string text(
                            chunk);

                        AppendLog(text);

                        progressWindow +=
                            text;

                        if (progressWindow.size() >
                            8192U) {
                            progressWindow.erase(
                                0,
                                progressWindow.size() -
                                    8192U);
                        }

                        const RetroBeamProgressUpdate update =
                            ParseRetroBeamProgressText(
                                progressWindow);

                        if (!update.HasTelemetry())
                            return;

                        std::lock_guard lock(
                            mutex_);

                        if (!update.phaseStatus.empty()) {
                            state_.status =
                                update.phaseStatus;
                        }

                        if (update.progress >= 0.0F) {
                            state_.progress =
                                std::max(
                                    state_.progress,
                                    update.progress);

                            if (update.phaseStatus.empty()) {
                                state_.status =
                                    "Writing " +
                                    targetDescription +
                                    " with RetroBeam - " +
                                    std::to_string(
                                        static_cast<int>(
                                            std::lround(
                                                state_.progress *
                                                100.0F))) +
                                    "%";
                            }
                        }

                        if (update.fifoPercent >= 0)
                            state_.ringBufferPercent =
                                update.fifoPercent;

                        if (update.bufferPercent >= 0) {
                            state_.bufferPercent =
                                update.bufferPercent;
                            state_.driveBufferPercent =
                                update.bufferPercent;
                        }

                        if (!update.speed.empty())
                            state_.actualSpeed =
                                update.speed;
                    });

            if (request.advanced.useStreamingPolicy &&
                request.advanced.restoreStreamingDefaults) {
                AppendLog(
                    "\nRestoring RetroBeam MMC streaming defaults...\n");

                const CapturedProcessResult restore =
                    RunProcessCapture(
                        retrobeam,
                        {
                            "dev=" +
                                request.cdrecordDevice,
                            "driveropts=streamrestore",
                            "-setdropts",
                        },
                        imagePath.parent_path(),
                        [this](
                            std::string_view chunk) {
                            AppendLog(
                                std::string(chunk));
                        });

                if (!restore.started ||
                    restore.exitCode != 0) {
                    AppendLog(
                        "WARNING: Could not restore RetroBeam streaming defaults.\n");
                }
            }

            if (!result.started ||
                result.exitCode != 0) {
                SetFailure(
                    targetDescription +
                    " RetroBeam write failed. The disc may be incomplete.");
                return;
            }

            const bool ejected =
                Stage36Eject(
                    blockDevice);

            if (!ejected) {
                AppendLog(
                    "\nWARNING: Burn succeeded, but Linux could not automatically eject the disc.\n");
            }

            std::lock_guard lock(
                mutex_);

            state_.stage =
                BurnStage::Complete;
            state_.busy = false;
            state_.writing = false;
            state_.progress = 1.0F;
            state_.session = 1;
            state_.remainingTime = "00:00";
            state_.status =
                ejected
                    ? "Burn complete! Disc ejected. Backend: RetroBeam."
                    : "Burn complete! Backend: RetroBeam.";
            return;
        }

        // growisofs physical + dry-run path.
        const fs::path growisofs =
            Stage30SiblingExecutable(
                "growisofs");

        if (growisofs.empty()) {
            SetFailure(
                "growisofs was not found.");
            return;
        }

        std::vector<std::string> args;

        if (request.simulate)
            args.emplace_back("-dry-run");

        args.emplace_back(
            "-use-the-force-luke=dao");

        if (layerBreak != 0) {
            args.emplace_back(
                "-use-the-force-luke=break:" +
                std::to_string(
                    layerBreak));
        }

        args.emplace_back("-dvd-compat");

        if (request.requestedSpeedX > 0) {
            args.emplace_back(
                "-speed=" +
                std::to_string(
                    request.requestedSpeedX));
        }

        args.emplace_back("-Z");
        args.emplace_back(
            blockDevice.string() +
            "=" +
            imagePath.string());

        {
            std::lock_guard lock(
                mutex_);

            state_.stage =
                BurnStage::BurningSession1;
            state_.busy = true;
            state_.writing =
                !request.simulate;
            state_.progress = 0.0F;
            state_.session = 1;
            state_.bufferPercent = -1;
            state_.ringBufferPercent = -1;
            state_.driveBufferPercent = -1;
            state_.actualSpeed.clear();
            state_.remainingTime.clear();
            state_.status =
                request.simulate
                    ? targetDescription +
                        " growisofs dry run - no write..."
                    : "Writing " +
                        targetDescription +
                        " with growisofs...";
        }

        AppendLog(
            request.simulate
                ? "\nDVD DRY RUN - growisofs\nWrite type: DAO\n"
                : "\nDVD WRITE - growisofs\nWrite type: DAO\n");

        static const std::regex growProgressPattern(
            R"(\(\s*([0-9]+(?:\.[0-9]+)?)%\)\s+@([0-9]+(?:\.[0-9]+)?)x,\s+remaining\s+([0-9?:]+)\s+RBU\s+([0-9]+(?:\.[0-9]+)?)%\s+UBU\s+([0-9]+(?:\.[0-9]+)?)%)",
            std::regex::icase);

        static const std::regex growSimplePercent(
            R"(([0-9]+(?:\.[0-9]+)?)%\s+done)",
            std::regex::icase);

        // Stage 41B: Windows-equivalent line-framed growisofs telemetry.
        //
        // The old Linux path searched an 8 KiB rolling window with
        // std::regex_search(). That returned the FIRST matching progress line
        // still present in the window, pinning the GUI to old telemetry.
        //
        // Linux process callbacks may arrive as arbitrary chunks, including
        // growisofs CR-rewritten progress records. Frame those chunks into
        // logical CR/LF lines first, then apply the same field mapping as
        // Windows: percent, speed, remaining, RBU and UBU.
        const auto applyGrowisofsLine =
            [this,
             targetDescription](
                std::string_view lineView) {
                const std::string line(
                    lineView);

                if (line.empty())
                    return;

                std::string lowerLine =
                    line;

                std::transform(
                    lowerLine.begin(),
                    lowerLine.end(),
                    lowerLine.begin(),
                    [](const unsigned char c) {
                        return static_cast<char>(
                            std::tolower(c));
                    });

                std::string backendPhase;

                if (lowerLine.find("lead-in") !=
                        std::string::npos ||
                    lowerLine.find("leadin") !=
                        std::string::npos) {
                    backendPhase =
                        "Writing Lead-In...";
                } else if (
                    lowerLine.find("starting new track") !=
                        std::string::npos ||
                    lowerLine.find("writing track") !=
                        std::string::npos) {
                    backendPhase =
                        "Writing Sectors...";
                } else if (
                    lowerLine.find("fixating") !=
                        std::string::npos ||
                    lowerLine.find("closing session") !=
                        std::string::npos ||
                    lowerLine.find("lead-out") !=
                        std::string::npos ||
                    lowerLine.find("leadout") !=
                        std::string::npos) {
                    backendPhase =
                        "Finalising Disc...";
                }

                if (!backendPhase.empty()) {
                    std::lock_guard phaseLock(
                        mutex_);

                    state_.status =
                        std::move(
                            backendPhase);
                }

                std::smatch match;

                if (std::regex_search(
                        line,
                        match,
                        growProgressPattern)) {
                    const float percent =
                        std::stof(
                            match[1].str());

                    const std::string speed =
                        match[2].str() +
                        "x";

                    const std::string remaining =
                        match[3].str();

                    const int rbu =
                        static_cast<int>(
                            std::lround(
                                std::stod(
                                    match[4].str())));

                    const int ubu =
                        static_cast<int>(
                            std::lround(
                                std::stod(
                                    match[5].str())));

                    std::lock_guard lock(
                        mutex_);

                    state_.progress =
                        std::clamp(
                            percent /
                                100.0F,
                            state_.progress,
                            0.999F);

                    state_.actualSpeed =
                        speed;

                    state_.remainingTime =
                        remaining;

                    state_.ringBufferPercent =
                        std::clamp(
                            rbu,
                            0,
                            100);

                    state_.driveBufferPercent =
                        std::clamp(
                            ubu,
                            0,
                            100);

                    state_.status =
                        "Writing " +
                        targetDescription +
                        " - " +
                        std::to_string(
                            static_cast<int>(
                                std::lround(
                                    percent))) +
                        "%";

                    return;
                }

                // Retain the Linux fallback for growisofs variants that emit
                // a simple "<percent>% done" record instead of RBU/UBU.
                if (std::regex_search(
                        line,
                        match,
                        growSimplePercent)) {
                    const float percent =
                        std::stof(
                            match[1].str());

                    std::lock_guard lock(
                        mutex_);

                    state_.progress =
                        std::clamp(
                            percent /
                                100.0F,
                            state_.progress,
                            0.999F);

                    state_.status =
                        "Writing " +
                        targetDescription +
                        " - " +
                        std::to_string(
                            static_cast<int>(
                                std::lround(
                                    percent))) +
                        "%";
                }
            };

        std::string growLineBuffer;

        const CapturedProcessResult result =
            RunProcessCapture(
                growisofs,
                args,
                imagePath.parent_path(),
                [this,
                 &growLineBuffer,
                 &applyGrowisofsLine](
                    std::string_view chunk) {
                    const std::string text(
                        chunk);

                    AppendLog(text);

                    growLineBuffer +=
                        text;

                    for (;;) {
                        const std::size_t lineEnd =
                            growLineBuffer.find_first_of(
                                "\r\n");

                        if (lineEnd ==
                            std::string::npos) {
                            break;
                        }

                        const std::string line =
                            growLineBuffer.substr(
                                0,
                                lineEnd);

                        std::size_t eraseCount =
                            lineEnd +
                            1U;

                        if (growLineBuffer[lineEnd] ==
                                '\r' &&
                            eraseCount <
                                growLineBuffer.size() &&
                            growLineBuffer[eraseCount] ==
                                '\n') {
                            ++eraseCount;
                        }

                        growLineBuffer.erase(
                            0,
                            eraseCount);

                        if (!line.empty()) {
                            applyGrowisofsLine(
                                line);
                        }
                    }

                    if (growLineBuffer.size() >
                        65536U) {
                        growLineBuffer.erase(
                            0,
                            growLineBuffer.size() -
                                16384U);
                    }
                });

        if (!growLineBuffer.empty()) {
            applyGrowisofsLine(
                growLineBuffer);
        }

        if (!result.started ||
            result.exitCode != 0) {
            SetFailure(
                request.simulate
                    ? targetDescription +
                        " growisofs dry run failed. No image data was intentionally written."
                    : targetDescription +
                        " growisofs write failed. The disc may be incomplete.");
            return;
        }

        if (request.simulate) {
            std::lock_guard lock(
                mutex_);

            state_.stage =
                BurnStage::Ready;
            state_.busy = false;
            state_.writing = false;
            state_.progress = 0.0F;
            state_.status =
                targetDescription +
                " growisofs dry run completed successfully. No image data was written.";
            return;
        }

        const bool ejected =
            Stage36Eject(
                blockDevice);

        if (!ejected) {
            AppendLog(
                "\nWARNING: Burn succeeded, but Linux could not automatically eject the disc.\n");
        }

        std::lock_guard lock(
            mutex_);

        state_.stage =
            BurnStage::Complete;
        state_.busy = false;
        state_.writing = false;
        state_.progress = 1.0F;
        state_.session = 1;
        state_.remainingTime = "00:00";
        state_.status =
            ejected
                ? "Burn complete! Disc ejected. Backend: growisofs."
                : "Burn complete! Backend: growisofs.";
        return;
    }

    // ------------------------------------------------------------
    // CD TARGETS: PlayStation / PS2 CD / Saturn
    // ------------------------------------------------------------
    if (request.target !=
            BurnTarget::PlayStation &&
        request.target !=
            BurnTarget::PlayStation2Cd &&
        request.target !=
            BurnTarget::Saturn) {
        SetFailure(
            "Unsupported Retro Burner target.");
        return;
    }

    int cueTracks = 1;
    std::string cueError;

    if (request.target ==
            BurnTarget::PlayStation ||
        request.target ==
            BurnTarget::Saturn) {
        if (extension != ".cue") {
            SetFailure(
                std::string(
                    Stage36TargetName(
                        request.target)) +
                " burning requires the .cue file so track layout and audio are preserved.");
            return;
        }

        if (!Stage36ReadCue(
                imagePath,
                cueTracks,
                cueError)) {
            SetFailure(
                std::move(cueError));
            return;
        }
    } else {
        if (extension == ".cue") {
            if (!Stage36ReadCue(
                    imagePath,
                    cueTracks,
                    cueError)) {
                SetFailure(
                    std::move(cueError));
                return;
            }
        } else if (
            extension == ".iso") {
            const std::uintmax_t imageBytes =
                fs::file_size(
                    imagePath,
                    stage36FileError);

            if (stage36FileError ||
                imageBytes == 0 ||
                (imageBytes %
                 2048ULL) != 0ULL) {
                SetFailure(
                    "The PS2 CD ISO is empty or is not aligned to 2048-byte sectors.");
                return;
            }

            // RB_STAGE44K_PS2CD_VALIDATION_GATE
            const Ps2IsoMediaProbeResult mediaProbe =
                ProbePs2IsoMedia(
                    imagePath);

            if (!mediaProbe.inspected) {
                SetFailure(
                    "Could not inspect the PS2 ISO media type: " +
                    mediaProbe.error);
                return;
            }

            if (mediaProbe.LooksLikeDvdOrigin()) {
                SetFailure(
                    "This ISO contains DVD/UDF filesystem structures (" +
                    mediaProbe.evidence +
                    "). It may fit on a CD-R by size, but it is not a PS2 CD image. "
                    "Select PlayStation 2 - DVD instead.");
                return;
            }
        } else {
            SetFailure(
                "PlayStation 2 CD supports .cue (BIN/CUE) or .iso images.");
            return;
        }
    }

    // RB_STAGE44AC_LINUX_CUE_AUTHORITY
    // Use the parser inside the exact RetroBeam binary that will perform the
    // write. This process exits before any libscg/drive/media access.
    if (extension == ".cue") {
        AppendLog(
            "\nAuthoritative RetroBeam CUE parser preflight "
            "(no optical drive access)\n");

        std::string cueCheckText;
        const CapturedProcessResult cueCheck =
            RunProcessCapture(
                retrobeam,
                {
                    "--rb-cue-check",
                    imagePath.string(),
                },
                imagePath.parent_path(),
                [this,
                 &cueCheckText](
                    std::string_view chunk) {
                    cueCheckText.append(
                        chunk.data(),
                        chunk.size());
                    AppendLog(
                        std::string(chunk));
                });

        const std::string& cueCheckEvidence =
            cueCheckText.empty()
                ? cueCheck.output
                : cueCheckText;

        if (!cueCheck.started ||
            cueCheck.exitCode != 0 ||
            cueCheckEvidence.find(
                "RB_CUECHECK_OK") ==
                std::string::npos) {
            SetFailure(
                "RetroBeam rejected this CUE layout. No disc was touched; "
                "see Burn Log for the parser reason.");
            return;
        }

        AppendLog(
            "RetroBeam CUE parser: ACCEPTED. The same parsed layout will be "
            "used for recording.\n");
    }

    const std::string cdLayout =
        request.target ==
            BurnTarget::PlayStation2Cd
        ? (extension == ".cue"
            ? "PlayStation 2 CD BIN/CUE - " +
                std::to_string(
                    cueTracks) +
                (cueTracks == 1
                    ? " track"
                    : " tracks")
            : "PlayStation 2 CD ISO - no UDF/DVD markers detected")
        : std::string(
            Stage36TargetName(
                request.target)) +
            " BIN/CUE - " +
            std::to_string(
                cueTracks) +
            (cueTracks == 1
                ? " track"
                : " tracks");

    {
        std::lock_guard lock(
            mutex_);

        state_.layout =
            cdLayout;
        state_.status =
            "Image validated: " +
            cdLayout;
    }

    AppendLog(
        "\nRetro Burner image check\n"
        "Target: " +
        std::string(
            Stage36TargetName(
                request.target)) +
        "\nImage: " +
        imagePath.string() +
        "\nLayout: " +
        cdLayout +
        "\n");

    if (request.checkOnly) {
        std::lock_guard lock(
            mutex_);

        state_.stage =
            BurnStage::Ready;
        state_.busy = false;
        state_.writing = false;
        state_.progress = 0.0F;
        state_.status =
            cdLayout +
            ". Check complete; no disc was written.";
        return;
    }

    if (request.cdrecordDevice.empty()) {
        SetFailure(
            "RetroBeam has no mapped SCSI address for the selected CD writer.");
        return;
    }

    AppendLog(
        "\nRead-only CD media preflight\n");

    const CapturedProcessResult preflight =
        RunProcessCapture(
            retrobeam,
            {
                "dev=" +
                    request.cdrecordDevice,
                "-atip",
            },
            imagePath.parent_path(),
            [this](
                std::string_view chunk) {
                AppendLog(
                    std::string(chunk));
            });

    if (!preflight.started ||
        preflight.exitCode != 0) {
        SetFailure(
            "The selected burner or inserted blank CD-R failed preflight.");
        return;
    }

    {
        std::lock_guard lock(
            mutex_);

        state_.stage =
            BurnStage::BurningSession1;
        state_.busy = true;
        state_.writing =
            !request.simulate;
        state_.progress = 0.0F;
        state_.session = 1;
        state_.bufferPercent = -1;
        state_.ringBufferPercent = -1;
        state_.driveBufferPercent = -1;
        state_.actualSpeed.clear();
        state_.remainingTime.clear();
        state_.status =
            request.simulate
                ? "Running RetroBeam dummy CD write..."
                : "Writing Lead-In...";
    }

    std::vector<std::string> args;

    args.emplace_back(
        "dev=" +
        request.cdrecordDevice);

    args.emplace_back("-v");

    if (request.requestedSpeedX > 0) {
        args.emplace_back(
            "speed=" +
            std::to_string(
                request.requestedSpeedX));
    }

    // RB_STAGE44AC_CD_FIFO_8M
    // Keep the CD producer FIFO identical on Windows and Linux.
    args.emplace_back("fs=8m");
    args.emplace_back("gracetime=2");

    args.emplace_back(
        Stage36RetroBeamDriverOptions(
            request,
            false));

    if (request.simulate) {
        // RB_STAGE44AF_LINUX_CD_DUMMY_FEEDBACK
        args.emplace_back("-dummy");

        AppendLog(
            "\n"
            "============================================================\n"
            " DUMMY WRITE MODE - RECORDING LASER OFF\n"
            " Full RetroBeam CD recording pipeline will be exercised.\n"
            " The blank CD-R will NOT be recorded.\n"
            "============================================================\n");
    }

    // RB_STAGE44L_PS2CD_TAO_XA
    //
    // Preserve XA Mode 2 Form 1 while avoiding DAO SEND CUE SHEET for
    // single-track PS2-CD ISO images. CUE/BIN layouts continue to use DAO.
    const bool ps2CdIso =
        request.target ==
            BurnTarget::PlayStation2Cd &&
        extension == ".iso";

    // RB_STAGE44AD_PS1_DAO_CUE
    //
    // Ordinary PS1 BIN/CUE contains no original 96-byte P-W stream.
    // Keep the authoritative RetroBeam CUE layout, but use DAO rather
    // than forcing RetroBeam to manufacture RAW96R subchannel data.
    const bool ps1Cue =
        request.target ==
            BurnTarget::PlayStation &&
        extension == ".cue";

    args.emplace_back(
        ps2CdIso
            ? "-tao"
            : "-dao");

    if (extension == ".cue") {
        args.emplace_back(
            "cuefile=" +
            imagePath.string());
    } else if (
        request.target ==
            BurnTarget::PlayStation2Cd &&
        extension == ".iso") {
        // RB_STAGE44K_PS2CD_XA_MODE
        args.emplace_back("-xa");
        args.emplace_back(
            imagePath.string());
    } else {
        args.emplace_back("-data");
        args.emplace_back(
            imagePath.string());
    }

    if (ps1Cue) {
        AppendLog(
            "\nPS1 recording mode: DAO / CDRWIN CUE\n"
            "RetroBeam will preserve the parsed mixed-mode CUE layout "
            "without forcing a synthetic RAW96R P-W stream.\n");
    }

    if (ps2CdIso) {
        AppendLog(
            "\nPS2 CD ISO recording mode: "
            "TAO / CD-ROM XA Mode 2 Form 1\n");
        AppendLog(
            std::string("Post-burn verification: ") +
            (request.verifyAfterBurn ? "ON" : "OFF") +
            "\n");
    }

    std::string progressWindow;

    const CapturedProcessResult result =
        RunProcessCapture(
            retrobeam,
            args,
            imagePath.parent_path(),
            [this,
             &progressWindow,
             target = request.target](
                std::string_view chunk) {
                const std::string text(
                    chunk);

                AppendLog(text);

                progressWindow += text;

                if (progressWindow.size() >
                    8192U) {
                    progressWindow.erase(
                        0,
                        progressWindow.size() -
                            8192U);
                }

                const RetroBeamProgressUpdate update =
                    ParseRetroBeamProgressText(
                        progressWindow);

                if (!update.HasTelemetry())
                    return;

                std::lock_guard lock(
                    mutex_);

                if (!update.phaseStatus.empty()) {
                    state_.status =
                        update.phaseStatus;
                }

                if (update.progress >= 0.0F) {
                    state_.progress =
                        std::max(
                            state_.progress,
                            update.progress);

                    if (update.phaseStatus.empty()) {
                        state_.status =
                            "Writing " +
                            std::string(
                                Stage36TargetName(
                                    target)) +
                            " - " +
                            std::to_string(
                                static_cast<int>(
                                    std::lround(
                                        state_.progress *
                                        100.0F))) +
                            "%";
                    }
                }

                if (update.fifoPercent >= 0)
                    state_.ringBufferPercent =
                        update.fifoPercent;

                if (update.bufferPercent >= 0) {
                    state_.bufferPercent =
                        update.bufferPercent;
                    state_.driveBufferPercent =
                        update.bufferPercent;
                }

                if (!update.speed.empty())
                    state_.actualSpeed =
                        update.speed;
            });

    // RB_STAGE44L_FAILURE_STATUS
    if (!result.started) {
        SetFailure(
            result.error.empty()
                ? "RetroBeam could not start."
                : "RetroBeam could not start: " +
                      result.error);
        return;
    }

    if (result.exitCode != 0) {
        const std::string detail =
            SummarizeRetroBeamFailure(
                result.output.empty()
                    ? progressWindow
                    : result.output);

        SetFailure(
            request.simulate
                ? (detail.empty()
                    ? "RetroBeam dummy CD write failed."
                    : "RetroBeam dummy CD write failed: " +
                          detail)
                : (detail.empty()
                    ? std::string(
                          Stage36TargetName(
                              request.target)) +
                          " RetroBeam write failed. See Burn Log for full backend output."
                    : "RetroBeam burn failed: " +
                          detail));
        return;
    }

    if (request.simulate) {
        AppendLog(
            "\n"
            "============================================================\n"
            " DUMMY WRITE PASSED\n"
            " All tracks and finalisation completed successfully.\n"
            " Recording laser remained OFF; CD-R was not written.\n"
            "============================================================\n");

        std::lock_guard lock(
            mutex_);

        state_.stage =
            BurnStage::Ready;
        state_.busy = false;
        state_.writing = false;
        state_.progress = 1.0F;
        state_.status =
            "Dummy write passed - full CD pipeline completed; "
            "recording laser remained off and the CD-R was not written.";
        return;
    }

    // Preserve the hardware-proven direct SG verification for single-track
    // PS2-CD ISO burns.
    // RB_STAGE44M_LINUX_VERIFY_GATE
    if (request.verifyAfterBurn &&
        request.target ==
            BurnTarget::PlayStation2Cd &&
        extension == ".iso") {
        {
            std::lock_guard lock(
                mutex_);

            // RB_STAGE44J_PS2CD_VERIFY_PROGRESS
            //
            // The write/fixation has completed, but the job is not done:
            // direct SG_IO readback may take several minutes. Keep the burn
            // lifecycle alive and remove stale write-only telemetry.
            state_.writing = false;
            state_.progress = 0.999F;
            state_.bufferPercent = -1;
            state_.ringBufferPercent = -1;
            state_.driveBufferPercent = -1;
            state_.actualSpeed.clear();
            state_.remainingTime.clear();
            state_.status =
                "Verifying Disc... 0%";
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(
                500));

        const OpticalVerifyResult verification =
            VerifyOpticalSectors(
                request.cdrecordDevice,
                imagePath,
                0,
                [this](
                    const std::uint32_t completedSectors,
                    const std::uint32_t totalSectors) {
                    if (totalSectors == 0)
                        return;

                    const unsigned percent =
                        static_cast<unsigned>(
                            std::min<std::uint64_t>(
                                100ULL,
                                (static_cast<std::uint64_t>(
                                     completedSectors) *
                                 100ULL) /
                                    totalSectors));

                    std::lock_guard lock(
                        mutex_);

                    // Main burn progress intentionally remains at 99.9%
                    // until readback succeeds; verification percent is
                    // reported explicitly in the live PHASE text.
                    state_.progress = 0.999F;
                    state_.status =
                        "Verifying Disc... " +
                        std::to_string(percent) +
                        "%";
                });

        AppendLog(
            "\n===== Direct SG verification =====\n" +
            verification.ToText());

        if (!verification.success) {
            SetFailure(
                "RetroBeam returned success, but direct SG verification "
                "did not match the source PS2 CD ISO.");
            return;
        }
    }

    const bool ejected =
        Stage36Eject(
            blockDevice);

    if (!ejected) {
        AppendLog(
            "\nWARNING: Burn succeeded, but Linux could not automatically eject the disc.\n");
    }

    std::lock_guard lock(
        mutex_);

    state_.stage =
        BurnStage::Complete;
    state_.busy = false;
    state_.writing = false;
    state_.progress = 1.0F;
    state_.session = 1;
    state_.remainingTime = "00:00";
    state_.status =
        ejected
            ? "Burn complete! Disc ejected. Backend: RetroBeam."
            : "Burn complete! Backend: RetroBeam.";

}

void BurnEngine::RunBurnerMaxOnly(
    BurnRequest request,
    const std::wstring&)
{
    if (request.target !=
            BurnTarget::Xbox360 ||
        request.xbox360DiscType !=
            Xbox360DiscType::Xgd3) {
        SetFailure(
            "BurnerMAX testing is available only for Xbox 360 XGD3.");
        return;
    }

    const fs::path blockDevice =
        Stage30Path(
            request.opticalDriveRoot);

    const fs::path dvdMediaInfo =
        Stage30SiblingExecutable(
            "dvd+rw-mediainfo");

    if (dvdMediaInfo.empty()) {
        SetFailure(
            "dvd+rw-mediainfo was not found. Install dvd+rw-tools.");
        return;
    }

    static const std::regex mountedProfilePattern(
        R"(Mounted Media:\s+([0-9A-Fa-f]+)h,)",
        std::regex::icase);
    static const std::regex blankDiscPattern(
        R"(Disc status:\s+blank)",
        std::regex::icase);
    static const std::regex freeBlocksPattern(
        R"(Free Blocks:\s+([0-9]+)\*2KB)",
        std::regex::icase);

    auto mediaInfo =
        [this,
         &dvdMediaInfo,
         &blockDevice](
            const char* heading,
            std::string& output) -> bool {
            AppendLog(
                std::string("\n") +
                heading +
                "\n");

            const CapturedProcessResult result =
                RunProcessCapture(
                    dvdMediaInfo,
                    {blockDevice.string()},
                    {});

            output =
                result.output;

            if (!result.output.empty())
                AppendLog(
                    result.output);

            if (!result.error.empty())
                AppendLog(
                    "\n" +
                    result.error +
                    "\n");

            return
                result.started &&
                result.exitCode == 0;
        };

    std::string output;

    if (!mediaInfo(
            "BurnerMAX DVD+R DL preflight (dvd+rw-mediainfo)",
            output)) {
        SetFailure(
            "dvd+rw-mediainfo could not validate the selected "
            "DVD writer/media. No disc data was written.");
        return;
    }

    std::smatch match;
    unsigned profile = 0;

    if (std::regex_search(
            output,
            match,
            mountedProfilePattern)) {
        profile =
            static_cast<unsigned>(
                std::stoul(
                    match[1].str(),
                    nullptr,
                    16));
    }

    if (profile != 0x2B) {
        SetFailure(
            "BurnerMAX testing requires a blank DVD+R DL in the selected drive.");
        return;
    }

    if (!std::regex_search(
            output,
            blankDiscPattern)) {
        SetFailure(
            "BurnerMAX testing requires media positively reported as blank. "
            "No disc data was written.");
        return;
    }

    {
        std::lock_guard lock(mutex_);
        state_.status =
            "Scanning drive for BurnerMAX support...";
    }

    const BurnerMaxResult burnerMax =
        EnableBurnerMax(
            request.cdrecordDevice,
            [this](
                const std::string& text) {
                AppendLog(text);
            });

    if (!burnerMax.Success()) {
        SetFailure(
            burnerMax.message);
        return;
    }

    std::string refreshed;

    if (!mediaInfo(
            "BurnerMAX capacity verification (dvd+rw-mediainfo)",
            refreshed)) {
        SetFailure(
            "BurnerMAX vendor-command verification passed, but writable "
            "capacity could not be re-read.");
        return;
    }

    constexpr std::uintmax_t
        kBurnerMaxFullCapacitySectors =
            4267040ULL;

    std::uintmax_t freeBlocks = 0;

    if (std::regex_search(
            refreshed,
            match,
            freeBlocksPattern)) {
        freeBlocks =
            std::stoull(
                match[1].str());
    }

    if (freeBlocks <
        kBurnerMaxFullCapacitySectors) {
        SetFailure(
            "BurnerMAX layer-boundary verification passed, but the drive "
            "reports only " +
            std::to_string(
                freeBlocks) +
            " writable sectors. Full XGD3 capacity requires at least "
            "4267040 sectors.");
        return;
    }

    AppendLog(
        "\nBurnerMAX SUCCESS\n"
        "Expanded writable capacity: " +
        std::to_string(
            freeBlocks) +
        " sectors (" +
        std::to_string(
            freeBlocks *
            2048ULL) +
        " bytes)\n"
        "No disc sectors were written.\n");

    std::lock_guard lock(mutex_);

    state_.stage =
        BurnStage::Ready;
    state_.busy = false;
    state_.writing = false;
    state_.progress = 0.0F;
    state_.layout =
        "Xbox 360 XGD3 - BurnerMAX";
    state_.status =
        burnerMax.status ==
                BurnerMaxStatus::AlreadyEnabled
            ? "BurnerMAX already active. Expanded XGD3 capacity verified."
            : "BurnerMAX enabled via " +
                  burnerMax.backend +
                  ". Expanded XGD3 capacity verified.";
}

void BurnEngine::SetFailure(std::string message)
{
    AppendLog("\nERROR: " + message + "\n");
    std::lock_guard lock(mutex_);
    state_.stage = BurnStage::Failed;
    state_.busy = false;
    state_.writing = false;
    state_.status = std::move(message);
}

void BurnEngine::AppendLog(const std::string& text)
{
    if (text.empty())
        return;

    std::lock_guard lock(mutex_);

    const auto eraseCurrentLine = [this]() {
        const std::size_t newline =
            state_.log.find_last_of('\n');
        if (newline == std::string::npos)
            state_.log.clear();
        else
            state_.log.erase(newline + 1);
    };

    std::size_t index = 0;

    if (logPendingCarriageReturn_) {
        if (!text.empty() && text.front() == '\n') {
            state_.log.push_back('\n');
            index = 1;
        } else {
            eraseCurrentLine();
        }
        logPendingCarriageReturn_ = false;
    }

    while (index < text.size()) {
        const char c = text[index];

        if (c != '\r') {
            state_.log.push_back(c);
            ++index;
            continue;
        }

        if (index + 1 >= text.size()) {
            logPendingCarriageReturn_ = true;
            break;
        }

        if (text[index + 1] == '\n') {
            state_.log.push_back('\n');
            index += 2;
            continue;
        }

        eraseCurrentLine();
        ++index;
    }

    if (state_.log.size() > kMaximumLogBytes) {
        state_.log.erase(
            0,
            state_.log.size() - kMaximumLogBytes);
    }
}

void BurnEngine::ClearPreparedXgd3()
{
    std::wstring directory;

    {
        std::lock_guard lock(mutex_);

        if (!preparedXgd3_.valid &&
            preparedXgd3_.workingDirectory.empty()) {
            return;
        }

        directory =
            std::move(
                preparedXgd3_.workingDirectory);

        preparedXgd3_ =
            PreparedXgd3Cache{};
    }

    if (!directory.empty()) {
        std::error_code ignored;

        fs::remove_all(
            Stage30Path(directory),
            ignored);
    }
}

bool BurnEngine::TryReusePreparedXgd3(
    const std::wstring& sourcePath,
    std::wstring& workingImagePath)
{
    const fs::path source =
        Stage30Path(sourcePath);

    std::uint64_t bytes = 0;
    std::uint64_t writeTime = 0;

    if (!Stage30FileIdentity(
            source,
            bytes,
            writeTime)) {
        ClearPreparedXgd3();
        return false;
    }

    std::wstring staleDirectory;

    {
        std::lock_guard lock(mutex_);

        const bool matches =
            preparedXgd3_.valid &&
            preparedXgd3_.sourcePath ==
                sourcePath &&
            preparedXgd3_.sourceBytes ==
                bytes &&
            preparedXgd3_.sourceWriteTime ==
                writeTime &&
            !preparedXgd3_.workingImagePath.empty() &&
            fs::is_regular_file(
                Stage30Path(
                    preparedXgd3_
                        .workingImagePath));

        if (matches) {
            workingImagePath =
                preparedXgd3_
                    .workingImagePath;
            return true;
        }

        staleDirectory =
            std::move(
                preparedXgd3_
                    .workingDirectory);

        preparedXgd3_ =
            PreparedXgd3Cache{};
    }

    if (!staleDirectory.empty()) {
        std::error_code ignored;

        fs::remove_all(
            Stage30Path(
                staleDirectory),
            ignored);
    }

    return false;
}

void BurnEngine::StorePreparedXgd3(
    const std::wstring& sourcePath,
    const std::wstring& workingDirectory,
    const std::wstring& workingImagePath)
{
    std::uint64_t bytes = 0;
    std::uint64_t writeTime = 0;

    if (!Stage30FileIdentity(
            Stage30Path(
                sourcePath),
            bytes,
            writeTime)) {
        return;
    }

    std::wstring staleDirectory;

    {
        std::lock_guard lock(mutex_);

        staleDirectory =
            std::move(
                preparedXgd3_
                    .workingDirectory);

        preparedXgd3_.valid =
            true;
        preparedXgd3_.sourcePath =
            sourcePath;
        preparedXgd3_.workingDirectory =
            workingDirectory;
        preparedXgd3_.workingImagePath =
            workingImagePath;
        preparedXgd3_.sourceBytes =
            bytes;
        preparedXgd3_.sourceWriteTime =
            writeTime;
    }

    if (!staleDirectory.empty() &&
        staleDirectory !=
            workingDirectory) {
        std::error_code ignored;

        fs::remove_all(
            Stage30Path(
                staleDirectory),
            ignored);
    }
}
