#include "burn_engine.h"
#include "retrobeam_failure.h"
#include "ps2_media_probe.h"
#include "burnermax.h"
#include "embedded_tools.h"
#include "resource.h"

#include <windows.h>
#include <mmsystem.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMinimumFreeSpaceMargin = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumLogBytes = 2U * 1024U * 1024U;

struct UniqueHandle final {
    HANDLE value = INVALID_HANDLE_VALUE;

    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : value(handle) {}
    ~UniqueHandle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) {
            CloseHandle(value);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value(other.value) {
        other.value = INVALID_HANDLE_VALUE;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE && value != nullptr) {
                CloseHandle(value);
            }
            value = other.value;
            other.value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    [[nodiscard]] bool Valid() const noexcept {
        return value != INVALID_HANDLE_VALUE && value != nullptr;
    }
    HANDLE Release() noexcept {
        const HANDLE result = value;
        value = INVALID_HANDLE_VALUE;
        return result;
    }
};

struct TemporaryDirectory final {
    fs::path path;

    TemporaryDirectory() = default;
    explicit TemporaryDirectory(fs::path value)
        : path(std::move(value)) {}

    ~TemporaryDirectory() {
        if (!path.empty()) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    TemporaryDirectory(TemporaryDirectory&& other) noexcept
        : path(std::move(other.path)) {
        other.path.clear();
    }

    TemporaryDirectory& operator=(TemporaryDirectory&& other) noexcept {
        if (this != &other) {
            if (!path.empty()) {
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
            path = std::move(other.path);
            other.path.clear();
        }
        return *this;
    }

    [[nodiscard]] fs::path Release() noexcept {
        fs::path result = std::move(path);
        path.clear();
        return result;
    }
};

struct ExtractedLayout final {
    bool audioData = false;
    std::vector<fs::path> firstSession;
    std::vector<fs::path> secondSession;
    std::uintmax_t firstSessionBytes = 0;
    std::uintmax_t secondSessionBytes = 0;
    std::string description;
};

[[nodiscard]] std::wstring QuoteArgument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    if (argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'\"');
    return result;
}

[[nodiscard]] std::wstring BuildCommandLine(
    const fs::path& executable,
    const std::vector<std::wstring>& arguments) {
    std::wstring command = QuoteArgument(executable.wstring());
    for (const std::wstring& argument : arguments) {
        command.push_back(L' ');
        command += QuoteArgument(argument);
    }
    return command;
}

[[nodiscard]] std::string Win32Error(const DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);

    std::string result;
    if (length > 0 && buffer != nullptr) {
        const int bytes = WideCharToMultiByte(
            CP_UTF8, 0, buffer, static_cast<int>(length),
            nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            result.resize(static_cast<std::size_t>(bytes));
            WideCharToMultiByte(
                CP_UTF8, 0, buffer, static_cast<int>(length),
                result.data(), bytes, nullptr, nullptr);
            while (!result.empty() &&
                   (result.back() == '\r' || result.back() == '\n' || result.back() == ' ')) {
                result.pop_back();
            }
        }
        LocalFree(buffer);
    }
    if (result.empty()) {
        result = "Windows error " + std::to_string(error);
    }
    return result;
}

[[nodiscard]] bool QueryFileIdentity(
    const std::wstring& path,
    std::uint64_t& sizeBytes,
    std::uint64_t& writeTime) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(
            path.c_str(),
            GetFileExInfoStandard,
            &data)) {
        return false;
    }

    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;

    ULARGE_INTEGER time{};
    time.HighPart = data.ftLastWriteTime.dwHighDateTime;
    time.LowPart = data.ftLastWriteTime.dwLowDateTime;

    sizeBytes = size.QuadPart;
    writeTime = time.QuadPart;
    return sizeBytes > 0;
}

[[nodiscard]] bool SameWindowsPath(
    const std::wstring& left,
    const std::wstring& right) {
    return CompareStringOrdinal(
               left.c_str(),
               static_cast<int>(left.size()),
               right.c_str(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] fs::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return fs::path(buffer).parent_path();
}

[[nodiscard]] TemporaryDirectory MakeTemporaryDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    const fs::path root(buffer);
    const DWORD processId = GetCurrentProcessId();
    const ULONGLONG tick = GetTickCount64();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        fs::path candidate = root /
            (L"RetroBurner-" + std::to_wstring(processId) + L"-" +
             std::to_wstring(tick) + L"-" + std::to_wstring(attempt));
        std::error_code error;
        if (fs::create_directory(candidate, error)) {
            return TemporaryDirectory{std::move(candidate)};
        }
    }
    return {};
}

[[nodiscard]] TemporaryDirectory MakeTemporaryDirectoryNear(
    const fs::path& parent,
    const std::wstring_view prefix) {
    if (parent.empty()) {
        return {};
    }

    const DWORD processId = GetCurrentProcessId();
    const ULONGLONG tick = GetTickCount64();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        fs::path candidate = parent /
            (std::wstring(prefix) + L"-" +
             std::to_wstring(processId) + L"-" +
             std::to_wstring(tick) + L"-" +
             std::to_wstring(attempt));
        std::error_code error;
        if (fs::create_directory(candidate, error)) {
            return TemporaryDirectory{std::move(candidate)};
        }
    }
    return {};
}

using OutputCallback = std::function<void(std::string_view)>;

struct ProcessResult final {
    bool started = false;
    DWORD exitCode = ERROR_GEN_FAILURE;
    std::string error;
};

[[nodiscard]] ProcessResult RunHiddenProcess(
    const fs::path& executable,
    const std::vector<std::wstring>& arguments,
    const fs::path& workingDirectory,
    const std::function<void(const std::string&)>& appendLog,
    const OutputCallback& onLine) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readRaw = INVALID_HANDLE_VALUE;
    HANDLE writeRaw = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&readRaw, &writeRaw, &security, 0)) {
        return {false, ERROR_GEN_FAILURE, "Could not create the process output pipe: " +
            Win32Error(GetLastError())};
    }
    UniqueHandle readPipe(readRaw);
    UniqueHandle writePipe(writeRaw);
    SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0);

    UniqueHandle nullInput(CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!nullInput.Valid()) {
        return {false, ERROR_GEN_FAILURE, "Could not open the child-process input: " +
            Win32Error(GetLastError())};
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe.value;
    startup.hStdError = writePipe.value;
    startup.hStdInput = nullInput.value;

    PROCESS_INFORMATION process{};
    std::wstring commandLine = BuildCommandLine(executable, arguments);
    const BOOL created = CreateProcessW(
        executable.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startup,
        &process);
    if (!created) {
        return {false, ERROR_GEN_FAILURE, "Could not start " +
            executable.filename().string() + ": " + Win32Error(GetLastError())};
    }

    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);
    writePipe = UniqueHandle();

    std::array<char, 4096> chunk{};
    std::string pendingLine;
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(
                readPipe.value,
                chunk.data(),
                static_cast<DWORD>(chunk.size()),
                &bytesRead,
                nullptr) ||
            bytesRead == 0) {
            break;
        }

        const std::string text(chunk.data(), bytesRead);
        appendLog(text);
        pendingLine += text;
        for (;;) {
            const std::size_t separator = pendingLine.find_first_of("\r\n");
            if (separator == std::string::npos) {
                break;
            }
            if (separator > 0) {
                onLine(std::string_view(pendingLine.data(), separator));
            }
            std::size_t eraseCount = separator + 1U;
            while (eraseCount < pendingLine.size() &&
                   (pendingLine[eraseCount] == '\r' || pendingLine[eraseCount] == '\n')) {
                ++eraseCount;
            }
            pendingLine.erase(0, eraseCount);
        }
    }
    if (!pendingLine.empty()) {
        onLine(pendingLine);
    }

    WaitForSingleObject(processHandle.value, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(processHandle.value, &exitCode);
    return {true, exitCode, {}};
}

struct CopyProgressContext final {
    BurnEngine* engine = nullptr;
};

DWORD CALLBACK CopyProgressRoutine(
    LARGE_INTEGER totalFileSize,
    LARGE_INTEGER totalBytesTransferred,
    LARGE_INTEGER,
    LARGE_INTEGER,
    DWORD,
    DWORD,
    HANDLE,
    HANDLE,
    LPVOID context) {
    auto* progressContext =
        static_cast<CopyProgressContext*>(context);
    if (progressContext == nullptr ||
        progressContext->engine == nullptr ||
        totalFileSize.QuadPart <= 0) {
        return PROGRESS_CONTINUE;
    }

    const double fraction =
        static_cast<double>(totalBytesTransferred.QuadPart) /
        static_cast<double>(totalFileSize.QuadPart);

    progressContext->engine->SetPreparationProgress(
        static_cast<float>(std::clamp(fraction, 0.0, 1.0)),
        "Preparing XGD3 working copy...");
    return PROGRESS_CONTINUE;
}

[[nodiscard]] bool TryParsePercent(
    const std::string_view line,
    const std::regex& pattern,
    int& percent) {
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_search(
            line.begin(),
            line.end(),
            match,
            pattern)) {
        return false;
    }

    try {
        percent = std::stoi(match[1].str());
        percent = std::clamp(percent, 0, 100);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::string Lowercase(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

[[nodiscard]] bool ContainsCaseInsensitive(
    const std::string& haystack,
    const std::string_view needle) {
    const std::string loweredHaystack = Lowercase(haystack);
    const std::string loweredNeedle = Lowercase(std::string(needle));
    return loweredHaystack.find(loweredNeedle) != std::string::npos;
}

[[nodiscard]] std::uintmax_t SumFileSizes(const std::vector<fs::path>& files) {
    std::uintmax_t total = 0;
    for (const fs::path& path : files) {
        std::error_code error;
        const std::uintmax_t size = fs::file_size(path, error);
        if (!error) {
            total += size;
        }
    }
    return total;
}

[[nodiscard]] bool DetectLayout(
    const fs::path& directory,
    ExtractedLayout& layout,
    std::string& errorMessage) {
    std::vector<fs::path> audio;
    std::vector<fs::path> data;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        if (error || !entry.is_regular_file()) {
            continue;
        }
        const std::string extension = Lowercase(entry.path().extension().string());
        if (extension == ".wav") {
            audio.push_back(entry.path());
        } else if (extension == ".iso") {
            data.push_back(entry.path());
        }
    }
    if (error) {
        errorMessage = "Could not inspect the extracted CDI tracks: " + error.message();
        return false;
    }
    std::sort(audio.begin(), audio.end());
    std::sort(data.begin(), data.end());

    if (!audio.empty() && !data.empty()) {
        layout.audioData = true;
        layout.firstSession = std::move(audio);
        layout.secondSession = std::move(data);
        layout.description = "Audio+Data self-boot CDI";
    } else if (audio.empty() && data.size() >= 2) {
        layout.audioData = false;
        layout.firstSession.push_back(data[0]);
        layout.secondSession.assign(data.begin() + 1, data.end());
        layout.description = "Data+Data self-boot CDI";
    } else {
        errorMessage =
            "CDIrip did not produce a supported two-session Dreamcast layout.";
        return false;
    }

    layout.firstSessionBytes = SumFileSizes(layout.firstSession);
    layout.secondSessionBytes = SumFileSizes(layout.secondSession);
    if (layout.firstSessionBytes == 0 || layout.secondSessionBytes == 0) {
        errorMessage = "One of the extracted CDI sessions is empty.";
        return false;
    }
    return true;
}

[[nodiscard]] const char* BurnTargetName(const BurnTarget target) {
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

[[nodiscard]] bool TargetUsesCue(const BurnTarget target) {
    return target == BurnTarget::PlayStation ||
           target == BurnTarget::Saturn;
}


[[nodiscard]] std::wstring JoinRetroBeamDriverOptions(
    const BurnRequest& request,
    const std::uint64_t layerBreak = 0) {
    std::vector<std::wstring> options;
    options.emplace_back(request.advanced.burnFree ? L"burnfree" : L"noburnfree");

    const bool dvdTarget =
        request.target == BurnTarget::PlayStation2Dvd ||
        request.target == BurnTarget::Xbox360;
    if (dvdTarget) {
        switch (request.advanced.opcPolicy) {
        case RetroBeamOpcPolicy::Force:
            options.emplace_back(L"opc=force");
            break;
        case RetroBeamOpcPolicy::Skip:
            options.emplace_back(L"opc=skip");
            break;
        case RetroBeamOpcPolicy::Automatic:
        default:
            options.emplace_back(L"opc=auto");
            break;
        }
    }

    if (request.advanced.forceSpeed) {
        options.emplace_back(L"forcespeed");
    }

    if (request.advanced.useStreamingPolicy) {
        options.emplace_back(
            request.advanced.streamRotation == RetroBeamStreamRotation::Cav
                ? L"streamwrc=cav"
                : L"streamwrc=default");
        options.emplace_back(
            request.advanced.streamExact
                ? L"streamexact"
                : L"nostreamexact");
    }

    if (layerBreak != 0) {
        options.emplace_back(
            L"layerbreak=" + std::to_wstring(layerBreak));
    }

    std::wstring joined = L"driveropts=";
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (i != 0) {
            joined.push_back(L',');
        }
        joined += options[i];
    }
    return joined;
}

[[nodiscard]] const char* OpcPolicyName(const RetroBeamOpcPolicy policy) {
    switch (policy) {
    case RetroBeamOpcPolicy::Force: return "force";
    case RetroBeamOpcPolicy::Skip: return "skip";
    case RetroBeamOpcPolicy::Automatic:
    default: return "auto";
    }
}

[[nodiscard]] std::string RetroBeamAdvancedPolicyText(
    const BurnRequest& request) {
    std::string text =
        "RetroBeam advanced policy: BURN-Free=" +
        std::string(request.advanced.burnFree ? "enabled" : "disabled") +
        ", force-speed=" +
        std::string(request.advanced.forceSpeed ? "enabled" : "disabled");

    if (request.target == BurnTarget::PlayStation2Dvd ||
        request.target == BurnTarget::Xbox360) {
        text += ", OPC=";
        text += OpcPolicyName(request.advanced.opcPolicy);
    }

    if (request.target == BurnTarget::PlayStation2Dvd ||
        request.target == BurnTarget::Xbox360) {
        if (request.advanced.useStreamingPolicy) {
            text += ", SET STREAMING=";
            text += request.advanced.streamRotation == RetroBeamStreamRotation::Cav
                ? "CAV"
                : "firmware/default rotation";
            text += request.advanced.streamExact ? " exact" : " closest-supported";
            text += request.advanced.restoreStreamingDefaults
                ? " (restore defaults after burn)"
                : " (leave runtime streaming state)";
        } else {
            text += ", SET STREAMING=automatic";
        }
    }

    text += "\r\n";
    return text;
}

[[nodiscard]] bool ReadCueInformation(
    const fs::path& cuePath,
    int& trackCount,
    std::string& errorMessage) {
    std::ifstream stream(cuePath, std::ios::binary);
    if (!stream) {
        errorMessage = "Could not open the selected CUE sheet.";
        return false;
    }

    static const std::regex trackPattern(
        "^\\s*TRACK\\s+[0-9]+\\s+\\S+",
        std::regex::icase);
    static const std::regex quotedFilePattern(
        "^\\s*FILE\\s+\"([^\"]+)\"\\s+\\S+",
        std::regex::icase);
    static const std::regex plainFilePattern(
        "^\\s*FILE\\s+([^\\s]+)\\s+\\S+",
        std::regex::icase);

    trackCount = 0;
    int fileCount = 0;
    std::string line;

    while (std::getline(stream, line)) {
        if (std::regex_search(line, trackPattern)) {
            ++trackCount;
        }

        std::smatch match;
        std::string referenced;
        if (std::regex_search(line, match, quotedFilePattern)) {
            referenced = match[1].str();
        } else if (std::regex_search(line, match, plainFilePattern)) {
            referenced = match[1].str();
        }

        if (!referenced.empty()) {
            ++fileCount;
            const fs::path referencedPath =
                cuePath.parent_path() / fs::path(referenced);
            std::error_code existsError;
            if (!fs::is_regular_file(referencedPath, existsError)) {
                errorMessage =
                    "CUE references a file that is missing: " +
                    referenced;
                return false;
            }
        }
    }

    if (fileCount == 0) {
        errorMessage = "The CUE sheet contains no FILE entry.";
        return false;
    }
    if (trackCount == 0) {
        errorMessage = "The CUE sheet contains no TRACK entries.";
        return false;
    }

    return true;
}

[[nodiscard]] bool ValidateStandardImage(
    const BurnRequest& request,
    std::string& layout,
    int& cueTrackCount,
    std::string& errorMessage) {
    const fs::path imagePath(request.cdiPath);
    std::error_code fileError;

    if (!fs::is_regular_file(imagePath, fileError)) {
        errorMessage = "Windows cannot find the selected disc image.";
        return false;
    }

    const std::string extension =
        Lowercase(imagePath.extension().string());

    if (request.target == BurnTarget::PlayStation ||
        request.target == BurnTarget::Saturn) {
        if (extension != ".cue") {
            errorMessage =
                std::string(BurnTargetName(request.target)) +
                " burning requires the .cue file so track layout and audio are preserved.";
            return false;
        }

        if (!ReadCueInformation(
                imagePath,
                cueTrackCount,
                errorMessage)) {
            return false;
        }

        layout =
            std::string(BurnTargetName(request.target)) +
            " BIN/CUE - " +
            std::to_string(cueTrackCount) +
            (cueTrackCount == 1 ? " track" : " tracks");
        return true;
    }

    if (request.target == BurnTarget::PlayStation2Cd) {
        if (extension == ".cue") {
            if (!ReadCueInformation(
                    imagePath,
                    cueTrackCount,
                    errorMessage)) {
                return false;
            }

            layout =
                "PlayStation 2 CD BIN/CUE - " +
                std::to_string(cueTrackCount) +
                (cueTrackCount == 1 ? " track" : " tracks");
            return true;
        }

        if (extension != ".iso") {
            errorMessage =
                "PlayStation 2 CD supports .cue (BIN/CUE) or .iso images.";
            return false;
        }

        const std::uintmax_t size = fs::file_size(imagePath, fileError);
        if (fileError || size == 0 || (size % 2048ULL) != 0ULL) {
            errorMessage =
                "The PS2 CD ISO is empty or is not aligned to 2048-byte sectors.";
            return false;
        }

        // RB_STAGE44K_PS2CD_VALIDATION_GATE
        const Ps2IsoMediaProbeResult mediaProbe =
            ProbePs2IsoMedia(imagePath);

        if (!mediaProbe.inspected) {
            errorMessage =
                "Could not inspect the PS2 ISO media type: " +
                mediaProbe.error;
            return false;
        }

        if (mediaProbe.LooksLikeDvdOrigin()) {
            errorMessage =
                "This ISO contains DVD/UDF filesystem structures (" +
                mediaProbe.evidence +
                "). It may fit on a CD-R by size, but it is not a PS2 CD image. "
                "Select PlayStation 2 - DVD instead.";
            return false;
        }

        cueTrackCount = 1;
        layout =
            "PlayStation 2 CD ISO - no UDF/DVD markers detected";
        return true;
    }

    if (request.target == BurnTarget::PlayStation2Dvd) {
        if (extension != ".iso") {
            errorMessage = "PlayStation 2 DVD supports .iso images.";
            return false;
        }

        const std::uintmax_t size = fs::file_size(imagePath, fileError);
        if (fileError || size == 0) {
            errorMessage = "Could not read the PS2 DVD ISO size.";
            return false;
        }
        if ((size % 2048ULL) != 0ULL) {
            errorMessage =
                "The PS2 DVD ISO is not aligned to 2048-byte DVD sectors.";
            return false;
        }

        constexpr std::uintmax_t kDvdRSingleLayerBytes = 4707319808ULL;
        constexpr std::uintmax_t kDvdPlusRDualLayerNominalBytes = 8547991552ULL;
        if (size > kDvdPlusRDualLayerNominalBytes) {
            errorMessage =
                "This PS2 ISO is larger than nominal dual-layer DVD capacity.";
            return false;
        }

        cueTrackCount = 1;
        layout = size > kDvdRSingleLayerBytes
            ? "PlayStation 2 DVD ISO - dual-layer DVD (DVD9)"
            : "PlayStation 2 DVD ISO - single-layer DVD (DVD5)";
        return true;
    }

    if (request.target == BurnTarget::Xbox360) {
        if (extension != ".iso") {
            errorMessage = "Xbox 360 burning currently supports .iso images.";
            return false;
        }

        const std::uintmax_t size = fs::file_size(imagePath, fileError);
        if (fileError || size == 0 || (size % 2048ULL) != 0ULL) {
            errorMessage =
                "The Xbox 360 ISO is empty or is not aligned to 2048-byte sectors.";
            return false;
        }

        constexpr std::uintmax_t kDvdPlusRDualLayerNominalBytes = 8547991552ULL;
        constexpr std::uintmax_t kXgd3FullImageBytes = 8738846720ULL;
        const std::uintmax_t maximum =
            request.xbox360DiscType == Xbox360DiscType::Xgd3
                ? kXgd3FullImageBytes
                : kDvdPlusRDualLayerNominalBytes;
        if (size > maximum) {
            errorMessage =
                request.xbox360DiscType == Xbox360DiscType::Xgd3
                    ? "This XGD3 ISO is larger than the supported full XGD3 image size."
                    : "This XGD2 ISO is larger than nominal DVD+R DL capacity.";
            return false;
        }

        cueTrackCount = 1;
        layout =
            request.xbox360DiscType == Xbox360DiscType::Xgd3
                ? "Xbox 360 XGD3 ISO - DVD+R DL / BurnerMAX"
                : "Xbox 360 XGD2 ISO - DVD+R DL";
        return true;
    }

    errorMessage = "Unsupported Retro Burner target.";
    return false;
}
[[nodiscard]] std::vector<std::uintmax_t> FileSizes(
    const std::vector<fs::path>& files) {
    std::vector<std::uintmax_t> sizes;
    sizes.reserve(files.size());
    for (const fs::path& path : files) {
        std::error_code error;
        sizes.push_back(fs::file_size(path, error));
        if (error) {
            sizes.back() = 0;
        }
    }
    return sizes;
}

[[nodiscard]] double UnitMultiplier(const std::string& unit) {
    if (unit == "KB" || unit == "kB") {
        return 1024.0;
    }
    if (unit == "MB") {
        return 1024.0 * 1024.0;
    }
    if (unit == "GB") {
        return 1024.0 * 1024.0 * 1024.0;
    }
    return 1.0;
}

// RB_STAGE44M_WINDOWS_OPTICAL_VERIFY
struct WindowsOpticalVerifyResult final {
    bool success = false;
    std::uint32_t sectorCount = 0;
    std::uint64_t bytesCompared = 0;
    std::string error;
};

using WindowsOpticalVerifyProgress =
    std::function<void(
        std::uint32_t completedSectors,
        std::uint32_t totalSectors)>;

[[nodiscard]] WindowsOpticalVerifyResult
VerifyWindowsOpticalSectors(
    const std::wstring& opticalDriveRoot,
    const fs::path& sourcePath,
    WindowsOpticalVerifyProgress progressCallback)
{
    constexpr std::uint64_t kSectorBytes = 2048ULL;
    constexpr std::uint32_t kChunkSectors = 32U;

    WindowsOpticalVerifyResult result;

    std::error_code sizeError;
    const std::uintmax_t sourceBytes =
        fs::file_size(
            sourcePath,
            sizeError);

    if (sizeError ||
        sourceBytes == 0 ||
        (sourceBytes % kSectorBytes) != 0ULL) {
        result.error =
            "Source ISO is empty or not aligned to 2048-byte sectors.";
        return result;
    }

    const std::uint64_t totalSectors64 =
        sourceBytes /
        kSectorBytes;

    if (totalSectors64 > 0xFFFFFFFFULL) {
        result.error =
            "Source ISO has too many sectors for this verifier.";
        return result;
    }

    result.sectorCount =
        static_cast<std::uint32_t>(
            totalSectors64);

    if (opticalDriveRoot.size() < 2 ||
        opticalDriveRoot[1] != L':') {
        result.error =
            "Windows optical verification requires a drive-letter device.";
        return result;
    }

    const std::wstring rawDevice =
        L"\\\\.\\" +
        opticalDriveRoot.substr(0, 2);

    UniqueHandle device(
        CreateFileW(
            rawDevice.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));

    if (!device.Valid()) {
        result.error =
            "Could not open the burned disc for readback: " +
            Win32Error(GetLastError());
        return result;
    }

    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(
            device.value,
            beginning,
            nullptr,
            FILE_BEGIN)) {
        result.error =
            "Could not seek the optical device to LBA 0: " +
            Win32Error(GetLastError());
        return result;
    }

    std::ifstream source(
        sourcePath,
        std::ios::binary);

    if (!source) {
        result.error =
            "Could not reopen the source ISO for verification.";
        return result;
    }

    std::vector<unsigned char> sourceBuffer(
        static_cast<std::size_t>(
            kChunkSectors *
            kSectorBytes));

    std::vector<unsigned char> discBuffer(
        sourceBuffer.size());

    if (progressCallback)
        progressCallback(0, result.sectorCount);

    std::uint32_t completedSectors = 0;

    while (completedSectors < result.sectorCount) {
        const std::uint32_t sectorsThisChunk =
            std::min<std::uint32_t>(
                kChunkSectors,
                result.sectorCount -
                    completedSectors);

        const DWORD bytesThisChunk =
            static_cast<DWORD>(
                sectorsThisChunk *
                kSectorBytes);

        source.read(
            reinterpret_cast<char*>(
                sourceBuffer.data()),
            static_cast<std::streamsize>(
                bytesThisChunk));

        if (source.gcount() !=
            static_cast<std::streamsize>(
                bytesThisChunk)) {
            result.error =
                "Source ISO became unreadable during verification.";
            return result;
        }

        DWORD discBytesRead = 0;

        while (discBytesRead < bytesThisChunk) {
            DWORD readNow = 0;

            if (!ReadFile(
                    device.value,
                    discBuffer.data() +
                        discBytesRead,
                    bytesThisChunk -
                        discBytesRead,
                    &readNow,
                    nullptr)) {
                result.error =
                    "Optical readback failed at LBA " +
                    std::to_string(
                        completedSectors +
                        (discBytesRead /
                         static_cast<DWORD>(
                             kSectorBytes))) +
                    ": " +
                    Win32Error(GetLastError());
                return result;
            }

            if (readNow == 0) {
                result.error =
                    "Optical readback returned end-of-device before "
                    "the source ISO was fully compared.";
                return result;
            }

            discBytesRead += readNow;
        }

        if (!std::equal(
                sourceBuffer.begin(),
                sourceBuffer.begin() +
                    bytesThisChunk,
                discBuffer.begin())) {
            std::size_t mismatch = 0;

            while (mismatch < bytesThisChunk &&
                   sourceBuffer[mismatch] ==
                       discBuffer[mismatch]) {
                ++mismatch;
            }

            const std::uint64_t mismatchByte =
                result.bytesCompared +
                mismatch;

            result.error =
                "Disc readback mismatch at byte " +
                std::to_string(mismatchByte) +
                " (LBA " +
                std::to_string(
                    mismatchByte /
                    kSectorBytes) +
                ").";
            return result;
        }

        completedSectors += sectorsThisChunk;
        result.bytesCompared += bytesThisChunk;

        if (progressCallback) {
            progressCallback(
                completedSectors,
                result.sectorCount);
        }
    }

    result.success = true;
    return result;
}

[[nodiscard]] bool EjectOpticalDrive(const std::wstring& rootPath) {
    if (rootPath.size() < 2 || rootPath[1] != L':') {
        return false;
    }

    const std::wstring devicePath =
        L"\\\\.\\" + rootPath.substr(0, 2);

    UniqueHandle device(CreateFileW(
        devicePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr));
    if (!device.Valid()) {
        return false;
    }

    DWORD returned = 0;
    return DeviceIoControl(
               device.value,
               IOCTL_STORAGE_EJECT_MEDIA,
               nullptr,
               0,
               nullptr,
               0,
               &returned,
               nullptr) != FALSE;
}

void PlayBurnCompleteSound() {
    PlaySoundW(
        MAKEINTRESOURCEW(IDR_WAV_BURN_COMPLETE),
        GetModuleHandleW(nullptr),
        SND_RESOURCE | SND_ASYNC | SND_NODEFAULT);
}

} // namespace

BurnEngine::~BurnEngine() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    ClearPreparedXgd3();
}

void BurnEngine::ClearPreparedXgd3() {
    std::wstring directory;

    {
        std::lock_guard lock(mutex_);
        if (!preparedXgd3_.valid &&
            preparedXgd3_.workingDirectory.empty()) {
            return;
        }

        directory = std::move(preparedXgd3_.workingDirectory);
        preparedXgd3_ = PreparedXgd3Cache{};
    }

    if (!directory.empty()) {
        std::error_code ignored;
        fs::remove_all(fs::path(directory), ignored);
    }
}

bool BurnEngine::TryReusePreparedXgd3(
    const std::wstring& sourcePath,
    std::wstring& workingImagePath) {
    std::uint64_t currentBytes = 0;
    std::uint64_t currentWriteTime = 0;
    if (!QueryFileIdentity(
            sourcePath,
            currentBytes,
            currentWriteTime)) {
        ClearPreparedXgd3();
        return false;
    }

    std::wstring staleDirectory;
    {
        std::lock_guard lock(mutex_);

        const bool matches =
            preparedXgd3_.valid &&
            SameWindowsPath(
                preparedXgd3_.sourcePath,
                sourcePath) &&
            preparedXgd3_.sourceBytes == currentBytes &&
            preparedXgd3_.sourceWriteTime == currentWriteTime &&
            !preparedXgd3_.workingImagePath.empty() &&
            fs::is_regular_file(
                fs::path(preparedXgd3_.workingImagePath));

        if (matches) {
            workingImagePath =
                preparedXgd3_.workingImagePath;
            return true;
        }

        staleDirectory =
            std::move(preparedXgd3_.workingDirectory);
        preparedXgd3_ = PreparedXgd3Cache{};
    }

    if (!staleDirectory.empty()) {
        std::error_code ignored;
        fs::remove_all(
            fs::path(staleDirectory),
            ignored);
    }

    return false;
}

void BurnEngine::StorePreparedXgd3(
    const std::wstring& sourcePath,
    const std::wstring& workingDirectory,
    const std::wstring& workingImagePath) {
    std::uint64_t sourceBytes = 0;
    std::uint64_t sourceWriteTime = 0;
    if (!QueryFileIdentity(
            sourcePath,
            sourceBytes,
            sourceWriteTime)) {
        return;
    }

    std::wstring staleDirectory;
    {
        std::lock_guard lock(mutex_);
        staleDirectory =
            std::move(preparedXgd3_.workingDirectory);

        preparedXgd3_.valid = true;
        preparedXgd3_.sourcePath = sourcePath;
        preparedXgd3_.workingDirectory =
            workingDirectory;
        preparedXgd3_.workingImagePath =
            workingImagePath;
        preparedXgd3_.sourceBytes = sourceBytes;
        preparedXgd3_.sourceWriteTime =
            sourceWriteTime;
    }

    if (!staleDirectory.empty() &&
        !SameWindowsPath(
            staleDirectory,
            workingDirectory)) {
        std::error_code ignored;
        fs::remove_all(
            fs::path(staleDirectory),
            ignored);
    }
}

bool BurnEngine::Start(BurnRequest request) {
    {
        std::lock_guard lock(mutex_);
        if (state_.busy) {
            return false;
        }
        state_ = BurnSnapshot{};
        // RB_LOG_CR_RESET
        logPendingCarriageReturn_ = false;
        state_.busy = true;
        state_.stage = BurnStage::Preparing;
        state_.status =
            request.burnerMaxOnly
                ? "Testing / enabling BurnerMAX..."
                : (request.checkOnly
                    ? "Checking disc image..."
                    : (request.simulate
                        ? "Preparing simulated write..."
                        : "Verifying burner and blank media..."));
    }
    worker_ = std::jthread([this, request = std::move(request)]() mutable {
        Run(std::move(request));
    });
    return true;
}

// RB_DEDICATED_BURNERMAX_TEST_V84J
bool BurnEngine::StartBurnerMaxTest(
    std::wstring opticalDriveRoot) {
    {
        std::lock_guard lock(mutex_);
        if (state_.busy) {
            return false;
        }

        state_ = BurnSnapshot{};
        logPendingCarriageReturn_ = false;
        state_.busy = true;
        state_.stage = BurnStage::Preparing;
        state_.status = "Testing / enabling BurnerMAX...";
    }

    worker_ = std::jthread(
        [this, opticalDriveRoot = std::move(opticalDriveRoot)]() mutable {
            const EmbeddedToolPaths& tools = GetEmbeddedToolPaths();

            if (!tools.Ready()) {
                SetFailure(
                    "The embedded recording backend could not be prepared: " +
                    tools.error);
                return;
            }

            BurnRequest request;
            request.target = BurnTarget::Xbox360;
            request.xbox360DiscType = Xbox360DiscType::Xgd3;
            request.opticalDriveRoot =
                std::move(opticalDriveRoot);
            request.burnerMaxOnly = true;

            AppendLog(
                "BurnerMAX standalone test\r\n"
                "Image validation: BYPASSED (not required)\r\n"
                "Disc sector writes: DISABLED\r\n");

            RunBurnerMaxOnly(
                std::move(request),
                tools.dvdMediaInfo.wstring());
        });

    return true;
}
BurnSnapshot BurnEngine::Snapshot() const {
    std::lock_guard lock(mutex_);
    BurnSnapshot snapshot = state_;
    snapshot.xgd3Prepared = preparedXgd3_.valid;
    snapshot.preparedXgd3SourcePath =
        preparedXgd3_.sourcePath;
    snapshot.preparedXgd3WorkingPath =
        preparedXgd3_.workingImagePath;
    return snapshot;
}

void BurnEngine::Reset() {
    std::lock_guard lock(mutex_);
    if (!state_.busy) {
        state_ = BurnSnapshot{};
        // RB_LOG_CR_RESET
        logPendingCarriageReturn_ = false;
    }
}

void BurnEngine::AppendLog(const std::string& text) {
    std::lock_guard lock(mutex_);
    if (text.empty()) {
        return;
    }

    // RB_TERMINAL_LOG_CR_HANDLING

    // A lone CR means "redraw this console line". CRLF remains a newline.

    const auto eraseCurrentLogLine = [this]() {

        const std::size_t newline =

            state_.log.find_last_of('\n');



        if (newline == std::string::npos) {

            state_.log.clear();

        } else {

            state_.log.erase(newline + 1);

        }

    };



    std::size_t textIndex = 0;



    // A pipe read can split "\r\n" across two chunks.

    if (logPendingCarriageReturn_) {

        if (!text.empty() && text.front() == '\n') {

            state_.log.push_back('\n');

            textIndex = 1;

        } else {

            eraseCurrentLogLine();

        }

        logPendingCarriageReturn_ = false;

    }



    while (textIndex < text.size()) {

        const char ch = text[textIndex];



        if (ch != '\r') {

            state_.log.push_back(ch);

            ++textIndex;

            continue;

        }



        if (textIndex + 1 >= text.size()) {

            logPendingCarriageReturn_ = true;

            break;

        }



        if (text[textIndex + 1] == '\n') {

            state_.log.push_back('\n');

            textIndex += 2;

            continue;

        }



        eraseCurrentLogLine();

        ++textIndex;

    }
    // cdrtools' Windows/Cygwin build prints a number of privilege warnings
    // even when SPTI access is working normally. They are expected noise in
    // Retro Burner and would otherwise look like fatal errors to users.
    static constexpr std::array<std::string_view, 11> benignCdrecordLines = {
        "cdrecord: Insufficient 'file read' privileges.",
        "cdrecord: Insufficient 'file write' privileges.",
        "cdrecord: Insufficient 'device' privileges.",
        "cdrecord: Insufficient 'memlock' privileges.",
        "cdrecord: Insufficient 'priocntl' privileges.",
        "cdrecord: Insufficient 'network' privileges.",
        "cdrecord: Warning: using inofficial version of libscg",
        "cdrecord: Warning: Cannot read drive buffer.",
        "cdrecord: Warning: The DMA speed test has been skipped.",
        "cdrecord: WARNING: Drive returns wrong startsec (0) using -150",
        "No Media Present or Unknown Capacity",
    };

    const auto removeCompleteLineContaining =
        [this](const std::string_view marker) {
            std::size_t searchFrom = 0;
            while (true) {
                const std::size_t hit =
                    state_.log.find(marker, searchFrom);
                if (hit == std::string::npos) {
                    break;
                }

                const std::size_t lineEnd =
                    state_.log.find('\n', hit);
                if (lineEnd == std::string::npos) {
                    // The child process may have split this line across pipe
                    // reads. Leave it until the next chunk completes it.
                    break;
                }

                const std::size_t previousNewline =
                    hit == 0
                        ? std::string::npos
                        : state_.log.rfind('\n', hit - 1);
                const std::size_t lineStart =
                    previousNewline == std::string::npos
                        ? 0
                        : previousNewline + 1;
                state_.log.erase(
                    lineStart,
                    lineEnd - lineStart + 1);
                searchFrom = lineStart;
            }
        };

    for (const std::string_view marker : benignCdrecordLines) {
        removeCompleteLineContaining(marker);
    }

    // cdrecord animates its ten-second countdown with terminal backspaces.
    // Replace that terminal-only line with one clean GUI-friendly message.
    constexpr std::string_view countdownMarker =
        "Last chance to quit, starting real write in";
    while (true) {
        const std::size_t hit =
            state_.log.find(countdownMarker);
        if (hit == std::string::npos) {
            break;
        }
        const std::size_t lineEnd =
            state_.log.find('\n', hit);
        if (lineEnd == std::string::npos) {
            break;
        }
        const std::size_t previousNewline =
            hit == 0
                ? std::string::npos
                : state_.log.rfind('\n', hit - 1);
        const std::size_t lineStart =
            previousNewline == std::string::npos
                ? 0
                : previousNewline + 1;
        state_.log.replace(
            lineStart,
            lineEnd - lineStart + 1,
            "Starting burn...\r\n");
    }

    state_.log.erase(
        std::remove(state_.log.begin(), state_.log.end(), '\b'),
        state_.log.end());

    if (state_.log.size() > kMaximumLogBytes) {
        state_.log.erase(
            0,
            state_.log.size() - kMaximumLogBytes);
    }
}
void BurnEngine::SetPreparationProgress(
    const float progress,
    std::string status) {
    std::lock_guard lock(mutex_);
    state_.progress = std::clamp(progress, 0.0F, 1.0F);
    state_.status = std::move(status);
    state_.writing = false;
}

void BurnEngine::SetFailure(std::string message) {
    AppendLog("\r\nERROR: " + message + "\r\n");
    std::lock_guard lock(mutex_);
    state_.stage = BurnStage::Failed;
    state_.busy = false;
    state_.writing = false;
    state_.status = std::move(message);
}

void BurnEngine::RunBurnerMaxOnly(
    BurnRequest request,
    const std::wstring& dvdMediaInfoPath) {
    if (request.target != BurnTarget::Xbox360 ||
        request.xbox360DiscType != Xbox360DiscType::Xgd3) {
        SetFailure("BurnerMAX testing is available only for Xbox 360 XGD3.");
        return;
    }
    if (request.opticalDriveRoot.empty()) {
        SetFailure("The selected Windows DVD drive has no drive letter.");
        return;
    }

    const fs::path dvdMediaInfo(dvdMediaInfoPath);
    if (!fs::is_regular_file(dvdMediaInfo)) {
        SetFailure("The embedded DVD media-info backend could not be prepared.");
        return;
    }

    const fs::path workingDirectory =
        dvdMediaInfo.has_parent_path()
            ? dvdMediaInfo.parent_path()
            : fs::current_path();

    static const std::regex mountedProfilePattern(
        R"(Mounted Media:\s+([0-9A-Fa-f]+)h,)",
        std::regex::icase);
    static const std::regex blankDiscPattern(
        R"(Disc status:\s+blank)",
        std::regex::icase);
    static const std::regex freeBlocksPattern(
        R"(Free Blocks:\s+([0-9]+)\*2KB)",
        std::regex::icase);

    auto runMediaInfo =
        [this, &dvdMediaInfo, &workingDirectory, &request](
            const char* heading,
            std::string& output) -> bool {
            output.clear();
            AppendLog(std::string("\r\n") + heading + "\r\n");

            const auto append =
                [this, &output](const std::string& text) {
                    output += text;
                    AppendLog(text);
                };
            const auto ignoreLine =
                [](std::string_view) {};

            const ProcessResult result = RunHiddenProcess(
                dvdMediaInfo,
                {request.opticalDriveRoot},
                workingDirectory,
                append,
                ignoreLine);

            return result.started && result.exitCode == 0;
        };

    std::string mediaInfoOutput;
    if (!runMediaInfo(
            "BurnerMAX DVD+R DL preflight (dvd+rw-mediainfo)",
            mediaInfoOutput)) {
        SetFailure(
            "dvd+rw-mediainfo could not validate the selected DVD writer/media. "
            "No disc data was written.");
        return;
    }

    std::smatch match;
    unsigned mediaProfile = 0;
    if (std::regex_search(
            mediaInfoOutput,
            match,
            mountedProfilePattern)) {
        mediaProfile = static_cast<unsigned>(
            std::stoul(match[1].str(), nullptr, 16));
    }

    if (mediaProfile != 0x2B) {
        SetFailure(
            "BurnerMAX testing requires a blank DVD+R DL in the selected drive.");
        return;
    }
    if (!std::regex_search(mediaInfoOutput, blankDiscPattern)) {
        SetFailure(
            "BurnerMAX testing requires media positively reported as blank. "
            "No disc data was written.");
        return;
    }

    {
        std::lock_guard lock(mutex_);
        state_.status = "Scanning drive for BurnerMAX support...";
    }

    const BurnerMaxResult burnerMax = EnableBurnerMax(
        request.opticalDriveRoot,
        [this](const std::string& text) {
            AppendLog(text);
        });

    if (!burnerMax.Success()) {
        SetFailure(burnerMax.message);
        return;
    }

    std::string refreshedMediaInfo;
    if (!runMediaInfo(
            "BurnerMAX capacity verification (dvd+rw-mediainfo)",
            refreshedMediaInfo)) {
        SetFailure(
            "BurnerMAX vendor-command verification passed, but writable capacity "
            "could not be re-read. Eject/reinsert the blank disc and retry.");
        return;
    }

    constexpr std::uintmax_t kBurnerMaxFullCapacitySectors = 4267040ULL;
    std::uintmax_t freeBlocks = 0;
    if (std::regex_search(
            refreshedMediaInfo,
            match,
            freeBlocksPattern)) {
        freeBlocks = std::stoull(match[1].str());
    }

    if (freeBlocks < kBurnerMaxFullCapacitySectors) {
        SetFailure(
            "BurnerMAX layer-boundary verification passed, but the drive reports only " +
            std::to_string(freeBlocks) +
            " writable sectors. Full XGD3 capacity requires at least 4267040 sectors.");
        return;
    }

    AppendLog(
        "\r\nBurnerMAX SUCCESS\r\n"
        "Expanded writable capacity: " +
        std::to_string(freeBlocks) +
        " sectors (" +
        std::to_string(freeBlocks * 2048ULL) +
        " bytes)\r\n"
        "No disc sectors were written.\r\n");

    std::lock_guard lock(mutex_);
    state_.stage = BurnStage::Ready;
    state_.busy = false;
    state_.writing = false;
    state_.progress = 0.0F;
    state_.layout = "Xbox 360 XGD3 - BurnerMAX";
    state_.status =
        burnerMax.status == BurnerMaxStatus::AlreadyEnabled
            ? "BurnerMAX already active. Expanded XGD3 capacity verified."
            : "BurnerMAX enabled via " + burnerMax.backend +
                  ". Expanded XGD3 capacity verified.";
}

void BurnEngine::RunStandardImage(
    BurnRequest request,
    const std::wstring& retrobeamPath,
    const std::wstring& growisofsPath,
    const std::wstring& dvdMediaInfoPath,
    const std::wstring& abgx360Path) {
    const fs::path retrobeam(retrobeamPath);
    const fs::path growisofs(growisofsPath);
    const fs::path imagePath(request.cdiPath);
    const fs::path workingDirectory =
        imagePath.has_parent_path()
            ? imagePath.parent_path()
            : fs::current_path();

    std::string layout;
    std::string validationError;
    int cueTrackCount = 1;

    if (!ValidateStandardImage(
            request,
            layout,
            cueTrackCount,
            validationError)) {
        SetFailure(std::move(validationError));
        return;
    }

    {
        std::lock_guard lock(mutex_);
        state_.layout = layout;
        state_.status = "Image validated: " + layout;
    }

    AppendLog(
        "\r\nRetro Burner image check\r\n"
        "Target: " + std::string(BurnTargetName(request.target)) + "\r\n"
        "Image: " + imagePath.string() + "\r\n"
        "Layout: " + layout + "\r\n");

    if (request.checkOnly) {
        std::lock_guard lock(mutex_);
        state_.stage = BurnStage::Ready;
        state_.busy = false;
        state_.writing = false;
        state_.status =
            layout + ". Check complete; no disc was written.";
        return;
    }

    const bool dvdTarget =
        request.target == BurnTarget::PlayStation2Dvd ||
        request.target == BurnTarget::Xbox360;

    if (request.simulate && !dvdTarget) {
        SetFailure("Dry run is available only for DVD targets.");
        return;
    }

    const bool xbox = request.target == BurnTarget::Xbox360;
    const bool xgd3 =
        xbox &&
        request.xbox360DiscType == Xbox360DiscType::Xgd3;

    TemporaryDirectory abgxTemporary;
    fs::path burnImagePath = imagePath;

    if (xgd3) {
        std::wstring preparedWorkingImage;
        const bool reusedPreparedXgd3 =
            TryReusePreparedXgd3(
                imagePath.wstring(),
                preparedWorkingImage);

        if (reusedPreparedXgd3) {
            burnImagePath =
                fs::path(preparedWorkingImage);

            AppendLog(
                "\r\nXGD3 prepared-image cache\r\n"
                "Reusing the ABGX360-verified working copy from the successful "
                "preflight/preparation pass.\r\n"
                "Original ISO: " + imagePath.string() + "\r\n"
                "Prepared ISO: " + burnImagePath.string() + "\r\n"
                "ABGX360 copy/AutoFix/verification will not be repeated.\r\n");

            SetPreparationProgress(
                1.0F,
                "XGD3 already prepared - reusing verified ABGX360 working copy.");
        } else {
            const fs::path abgx360(abgx360Path);
            if (!fs::is_regular_file(abgx360)) {
                SetFailure(
                    "The embedded ABGX360 backend could not be prepared. "
                    "XGD3 burns require an ABGX360 AutoFix pass.");
                return;
            }

            std::error_code sourceSizeError;
            const std::uintmax_t sourceBytes =
                fs::file_size(imagePath, sourceSizeError);
            if (sourceSizeError || sourceBytes == 0) {
                SetFailure("Could not read the selected XGD3 ISO size.");
                return;
            }

            ULARGE_INTEGER freeBytes{};
            if (GetDiskFreeSpaceExW(
                    workingDirectory.c_str(),
                    &freeBytes,
                    nullptr,
                    nullptr) &&
                freeBytes.QuadPart <
                    sourceBytes + kMinimumFreeSpaceMargin) {
                SetFailure(
                    "Not enough free space beside the XGD3 ISO to create the "
                    "temporary ABGX360 working copy. The original ISO is never modified.");
                return;
            }

            {
                std::lock_guard lock(mutex_);
                state_.status =
                    "Creating temporary XGD3 working copy for ABGX360...";
                state_.writing = false;
                state_.progress = 0.0F;
            }

            abgxTemporary = MakeTemporaryDirectoryNear(
                workingDirectory,
                L".RetroBurner-ABGX360");
            if (abgxTemporary.path.empty()) {
                SetFailure(
                    "Could not create a temporary ABGX360 working directory "
                    "beside the selected ISO.");
                return;
            }

            burnImagePath =
                abgxTemporary.path / imagePath.filename();

            AppendLog(
                "\r\nABGX360 XGD3 prerequisite\r\n"
                "Retro Burner will AutoFix a temporary working copy.\r\n"
                "The selected original ISO will not be modified.\r\n"
                "Working image: " + burnImagePath.string() + "\r\n");

            CopyProgressContext copyProgress{this};
            if (!CopyFileExW(
                    imagePath.c_str(),
                    burnImagePath.c_str(),
                    CopyProgressRoutine,
                    &copyProgress,
                    nullptr,
                    COPY_FILE_FAIL_IF_EXISTS)) {
                SetFailure(
                    "Could not create the temporary ABGX360 working copy: " +
                    Win32Error(GetLastError()));
                return;
            }

            SetPreparationProgress(
                0.0F,
                "ABGX360 AutoFix Level 3 - starting...");

            std::string autoFixOutput;
            bool autoFixGameCrc = false;
            const std::regex abgxVideoPercent(
                R"(Checking Video CRC\.\.\.\s*([0-9]{1,3})%)",
                std::regex::icase);
            const std::regex abgxTablePercent(
                R"(^\s*([0-9]{1,3})%\s+)",
                std::regex::icase);
            const std::regex abgxSpeed(
                R"(([0-9]+(?:\.[0-9]+)?)\s+MB/s)",
                std::regex::icase);

            const auto appendAbgx =
                [this, &autoFixOutput](const std::string& text) {
                    autoFixOutput += text;
                    AppendLog(text);
                };
            const auto parseAutoFixLine =
                [this,
                 &autoFixGameCrc,
                 &abgxVideoPercent,
                 &abgxTablePercent,
                 &abgxSpeed](std::string_view line) {
                    int percent = 0;

                    if (line.rfind("Downloading ", 0) == 0) {
                        SetPreparationProgress(
                            0.0F,
                            "ABGX360 AutoFix - downloading verification data...");
                        return;
                    }

                    if (line.find("Checking Game CRC") !=
                        std::string_view::npos) {
                        autoFixGameCrc = true;
                        SetPreparationProgress(
                            0.0F,
                            "ABGX360 AutoFix - checking game CRC...");
                        return;
                    }

                    if (TryParsePercent(
                            line,
                            abgxVideoPercent,
                            percent)) {
                        SetPreparationProgress(
                            static_cast<float>(percent) / 100.0F,
                            "ABGX360 AutoFix - checking video CRC...");
                        return;
                    }

                    if (autoFixGameCrc &&
                        TryParsePercent(
                            line,
                            abgxTablePercent,
                            percent)) {
                        std::string detail =
                            "ABGX360 AutoFix - checking game CRC";
                        std::match_results<
                            std::string_view::const_iterator> speedMatch;
                        if (std::regex_search(
                                line.begin(),
                                line.end(),
                                speedMatch,
                                abgxSpeed)) {
                            detail +=
                                " - " +
                                speedMatch[1].str() +
                                " MB/s";
                        }
                        SetPreparationProgress(
                            static_cast<float>(percent) / 100.0F,
                            std::move(detail));
                    }
                };

            AppendLog(
                "\r\nABGX360 AutoFix Level 3\r\n");

            const ProcessResult autoFix = RunHiddenProcess(
                abgx360,
                {
                    L"-s",
                    L"--af3",
                    L"--",
                    burnImagePath.wstring(),
                },
                abgxTemporary.path,
                appendAbgx,
                parseAutoFixLine);

            if (!autoFix.started) {
                SetFailure(autoFix.error);
                return;
            }
            if (autoFix.exitCode != 0) {
                SetFailure(
                    "ABGX360 AutoFix could not complete. "
                    "XGD3 burning has been stopped before BurnerMAX or disc writing.");
                return;
            }

            if (ContainsCaseInsensitive(
                    autoFixOutput,
                    "autofix failed") ||
                ContainsCaseInsensitive(
                    autoFixOutput,
                    "aborting autofix")) {
                SetFailure(
                    "ABGX360 reported that AutoFix failed. "
                    "XGD3 burning has been stopped; check the ABGX360 log.");
                return;
            }

            SetPreparationProgress(
                0.0F,
                "ABGX360 verification - starting read-only pass...");

            std::string verifyOutput;
            bool verifyGameCrc = false;
            const auto appendVerify =
                [this, &verifyOutput](const std::string& text) {
                    verifyOutput += text;
                    AppendLog(text);
                };
            const auto parseVerifyLine =
                [this,
                 &verifyGameCrc,
                 &abgxVideoPercent,
                 &abgxTablePercent,
                 &abgxSpeed](std::string_view line) {
                    int percent = 0;

                    if (line.find("Checking Game CRC") !=
                        std::string_view::npos) {
                        verifyGameCrc = true;
                        SetPreparationProgress(
                            0.0F,
                            "ABGX360 verification - checking game CRC...");
                        return;
                    }

                    if (TryParsePercent(
                            line,
                            abgxVideoPercent,
                            percent)) {
                        SetPreparationProgress(
                            static_cast<float>(percent) / 100.0F,
                            "ABGX360 verification - checking video CRC...");
                        return;
                    }

                    if (verifyGameCrc &&
                        TryParsePercent(
                            line,
                            abgxTablePercent,
                            percent)) {
                        std::string detail =
                            "ABGX360 verification - checking game CRC";
                        std::match_results<
                            std::string_view::const_iterator> speedMatch;
                        if (std::regex_search(
                                line.begin(),
                                line.end(),
                                speedMatch,
                                abgxSpeed)) {
                            detail +=
                                " - " +
                                speedMatch[1].str() +
                                " MB/s";
                        }
                        SetPreparationProgress(
                            static_cast<float>(percent) / 100.0F,
                            std::move(detail));
                    }
                };

            AppendLog(
                "\r\nABGX360 verification - writes disabled\r\n");

            const ProcessResult verify = RunHiddenProcess(
                abgx360,
                {
                    L"-s",
                    L"-w",
                    L"-o",
                    L"--af3",
                    L"--",
                    burnImagePath.wstring(),
                },
                abgxTemporary.path,
                appendVerify,
                parseVerifyLine);

            if (!verify.started) {
                SetFailure(verify.error);
                return;
            }
            if (verify.exitCode != 0) {
                SetFailure(
                    "ABGX360 verification could not complete. "
                    "XGD3 burning has been stopped before BurnerMAX or disc writing.");
                return;
            }

            if (!ContainsCaseInsensitive(
                    verifyOutput,
                    "checking topology data")) {
                SetFailure(
                    "ABGX360 did not report an XGD3 topology-data check. "
                    "Confirm that the selected image is an XGD3 Xbox 360 ISO.");
                return;
            }

            if (ContainsCaseInsensitive(
                    verifyOutput,
                    "first 12 sectors of topology data are blank") ||
                ContainsCaseInsensitive(
                    verifyOutput,
                    "topology data is blank") ||
                ContainsCaseInsensitive(
                    verifyOutput,
                    "autofix failed") ||
                ContainsCaseInsensitive(
                    verifyOutput,
                    "aborting autofix")) {
                SetFailure(
                    "ABGX360 verification still reports missing/unfixed XGD3 data. "
                    "The burn has been blocked.");
                return;
            }

            const bool onlineVerificationWarning =
                ContainsCaseInsensitive(
                    verifyOutput,
                    "verification failed");

            AppendLog(
                onlineVerificationWarning
                    ? "\r\nABGX360: AutoFix completed and topology is present. "
                      "Online game verification was not conclusive; review the log.\r\n"
                    : "\r\nABGX360: XGD3 AutoFix prerequisite passed.\r\n");

            SetPreparationProgress(
                1.0F,
                onlineVerificationWarning
                    ? "ABGX360 patched XGD3; online verification warning - continuing preflight..."
                    : "ABGX360 XGD3 prerequisite passed.");

            const fs::path retainedDirectory =
                abgxTemporary.Release();
            StorePreparedXgd3(
                imagePath.wstring(),
                retainedDirectory.wstring(),
                burnImagePath.wstring());

            AppendLog(
                "\r\nXGD3 prepared working copy cached for this Retro Burner session.\r\n"
                "A following Burn will reuse this verified copy instead of repeating "
                "the ISO copy and ABGX360 passes.\r\n");
        }
    }

    if (dvdTarget) {
        if (request.opticalDriveRoot.empty()) {
            SetFailure("The selected Windows DVD drive has no drive letter.");
            return;
        }

        const fs::path dvdMediaInfo(dvdMediaInfoPath);
        if (!fs::is_regular_file(dvdMediaInfo) ||
            (!request.useGrowisofsForDvd && !fs::is_regular_file(retrobeam)) ||
            (request.useGrowisofsForDvd && !fs::is_regular_file(growisofs))) {
            SetFailure(
                request.useGrowisofsForDvd
                    ? "The embedded growisofs/DVD media backend could not be prepared."
                    : "The embedded RetroBeam/DVD media backend could not be prepared.");
            return;
        }

        std::string mediaInfoOutput;
        const auto appendMediaInfo =
            [this, &mediaInfoOutput](const std::string& text) {
                mediaInfoOutput += text;
                AppendLog(text);
            };
        const auto ignoreLine =
            [](std::string_view) {};

        AppendLog("\r\nDVD media preflight (dvd+rw-mediainfo)\r\n");
        const ProcessResult mediaInfo = RunHiddenProcess(
            dvdMediaInfo,
            {request.opticalDriveRoot},
            workingDirectory,
            appendMediaInfo,
            ignoreLine);

        if (!mediaInfo.started) {
            SetFailure(mediaInfo.error);
            return;
        }
        if (mediaInfo.exitCode != 0) {
            SetFailure(
                "dvd+rw-mediainfo could not validate the selected DVD writer/media. "
                "No image data was written.");
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

        std::smatch mediaMatch;
        unsigned mediaProfile = 0;
        if (std::regex_search(
                mediaInfoOutput,
                mediaMatch,
                mountedProfilePattern)) {
            mediaProfile = static_cast<unsigned>(
                std::stoul(mediaMatch[1].str(), nullptr, 16));
        }

        if (!std::regex_search(mediaInfoOutput, blankDiscPattern)) {
            SetFailure(
                "The selected DVD media is not positively reported as blank. "
                "No image data was written.");
            return;
        }

        std::error_code sizeError;
        const std::uintmax_t imageBytes =
            fs::file_size(burnImagePath, sizeError);
        if (sizeError || imageBytes == 0) {
            SetFailure("Could not read the selected image size.");
            return;
        }

        constexpr std::uintmax_t kDvdSingleLayerBytes = 4707319808ULL;
        const bool ps2NeedsDualLayer =
            request.target == BurnTarget::PlayStation2Dvd &&
            imageBytes > kDvdSingleLayerBytes;

        const bool isSingleLayerRecordable =
            mediaProfile == 0x11 || // DVD-R
            mediaProfile == 0x1B;   // DVD+R
        const bool isDualLayerRecordable =
            mediaProfile == 0x15 || // DVD-R DL sequential
            mediaProfile == 0x16 || // DVD-R DL layer jump
            mediaProfile == 0x2B;   // DVD+R DL

        if (request.target == BurnTarget::Xbox360) {
            if (mediaProfile != 0x2B) {
                SetFailure(
                    "Xbox 360 backups require blank DVD+R DL media. "
                    "DVD-R DL and single-layer media are not accepted.");
                return;
            }
        } else if (ps2NeedsDualLayer) {
            if (!isDualLayerRecordable) {
                SetFailure(
                    "This PS2 ISO requires dual-layer media. Insert a blank "
                    "DVD-R DL or DVD+R DL and retry.");
                return;
            }
        } else if (!isSingleLayerRecordable && !isDualLayerRecordable) {
            SetFailure(
                "PlayStation 2 DVD burning requires blank DVD-R, DVD+R, "
                "DVD-R DL or DVD+R DL media.");
            return;
        }

        if (!request.useGrowisofsForDvd) {
            if (request.cdrecordDevice.empty()) {
                SetFailure(
                    "RetroBeam has no mapped SCSI address for the selected DVD writer. "
                    "Press Refresh and try again.");
                return;
            }

            AppendLog("\r\nRetroBeam read-only drive preflight\r\n");
            const ProcessResult retrobeamPreflight = RunHiddenProcess(
                retrobeam,
                {
                    L"dev=" + std::wstring(
                        request.cdrecordDevice.begin(),
                        request.cdrecordDevice.end()),
                    L"-checkdrive",
                },
                workingDirectory,
                [this](const std::string& text) { AppendLog(text); },
                ignoreLine);
            if (!retrobeamPreflight.started) {
                SetFailure(retrobeamPreflight.error);
                return;
            }
            if (retrobeamPreflight.exitCode != 0) {
                SetFailure(
                    "RetroBeam could not validate the selected DVD writer. "
                    "No image data was written.");
                return;
            }
        } else {
            AppendLog(
                "\r\nDVD backend selected: growisofs\r\n"
                "RetroBeam drive preflight is intentionally skipped for this A/B path.\r\n");
        }

        if (xgd3) {
            {
                std::lock_guard lock(mutex_);
                state_.status = "Testing / enabling BurnerMAX for XGD3...";
            }

            const BurnerMaxResult burnerMax = EnableBurnerMax(
                request.opticalDriveRoot,
                [this](const std::string& text) {
                    AppendLog(text);
                });

            if (!burnerMax.Success()) {
                SetFailure(
                    "XGD3 BurnerMAX preflight failed: " +
                    burnerMax.message);
                return;
            }

            mediaInfoOutput.clear();
            AppendLog(
                "\r\nDVD media capacity after BurnerMAX (dvd+rw-mediainfo)\r\n");

            const ProcessResult refreshedMediaInfo = RunHiddenProcess(
                dvdMediaInfo,
                {request.opticalDriveRoot},
                workingDirectory,
                appendMediaInfo,
                ignoreLine);

            if (!refreshedMediaInfo.started ||
                refreshedMediaInfo.exitCode != 0) {
                SetFailure(
                    "BurnerMAX verification succeeded, but Retro Burner could not "
                    "re-read the expanded writable capacity. No image data was written.");
                return;
            }
        }

        std::uintmax_t freeBlocks = 0;
        const bool capacityReported =
            std::regex_search(
                mediaInfoOutput,
                mediaMatch,
                freeBlocksPattern);
        if (capacityReported) {
            freeBlocks = std::stoull(mediaMatch[1].str());
        }

        if (xgd3) {
            constexpr std::uintmax_t kBurnerMaxFullCapacitySectors =
                4267040ULL;
            if (!capacityReported) {
                SetFailure(
                    "BurnerMAX layer-boundary verification passed, but the drive "
                    "did not report writable capacity. No image data was written.");
                return;
            }
            if (freeBlocks < kBurnerMaxFullCapacitySectors) {
                SetFailure(
                    "BurnerMAX layer-boundary verification passed, but the drive "
                    "reports only " +
                    std::to_string(freeBlocks) +
                    " writable sectors. Full XGD3 capacity requires at least "
                    "4267040 sectors.");
                return;
            }

            AppendLog(
                "\r\nBurnerMAX expanded capacity verified: " +
                std::to_string(freeBlocks) +
                " sectors.\r\n");
        }

        if (capacityReported) {
            const std::uintmax_t reportedCapacity =
                freeBlocks * 2048ULL;
            if (imageBytes > reportedCapacity) {
                SetFailure(
                    "The selected image does not fit the writable capacity reported "
                    "for the inserted DVD.");
                return;
            }
        }

        const std::string targetDescription =
            xbox
                ? (xgd3 ? "Xbox 360 XGD3" : "Xbox 360 XGD2")
                : "PlayStation 2 DVD";

        // RB_DVD_BACKEND_SELECTOR_RESTORED_V84F
        // RetroBeam is the default backend, but growisofs remains available
        // for drive/media compatibility and A/B testing.
        if (request.useGrowisofsForDvd) {
            {
                std::lock_guard lock(mutex_);
                state_.stage = BurnStage::BurningSession1;
                state_.busy = true;
                state_.writing = !request.simulate;
                state_.progress = 0.0F;
                state_.session = 1;
                state_.bufferPercent = -1;
                state_.ringBufferPercent = -1;
                state_.driveBufferPercent = -1;
                state_.actualSpeed.clear();
                state_.remainingTime.clear();
                state_.status = request.simulate
                    ? targetDescription + " growisofs dry run - no write..."
                    : "Writing " + targetDescription + " with growisofs...";
            }

            bool ps2GrowisofsDualLayer = false;
            std::uint64_t ps2GrowisofsLayerBreak = 0;

            if (request.target == BurnTarget::PlayStation2Dvd) {
                std::error_code ps2GrowisofsSizeError;
                const std::uintmax_t ps2ImageBytes =
                    fs::file_size(
                        burnImagePath,
                        ps2GrowisofsSizeError);

                constexpr std::uintmax_t kDvdRSingleLayerBytes =
                    4707319808ULL;
                constexpr std::uintmax_t kDvdPlusRDualLayerNominalBytes =
                    8547991552ULL;

                if (ps2GrowisofsSizeError ||
                    ps2ImageBytes == 0 ||
                    (ps2ImageBytes % 2048ULL) != 0ULL) {
                    SetFailure(
                        "Could not determine a valid 2048-byte-sector PS2 DVD ISO size.");
                    return;
                }

                if (ps2ImageBytes > kDvdPlusRDualLayerNominalBytes) {
                    SetFailure(
                        "The PS2 DVD ISO is larger than nominal DVD9 capacity.");
                    return;
                }

                ps2GrowisofsDualLayer =
                    ps2ImageBytes > kDvdRSingleLayerBytes;

                if (ps2GrowisofsDualLayer) {
                    const std::uint64_t totalSectors =
                        static_cast<std::uint64_t>(
                            ps2ImageBytes / 2048ULL);
                    const std::uint64_t minimumLayer0 =
                        (totalSectors + 1ULL) / 2ULL;

                    ps2GrowisofsLayerBreak =
                        (minimumLayer0 + 15ULL) &
                        ~std::uint64_t{15ULL};

                    if (ps2GrowisofsLayerBreak >= totalSectors) {
                        SetFailure(
                            "Could not calculate a valid PS2 DVD9 layer break.");
                        return;
                    }

                    AppendLog(
                        "\r\nPS2 DVD mode: DVD9 / dual-layer "
                        "(selected automatically from ISO size)\r\n"
                        "Calculated PS2 DVD9 layer break: " +
                        std::to_string(ps2GrowisofsLayerBreak) +
                        " sectors\r\n");
                } else {
                    AppendLog(
                        "\r\nPS2 DVD mode: DVD5 / single-layer "
                        "(selected automatically from ISO size)\r\n");
                }
            }

            std::vector<std::wstring> arguments;
            if (request.simulate) {
                arguments.emplace_back(L"-dry-run");
            }

            // Console DVD policy: explicitly request DAO.
            arguments.emplace_back(L"-use-the-force-luke=dao");

            if (xbox || ps2GrowisofsDualLayer) {
                const std::uint64_t selectedLayerBreak =
                    xbox
                        ? static_cast<std::uint64_t>(
                            xgd3 ? 2133520 : 1913760)
                        : ps2GrowisofsLayerBreak;

                arguments.push_back(
                    L"-use-the-force-luke=break:" +
                    std::to_wstring(selectedLayerBreak));
            }

            arguments.emplace_back(L"-dvd-compat");
            if (request.requestedSpeedX > 0) {
                arguments.push_back(
                    L"-speed=" + std::to_wstring(request.requestedSpeedX));
            }
            arguments.emplace_back(L"-Z");
            arguments.push_back(
                request.opticalDriveRoot + L"=" + burnImagePath.wstring());

            AppendLog(
                request.simulate
                    ? "\r\nDVD DRY RUN - growisofs -dry-run\r\nWrite type: DAO\r\n"
                    : "\r\nDVD WRITE - growisofs\r\nWrite type: DAO\r\n");

            static const std::regex growProgressPattern(
                R"(\(\s*([0-9]+(?:\.[0-9]+)?)%\)\s+@([0-9]+(?:\.[0-9]+)?)x,\s+remaining\s+([0-9?:]+)\s+RBU\s+([0-9]+(?:\.[0-9]+)?)%\s+UBU\s+([0-9]+(?:\.[0-9]+)?)%)",
                std::regex::icase);

            const OutputCallback parseGrowisofs =
                [this, targetDescription](const std::string_view lineView) {
                    const std::string line(lineView);

                    // RB_RETROBEAM_PHASE_TEXT_V84B
                    std::string backendPhase;
                    if (ContainsCaseInsensitive(line, "lead-in") ||
                        ContainsCaseInsensitive(line, "leadin")) {
                        backendPhase = "Writing Lead-In...";
                    } else if (
                        ContainsCaseInsensitive(line, "starting new track") ||
                        ContainsCaseInsensitive(line, "writing track")) {
                        backendPhase = "Writing Sectors...";
                    } else if (
                        ContainsCaseInsensitive(line, "fixating") ||
                        ContainsCaseInsensitive(line, "closing session") ||
                        ContainsCaseInsensitive(line, "lead-out") ||
                        ContainsCaseInsensitive(line, "leadout")) {
                        backendPhase = "Finalising Disc...";
                    }

                    if (!backendPhase.empty()) {
                        std::lock_guard phaseLock(mutex_);
                        state_.status = std::move(backendPhase);
                    }

                    std::smatch match;
                    if (!std::regex_search(
                            line,
                            match,
                            growProgressPattern)) {
                        return;
                    }

                    const float percent =
                        std::stof(match[1].str());
                    const std::string speed =
                        match[2].str() + "x";
                    const std::string remaining =
                        match[3].str();
                    const int rbu =
                        static_cast<int>(std::lround(
                            std::stod(match[4].str())));
                    const int ubu =
                        static_cast<int>(std::lround(
                            std::stod(match[5].str())));

                    std::lock_guard lock(mutex_);
                    state_.progress = std::clamp(
                        percent / 100.0F,
                        state_.progress,
                        0.999F);
                    state_.actualSpeed = speed;
                    state_.remainingTime = remaining;
                    state_.ringBufferPercent =
                        std::clamp(rbu, 0, 100);
                    state_.driveBufferPercent =
                        std::clamp(ubu, 0, 100);
                    state_.status =
                        "Writing " + targetDescription + " - " +
                        std::to_string(
                            static_cast<int>(std::lround(percent))) +
                        "%";
                };

            const ProcessResult result = RunHiddenProcess(
                growisofs,
                arguments,
                workingDirectory,
                [this](const std::string& text) {
                    AppendLog(text);
                },
                parseGrowisofs);

            if (!result.started) {
                SetFailure(result.error);
                return;
            }
            if (result.exitCode != 0) {
                SetFailure(
                    request.simulate
                        ? targetDescription +
                              " growisofs dry run failed. No image data was intentionally written."
                        : targetDescription +
                              " growisofs write failed. The disc may be incomplete; check the burn log.");
                return;
            }

            if (request.simulate) {
                std::lock_guard lock(mutex_);
                state_.stage = BurnStage::Ready;
                state_.busy = false;
                state_.writing = false;
                state_.progress = xgd3 ? 1.0F : 0.0F;
                state_.status =
                    xgd3
                        ? "XGD3 growisofs preflight passed. Prepared ABGX360 copy cached; Burn will reuse it."
                        : targetDescription +
                              " growisofs dry run completed successfully. No image data was written.";
                return;
            }

            if (xgd3) {
                AppendLog(
                    "\r\nXGD3 growisofs burn completed successfully. "
                    "Removing the cached prepared working copy.\r\n");
                ClearPreparedXgd3();
            }

            const bool ejected =
                EjectOpticalDrive(request.opticalDriveRoot);
            if (!ejected) {
                AppendLog(
                    "\r\nWARNING: Burn succeeded, but Windows could not automatically eject the disc.\r\n");
            }

            PlayBurnCompleteSound();

            std::lock_guard lock(mutex_);
            state_.stage = BurnStage::Complete;
            state_.busy = false;
            state_.writing = false;
            state_.progress = 1.0F;
            state_.session = 1;
            state_.remainingTime = "00:00";
            state_.status = ejected
                ? "Burn complete! Disc ejected. Backend: growisofs."
                : "Burn complete! Backend: growisofs.";
            return;
        }

        // RB_RETROBEAM_UNIFIED_DVD_BACKEND
        // RetroBeam is the default RetroBurner DVD backend. PS2 DVD5/DVD9
        // mode remains automatic from ISO size; Xbox keeps its format layer break.
        {
            const bool ps2Dvd =
                request.target == BurnTarget::PlayStation2Dvd;

            if (request.cdrecordDevice.empty()) {
                SetFailure(
                    "RetroBeam/libscg has no mapped SCSI address for the selected writer. "
                    "Refresh the optical-drive list and try again.");
                return;
            }

            if (!fs::is_regular_file(retrobeam)) {
                SetFailure(
                    "The embedded RetroBeam backend could not be prepared.");
                return;
            }

            bool ps2DualLayer = false;
            std::uint64_t ps2LayerBreak = 0;
            std::uintmax_t ps2ImageBytes = 0;

            if (ps2Dvd) {
                std::error_code ps2SizeError;
                ps2ImageBytes =
                    fs::file_size(burnImagePath, ps2SizeError);

                constexpr std::uintmax_t kDvdRSingleLayerBytes =
                    4707319808ULL;
                constexpr std::uintmax_t kDvdPlusRDualLayerNominalBytes =
                    8547991552ULL;

                if (ps2SizeError ||
                    ps2ImageBytes == 0 ||
                    (ps2ImageBytes % 2048ULL) != 0ULL) {
                    SetFailure(
                        "Could not determine a valid 2048-byte-sector PS2 DVD ISO size.");
                    return;
                }

                if (ps2ImageBytes >
                    kDvdPlusRDualLayerNominalBytes) {
                    SetFailure(
                        "The PS2 DVD ISO is larger than nominal DVD9 capacity.");
                    return;
                }

                ps2DualLayer =
                    ps2ImageBytes > kDvdRSingleLayerBytes;

                if (ps2DualLayer) {
                    const std::uint64_t totalSectors =
                        static_cast<std::uint64_t>(
                            ps2ImageBytes / 2048ULL);

                    // RetroBeam requires a manual DVD DL break to be at least
                    // half the recorded data size and aligned to a 16-sector
                    // DVD ECC block. Round ceil(total/2) upward to 16 sectors.
                    const std::uint64_t minimumLayer0 =
                        (totalSectors + 1ULL) / 2ULL;

                    ps2LayerBreak =
                        (minimumLayer0 + 15ULL) &
                        ~std::uint64_t{15ULL};

                    if (ps2LayerBreak >= totalSectors) {
                        SetFailure(
                            "Could not calculate a valid PS2 DVD9 layer break.");
                        return;
                    }
                }
            }

            AppendLog(
                std::string("\r\n") +
                (ps2Dvd
                    ? "PS2 DVD BACKEND: RetroBeam / libscg\r\n"
                    : "Xbox 360 DVD BACKEND: RetroBeam / libscg\r\n") +
                "RetroBeam device: " +
                request.cdrecordDevice +
                "\r\n");

            if (ps2Dvd) {
                AppendLog(
                    std::string("PS2 DVD mode: ") +
                    (ps2DualLayer ? "DVD9 / dual-layer" : "DVD5 / single-layer") +
                    " (selected automatically from ISO size)\r\n");

                if (ps2DualLayer) {
                    AppendLog(
                        "Calculated PS2 DVD9 layer break: " +
                        std::to_string(ps2LayerBreak) +
                        " sectors\r\n");
                }
            } else {
                AppendLog(
                    "Xbox layer break: " +
                    std::to_string(
                        xgd3 ? 2133520 : 1913760) +
                    "\r\n");
            }

            // Keep no-write/preflight actions truly no-write. In particular,
            // DVD+R does not offer a generally useful dummy-write mode.
            if (request.simulate) {
                AppendLog("Write type: DAO (locked for console DVD media)\r\n");
            AppendLog(RetroBeamAdvancedPolicyText(request));
                AppendLog(
                    "RetroBeam drive/backend preflight complete. "
                    "No media WRITE command was issued and no disc sectors were written.\r\n");

                std::lock_guard lock(mutex_);
                state_.stage = BurnStage::Ready;
                state_.busy = false;
                state_.writing = false;
                state_.progress = 0.0F;
                state_.status =
                    targetDescription +
                    " preflight passed using RetroBeam. "
                    "No disc sectors were written.";
                return;
            }

            {
                std::lock_guard lock(mutex_);
                state_.stage = BurnStage::BurningSession1;
                state_.busy = true;
                state_.writing = true;
                state_.session = 1;
                state_.progress = 0.0F;
                state_.bufferPercent = -1;
                state_.ringBufferPercent = -1;
                state_.driveBufferPercent = -1;
                state_.actualSpeed.clear();
                state_.remainingTime.clear();
                state_.status =
                    "Writing " + targetDescription +
                    " with RetroBeam...";
            }

            std::vector<std::wstring> retrobeamArguments;
            retrobeamArguments.push_back(
                L"dev=" + std::wstring(
                    request.cdrecordDevice.begin(),
                    request.cdrecordDevice.end()));
            retrobeamArguments.emplace_back(L"-v");
            retrobeamArguments.emplace_back(L"-dao");

            if (request.requestedSpeedX > 0) {
                retrobeamArguments.push_back(
                    L"speed=" +
                    std::to_wstring(request.requestedSpeedX));
            }

            retrobeamArguments.emplace_back(L"fs=32m");

            const std::uint64_t selectedLayerBreak =
                xbox
                    ? static_cast<std::uint64_t>(xgd3 ? 2133520 : 1913760)
                    : (ps2DualLayer ? ps2LayerBreak : 0ULL);

            retrobeamArguments.push_back(
                JoinRetroBeamDriverOptions(request, selectedLayerBreak));

            retrobeamArguments.emplace_back(L"-data");
            retrobeamArguments.push_back(
                burnImagePath.wstring());

            AppendLog(RetroBeamAdvancedPolicyText(request));
            AppendLog(
                ps2Dvd
                    ? (ps2DualLayer
                        ? "Starting RetroBeam PS2 DVD9 write...\r\n"
                        : "Starting RetroBeam PS2 DVD5 write...\r\n")
                    : "Starting RetroBeam Xbox 360 DVD+R DL write...\r\n");

            static const std::regex cdrecordProgressPattern(
                R"(Track\s+(\d+):\s+([0-9]+(?:\.[0-9]+)?)\s+of\s+([0-9]+(?:\.[0-9]+)?)\s+([kMGT]?B)\s+written)",
                std::regex::icase);
            static const std::regex cdrecordFifoPattern(

                R"(\(fifo\s*([0-9]+)%\))",

                std::regex::icase);

            static const std::regex cdrecordBufferPattern(
                R"(\[buf\s*([0-9]+)%\])");
            static const std::regex cdrecordSpeedPattern(
                R"(([0-9]+(?:\.[0-9]+)?)x)");
            static const std::regex cdrecordStartSpeedPattern(
                R"(Starting to write.*speed\s+([0-9]+(?:\.[0-9]+)?))",
                std::regex::icase);

            const auto appendRetroBeam =
                [this](const std::string& outputText) {
                    AppendLog(outputText);
                };

            const OutputCallback parseRetroBeam =
                [this, targetDescription](const std::string_view lineView) {
                    const std::string line(lineView);

                    // RB_RETROBEAM_PHASE_TEXT_V84B
                    std::string backendPhase;
                    if (ContainsCaseInsensitive(line, "lead-in") ||
                        ContainsCaseInsensitive(line, "leadin")) {
                        backendPhase = "Writing Lead-In...";
                    } else if (
                        ContainsCaseInsensitive(line, "starting new track") ||
                        ContainsCaseInsensitive(line, "writing track")) {
                        backendPhase = "Writing Sectors...";
                    } else if (
                        ContainsCaseInsensitive(line, "fixating") ||
                        ContainsCaseInsensitive(line, "closing session") ||
                        ContainsCaseInsensitive(line, "lead-out") ||
                        ContainsCaseInsensitive(line, "leadout")) {
                        backendPhase = "Finalising Disc...";
                    }

                    if (!backendPhase.empty()) {
                        std::lock_guard phaseLock(mutex_);
                        state_.status = std::move(backendPhase);
                    }

                    std::smatch match;
                    float progress = -1.0F;
                    int fifoPercent = -1;
                    int bufferPercent = -1;
                    std::string speed;

                    if (std::regex_search(
                            line,
                            match,
                            cdrecordProgressPattern)) {
                        const double written =
                            std::stod(match[2].str()) *
                            UnitMultiplier(match[4].str());
                        const double total =
                            std::stod(match[3].str()) *
                            UnitMultiplier(match[4].str());

                        if (total > 0.0) {
                            progress =
                                static_cast<float>(
                                    std::clamp(
                                        written / total,
                                        0.0,
                                        0.999));
                        }
                    }

                    if (std::regex_search(


                            line,


                            match,


                            cdrecordFifoPattern)) {


                        fifoPercent =


                            std::stoi(match[1].str());


                    }


                    if (std::regex_search(
                            line,
                            match,
                            cdrecordBufferPattern)) {
                        bufferPercent =
                            std::stoi(match[1].str());
                    }

                    if (std::regex_search(
                            line,
                            match,
                            cdrecordStartSpeedPattern)) {
                        speed = match[1].str() + "x";
                    } else if (std::regex_search(
                                   line,
                                   match,
                                   cdrecordSpeedPattern)) {
                        speed = match[1].str() + "x";
                    }

                    if (progress >= 0.0F ||
                        fifoPercent >= 0 ||
                        bufferPercent >= 0 ||
                        !speed.empty()) {
                        std::lock_guard lock(mutex_);

                        if (progress >= 0.0F) {
                            state_.progress =
                                std::max(
                                    state_.progress,
                                    progress);
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

                        if (bufferPercent >= 0) {


                            const int clampedBuffer =


                                std::clamp(bufferPercent, 0, 100);


                            state_.bufferPercent = clampedBuffer;


                            state_.driveBufferPercent = clampedBuffer;


                        }

                        if (fifoPercent >= 0) {


                            state_.ringBufferPercent =


                                std::clamp(fifoPercent, 0, 100);


                        }


                        if (!speed.empty()) {
                            state_.actualSpeed =
                                std::move(speed);
                        }
                    }
                };

            const ProcessResult retrobeamResult =
                RunHiddenProcess(
                    retrobeam,
                    retrobeamArguments,
                    workingDirectory,
                    appendRetroBeam,
                    parseRetroBeam);

            if (request.advanced.useStreamingPolicy &&
                request.advanced.restoreStreamingDefaults) {
                AppendLog("\r\nRestoring RetroBeam MMC streaming defaults...\r\n");
                const ProcessResult restoreResult = RunHiddenProcess(
                    retrobeam,
                    {
                        L"dev=" + std::wstring(
                            request.cdrecordDevice.begin(),
                            request.cdrecordDevice.end()),
                        L"driveropts=streamrestore",
                        L"-setdropts",
                    },
                    workingDirectory,
                    appendRetroBeam,
                    [](std::string_view) {});
                if (!restoreResult.started || restoreResult.exitCode != 0) {
                    AppendLog(
                        "WARNING: RetroBeam could not restore MMC streaming defaults. "
                        "Power-cycling the drive will clear volatile settings.\r\n");
                }
            }

            if (!retrobeamResult.started) {
                SetFailure(retrobeamResult.error);
                return;
            }

            if (retrobeamResult.exitCode != 0) {
                SetFailure(
                    targetDescription +
                    " RetroBeam write failed. "
                    "The disc may be incomplete; check the burn log.");
                return;
            }

            if (xgd3) {
                AppendLog(
                    "\r\nXGD3 burn completed successfully. "
                    "Removing the cached prepared working copy.\r\n");
                ClearPreparedXgd3();
            }

            const bool ejected =
                EjectOpticalDrive(
                    request.opticalDriveRoot);

            if (!ejected) {
                AppendLog(
                    "\r\nWARNING: Burn succeeded, but Windows could not automatically eject the disc.\r\n");
            }

            PlayBurnCompleteSound();

            std::lock_guard lock(mutex_);
            state_.stage = BurnStage::Complete;
            state_.busy = false;
            state_.writing = false;
            state_.progress = 1.0F;
            state_.session = 1;
            state_.remainingTime = "00:00";
            state_.status = ejected
                ? "Burn complete! Disc ejected. Backend: RetroBeam."
                : "Burn complete! Backend: RetroBeam.";
            return;
        }
    }

    const auto append =
        [this](const std::string& text) {
            AppendLog(text);
        };
    const auto ignoreLine =
        [](std::string_view) {};

    AppendLog("\r\nRead-only media preflight\r\n");
    const std::vector<std::wstring> preflightArguments = {
        L"dev=" + std::wstring(
            request.cdrecordDevice.begin(),
            request.cdrecordDevice.end()),
        L"-atip",
    };

    const ProcessResult preflight = RunHiddenProcess(
        retrobeam,
        preflightArguments,
        workingDirectory,
        append,
        ignoreLine);

    if (!preflight.started) {
        SetFailure(preflight.error);
        return;
    }
    if (preflight.exitCode != 0) {
        SetFailure(
            "The selected burner or inserted blank media failed preflight. "
            "No image data was written.");
        return;
    }

    {
        std::lock_guard lock(mutex_);
        state_.stage = BurnStage::BurningSession1;
        state_.busy = true;
        state_.writing = true;
        state_.session = 1;
        state_.bufferPercent = -1;
        state_.ringBufferPercent = -1;
        state_.driveBufferPercent = -1;
        state_.remainingTime.clear();
        state_.actualSpeed.clear();
        state_.progress = 0.0F;
        // RB_STAGE44S_WINDOWS_CD_PHASE_BUFFER_PARITY
        // Match the Linux CD lifecycle from the moment RetroBeam starts.
        state_.status = "Writing Lead-In...";
    }

    AppendLog("\r\nWriting disc\r\n");

    std::vector<std::wstring> arguments;
    arguments.push_back(
        L"dev=" + std::wstring(
            request.cdrecordDevice.begin(),
            request.cdrecordDevice.end()));
    arguments.emplace_back(L"-v");

    if (request.requestedSpeedX > 0) {
        arguments.push_back(
            L"speed=" +
            std::to_wstring(request.requestedSpeedX));
    }

    arguments.push_back(JoinRetroBeamDriverOptions(request));
    AppendLog(RetroBeamAdvancedPolicyText(request));

    if (!request.verifyAfterBurn) {
        arguments.emplace_back(L"-eject");
    }

    const std::string extension =
        Lowercase(imagePath.extension().string());

    // RB_STAGE44L_PS2CD_TAO_XA
    //
    // A single cooked 2048-byte PS2-CD ISO still needs CD-ROM XA Mode 2
    // Form 1, but TAO avoids the DAO SEND CUE SHEET command rejected by
    // some otherwise functional writers. CUE/BIN layouts keep DAO.
    const bool ps2CdIso =
        request.target ==
            BurnTarget::PlayStation2Cd &&
        extension == ".iso";

    arguments.emplace_back(
        ps2CdIso
            ? L"-tao"
            : L"-dao");

    if (extension == ".cue") {
        arguments.push_back(
            L"cuefile=" + imagePath.wstring());
    } else if (
        request.target == BurnTarget::PlayStation2Cd &&
        extension == ".iso") {
        // RB_STAGE44K_PS2CD_XA_MODE
        //
        // A 2048-byte PS2 CD ISO contains user data for CD-ROM XA
        // Mode 2 Form 1 sectors. Let RetroBeam/cdrecord create the XA
        // subheaders/ECC instead of writing an ordinary Mode-1 data track.
        arguments.emplace_back(L"-xa");
        arguments.push_back(imagePath.wstring());
    } else {
        arguments.emplace_back(L"-data");
        arguments.push_back(imagePath.wstring());
    }

    if (ps2CdIso) {
        AppendLog(
            "\r\nPS2 CD ISO recording mode: "
            "TAO / CD-ROM XA Mode 2 Form 1\r\n");
        AppendLog(
            std::string("Post-burn verification: ") +
            (request.verifyAfterBurn ? "ON" : "OFF") +
            "\r\n");
    }

    static const std::regex progressPattern(
        R"(Track\s+(\d+):\s+([0-9]+(?:\.[0-9]+)?)\s+of\s+([0-9]+(?:\.[0-9]+)?)\s+([kMGT]?B)\s+written)",
        std::regex::icase);
    static const std::regex fifoPattern(
        R"(\(fifo\s*([0-9]+)%\))",
        std::regex::icase);
    static const std::regex bufferPattern(
        R"(\[buf\s*([0-9]+)%\])",
        std::regex::icase);
    static const std::regex speedPattern(
        R"(([0-9]+(?:\.[0-9]+)?)x)");
    static const std::regex startSpeedPattern(
        R"(Starting to write.*speed\s+([0-9]+(?:\.[0-9]+)?))",
        std::regex::icase);

    const int totalTracks = std::max(1, cueTrackCount);
    const std::string targetName = BurnTargetName(request.target);

    const OutputCallback parseLine =
        [this, totalTracks, targetName](const std::string_view lineView) {
            const std::string line(lineView);
            std::smatch match;
            float progress = -1.0F;
            int fifoPercent = -1;
            int bufferPercent = -1;
            std::string speed;
            std::string backendPhase;

            // Keep Windows visually identical to the Linux RetroBeam
            // lifecycle. Setup/OPC/pregap are lead-in, track progress is
            // sector writing, and fixation/session close is finalisation.
            if (ContainsCaseInsensitive(line, "lead-in") ||
                ContainsCaseInsensitive(line, "leadin") ||
                ContainsCaseInsensitive(line, "starting real sao write") ||
                ContainsCaseInsensitive(line, "waiting for reader process") ||
                ContainsCaseInsensitive(line, "performing opc") ||
                ContainsCaseInsensitive(line, "sending cue sheet") ||
                ContainsCaseInsensitive(line, "writing pregap")) {
                backendPhase = "Writing Lead-In...";
            } else if (
                ContainsCaseInsensitive(line, "starting new track") ||
                ContainsCaseInsensitive(line, "writing track")) {
                backendPhase = "Writing Sectors...";
            } else if (
                ContainsCaseInsensitive(line, "fixating") ||
                ContainsCaseInsensitive(line, "closing session") ||
                ContainsCaseInsensitive(line, "lead-out") ||
                ContainsCaseInsensitive(line, "leadout")) {
                backendPhase = "Finalising Disc...";
            }

            if (std::regex_search(
                    line,
                    match,
                    progressPattern)) {
                const int trackNumber =
                    std::clamp(
                        std::stoi(match[1].str()),
                        1,
                        totalTracks);
                const double written =
                    std::stod(match[2].str()) *
                    UnitMultiplier(match[4].str());
                const double total =
                    std::stod(match[3].str()) *
                    UnitMultiplier(match[4].str());
                const double trackFraction =
                    total > 0.0
                        ? std::clamp(
                            written / total,
                            0.0,
                            1.0)
                        : 0.0;

                progress = static_cast<float>(
                    std::clamp(
                        (static_cast<double>(trackNumber - 1) +
                         trackFraction) /
                            static_cast<double>(totalTracks),
                        0.0,
                        0.999));

                // Progress records are the sector-writing phase even when
                // RetroBeam no longer repeats "Starting new track".
                if (backendPhase.empty()) {
                    backendPhase = "Writing Sectors...";
                }
            }

            if (std::regex_search(
                    line,
                    match,
                    fifoPattern)) {
                fifoPercent =
                    std::stoi(match[1].str());
            }

            if (std::regex_search(
                    line,
                    match,
                    bufferPattern)) {
                bufferPercent =
                    std::stoi(match[1].str());
            }

            if (std::regex_search(
                    line,
                    match,
                    startSpeedPattern)) {
                speed = match[1].str() + "x";
            } else if (std::regex_search(
                           line,
                           match,
                           speedPattern)) {
                speed = match[1].str() + "x";
            }

            if (progress >= 0.0F ||
                fifoPercent >= 0 ||
                bufferPercent >= 0 ||
                !speed.empty() ||
                !backendPhase.empty()) {
                std::lock_guard lock(mutex_);

                if (progress >= 0.0F) {
                    state_.progress =
                        std::max(
                            state_.progress,
                            progress);
                }

                if (!backendPhase.empty()) {
                    state_.status =
                        std::move(backendPhase);
                }

                if (fifoPercent >= 0) {
                    state_.ringBufferPercent =
                        std::clamp(
                            fifoPercent,
                            0,
                            100);
                }

                if (bufferPercent >= 0) {
                    const int clampedBuffer =
                        std::clamp(
                            bufferPercent,
                            0,
                            100);

                    // bufferPercent remains populated for the compact text
                    // metric; driveBufferPercent feeds the graphical bar.
                    state_.bufferPercent =
                        clampedBuffer;
                    state_.driveBufferPercent =
                        clampedBuffer;
                }

                if (!speed.empty()) {
                    state_.actualSpeed =
                        std::move(speed);
                }
            }
        };

    // RB_STAGE44L_FAILURE_STATUS
    std::string retroBeamBurnOutput;

    const auto appendBurnOutput =
        [this,
         &retroBeamBurnOutput](
            const std::string& text) {
            retroBeamBurnOutput +=
                text;

            if (retroBeamBurnOutput.size() >
                65536U) {
                retroBeamBurnOutput.erase(
                    0,
                    retroBeamBurnOutput.size() -
                        65536U);
            }

            AppendLog(text);
        };

    const ProcessResult result = RunHiddenProcess(
        retrobeam,
        arguments,
        workingDirectory,
        appendBurnOutput,
        parseLine);

    if (!result.started) {
        SetFailure(result.error);
        return;
    }

    if (result.exitCode != 0) {
        const std::string detail =
            SummarizeRetroBeamFailure(
                retroBeamBurnOutput);

        SetFailure(
            detail.empty()
                ? std::string(
                      BurnTargetName(
                          request.target)) +
                      " RetroBeam write failed. See Burn Log for full backend output."
                : "RetroBeam burn failed: " +
                      detail);
        return;
    }


    // Explicit optional verification. Unchecked requests never enter here.
    if (request.verifyAfterBurn &&
        request.target ==
            BurnTarget::PlayStation2Cd &&
        extension == ".iso") {
        {
            std::lock_guard lock(mutex_);

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

        Sleep(500);

        const WindowsOpticalVerifyResult verification =
            VerifyWindowsOpticalSectors(
                request.opticalDriveRoot,
                imagePath,
                [this](
                    const std::uint32_t completed,
                    const std::uint32_t total) {
                    const unsigned percent =
                        total == 0
                            ? 100U
                            : static_cast<unsigned>(
                                  std::min<std::uint64_t>(
                                      100ULL,
                                      (static_cast<std::uint64_t>(
                                           completed) *
                                       100ULL) /
                                          total));

                    std::lock_guard lock(mutex_);

                    state_.writing = false;
                    state_.progress = 0.999F;
                    state_.status =
                        "Verifying Disc... " +
                        std::to_string(percent) +
                        "%";
                });

        AppendLog(
            "\r\n===== Windows optical verification =====\r\n"
            "source=" +
            imagePath.string() +
            "\r\nsector_count=" +
            std::to_string(
                verification.sectorCount) +
            "\r\nbytes_compared=" +
            std::to_string(
                verification.bytesCompared) +
            "\r\nverify_success=" +
            std::string(
                verification.success
                    ? "yes"
                    : "no") +
            "\r\n");

        if (!verification.success) {
            SetFailure(
                "Post-burn verification failed: " +
                verification.error);
            return;
        }
    }

    PlayBurnCompleteSound();

    std::lock_guard lock(mutex_);
    state_.busy = false;
    state_.writing = false;
    state_.stage = BurnStage::Complete;
    state_.progress = 1.0F;
    state_.session = 1;
    state_.status = "Burn complete! Disc ejected.";
}
void BurnEngine::Run(BurnRequest request) {
    const EmbeddedToolPaths& tools = GetEmbeddedToolPaths();
    if (!tools.Ready()) {
        SetFailure("The embedded recording backend could not be prepared: " + tools.error);
        return;
    }

    const fs::path appDirectory = tools.directory;
    const fs::path cdirip = tools.cdirip;
    const fs::path retrobeam = tools.retrobeam;
    const fs::path growisofs = tools.growisofs;
    const fs::path dvdMediaInfo = tools.dvdMediaInfo;
    const fs::path abgx360 = tools.abgx360;

    if (request.burnerMaxOnly) {
        RunBurnerMaxOnly(
            std::move(request),
            dvdMediaInfo.wstring());
        return;
    }

    if (request.target != BurnTarget::Dreamcast) {
        RunStandardImage(
            std::move(request),
            retrobeam.wstring(),
            growisofs.wstring(),
            dvdMediaInfo.wstring(),
            abgx360.wstring());
        return;
    }
    TemporaryDirectory temporary = MakeTemporaryDirectory();
    if (temporary.path.empty()) {
        SetFailure("Windows could not create a temporary extraction directory.");
        return;
    }

    std::error_code fileError;
    const std::uintmax_t cdiSize = fs::file_size(request.cdiPath, fileError);
    ULARGE_INTEGER freeBytes{};
    if (!fileError && GetDiskFreeSpaceExW(
            temporary.path.c_str(), &freeBytes, nullptr, nullptr) &&
        freeBytes.QuadPart < cdiSize + kMinimumFreeSpaceMargin) {
        SetFailure("Not enough free temporary disk space to extract this CDI.");
        return;
    }

    const auto append = [this](const std::string& text) { AppendLog(text); };
    const auto ignoreLine = [](std::string_view) {};

    if (!request.checkOnly) {
        AppendLog("Retro Burner - read-only media preflight\r\n");
        const std::vector<std::wstring> preflightArguments = {
            L"dev=" + std::wstring(
                request.cdrecordDevice.begin(), request.cdrecordDevice.end()),
            L"-atip",
        };
        const ProcessResult preflight = RunHiddenProcess(
            retrobeam,
            preflightArguments,
            appDirectory,
            append,
            ignoreLine);
        if (!preflight.started) {
            SetFailure(preflight.error);
            return;
        }
        if (preflight.exitCode != 0) {
            SetFailure(
                "The selected burner or blank CD-R failed the read-only preflight. "
                "No data was written; open Burn log for RetroBeam's reason.");
            return;
        }
    }

    {
        std::lock_guard lock(mutex_);
        state_.status = "Extracting and analysing CDI tracks...";
    }
    AppendLog("\r\nCDIrip extraction\r\n");
    const std::vector<std::wstring> extractArguments = {
        request.cdiPath,
        temporary.path.wstring(),
        // Match DCDIB's all-layout extraction behaviour on Windows:
        // force data tracks to ISO, but do NOT use CDIrip's -cdrecord
        // preset because that also enables -cutall and trims audio tracks.
        L"-iso",
    };
    const ProcessResult extraction = RunHiddenProcess(
        cdirip,
        extractArguments,
        temporary.path,
        append,
        ignoreLine);
    if (!extraction.started) {
        SetFailure(extraction.error);
        return;
    }
    if (extraction.exitCode != 0) {
        SetFailure("CDIrip could not extract this image. Open Burn log for details.");
        return;
    }

    ExtractedLayout layout;
    std::string layoutError;
    if (!DetectLayout(temporary.path, layout, layoutError)) {
        SetFailure(std::move(layoutError));
        return;
    }

    {
        std::lock_guard lock(mutex_);
        state_.layout = layout.description;
    }
    AppendLog("\r\nDetected: " + layout.description + "\r\n");
    if (request.checkOnly) {
        std::lock_guard lock(mutex_);
        state_.stage = BurnStage::Ready;
        state_.busy = false;
        state_.status = layout.description + ". Check complete; no disc was written.";
        return;
    }

    const std::uintmax_t totalBytes =
        layout.firstSessionBytes + layout.secondSessionBytes;

    const auto runSession = [this, &retrobeam, &temporary, &request, totalBytes](
                                const int sessionNumber,
                                const bool audioSession,
                                const std::vector<fs::path>& tracks,
                                const std::uintmax_t priorSessionBytes) -> bool {
        const std::vector<std::uintmax_t> trackSizes = FileSizes(tracks);
        std::vector<std::uintmax_t> priorTrackBytes(trackSizes.size(), 0);
        for (std::size_t index = 1; index < trackSizes.size(); ++index) {
            priorTrackBytes[index] = priorTrackBytes[index - 1] + trackSizes[index - 1];
        }

        {
            std::lock_guard lock(mutex_);
            state_.stage = sessionNumber == 1
                ? BurnStage::BurningSession1
                : BurnStage::BurningSession2;
            state_.busy = true;
            state_.writing = true;
            state_.session = sessionNumber;
            state_.bufferPercent = -1;
            state_.actualSpeed.clear();
            state_.status = "Burning session " + std::to_string(sessionNumber) + " of 2";
        }

        AppendLog("\r\nBurning session " + std::to_string(sessionNumber) + " of 2\r\n");
        std::vector<std::wstring> arguments;
        arguments.push_back(
            L"dev=" + std::wstring(
                request.cdrecordDevice.begin(), request.cdrecordDevice.end()));
        arguments.emplace_back(L"-v");
        if (request.requestedSpeedX > 0) {
            arguments.push_back(L"speed=" + std::to_wstring(request.requestedSpeedX));
        }
        arguments.push_back(JoinRetroBeamDriverOptions(request));
        AppendLog(RetroBeamAdvancedPolicyText(request));
        if (sessionNumber == 1) {
            arguments.emplace_back(audioSession ? L"-dao" : L"-tao");
            arguments.emplace_back(L"-multi");
            if (!audioSession) {
                arguments.emplace_back(L"-xa");
            }
        } else {
            arguments.emplace_back(L"-eject");
            arguments.emplace_back(L"-overburn");
            arguments.emplace_back(L"-tao");
            arguments.emplace_back(L"-xa");
        }
        for (const fs::path& track : tracks) {
            arguments.push_back(track.filename().wstring());
        }

        static const std::regex progressPattern(
            R"(Track\s+(\d+):\s+([0-9]+(?:\.[0-9]+)?)\s+of\s+([0-9]+(?:\.[0-9]+)?)\s+([kMGT]?B)\s+written)",
            std::regex::icase);
        static const std::regex bufferPattern(R"(\[buf\s*([0-9]+)%\])");
        static const std::regex speedPattern(R"(([0-9]+(?:\.[0-9]+)?)x)");
        static const std::regex startSpeedPattern(
            R"(Starting to write.*speed\s+([0-9]+(?:\.[0-9]+)?))",
            std::regex::icase);

        const OutputCallback parseLine =
            [this, &trackSizes, &priorTrackBytes, priorSessionBytes, totalBytes](
                const std::string_view lineView) {
                const std::string line(lineView);
                std::smatch match;
                float overallProgress = -1.0F;
                int bufferPercent = -1;
                std::string speed;

                if (std::regex_search(line, match, progressPattern)) {
                    const int trackNumber = std::max(1, std::stoi(match[1].str()));
                    const std::size_t trackIndex = std::min<std::size_t>(
                        static_cast<std::size_t>(trackNumber - 1),
                        trackSizes.empty() ? 0U : trackSizes.size() - 1U);
                    const double written = std::stod(match[2].str()) *
                        UnitMultiplier(match[4].str());
                    const double reportedTotal = std::stod(match[3].str()) *
                        UnitMultiplier(match[4].str());
                    const double fraction = reportedTotal > 0.0
                        ? std::clamp(written / reportedTotal, 0.0, 1.0)
                        : 0.0;
                    const double completed = static_cast<double>(priorSessionBytes) +
                        static_cast<double>(priorTrackBytes[trackIndex]) +
                        fraction * static_cast<double>(trackSizes[trackIndex]);
                    overallProgress = static_cast<float>(std::clamp(
                        completed / static_cast<double>(totalBytes), 0.0, 0.999));
                }
                if (std::regex_search(line, match, bufferPattern)) {
                    bufferPercent = std::stoi(match[1].str());
                }
                if (std::regex_search(line, match, startSpeedPattern)) {
                    speed = match[1].str() + "x";
                } else if (std::regex_search(line, match, speedPattern)) {
                    speed = match[1].str() + "x";
                }

                if (overallProgress >= 0.0F || bufferPercent >= 0 || !speed.empty()) {
                    std::lock_guard lock(mutex_);
                    if (overallProgress >= 0.0F) {
                        state_.progress = std::max(state_.progress, overallProgress);
                    }
                    if (bufferPercent >= 0) {
                        state_.bufferPercent = bufferPercent;
                    }
                    if (!speed.empty()) {
                        state_.actualSpeed = std::move(speed);
                    }
                }
            };

        const ProcessResult result = RunHiddenProcess(
            retrobeam,
            arguments,
            temporary.path,
            [this](const std::string& text) { AppendLog(text); },
            parseLine);
        if (!result.started) {
            SetFailure(result.error);
            return false;
        }
        if (result.exitCode != 0) {
            SetFailure(
                "Session " + std::to_string(sessionNumber) +
                " failed. The disc is incomplete and cannot be reused; open Burn log for details.");
            return false;
        }
        return true;
    };

    if (!runSession(1, layout.audioData, layout.firstSession, 0)) {
        return;
    }
    if (!runSession(
            2,
            false,
            layout.secondSession,
            layout.firstSessionBytes)) {
        return;
    }

    AppendLog("\r\nBurn complete.\r\n");
    PlayBurnCompleteSound();

    std::lock_guard lock(mutex_);
    state_.stage = BurnStage::Complete;
    state_.busy = false;
    state_.writing = false;
    state_.progress = 1.0F;
    state_.session = 2;
    state_.status = "Burn complete! Disc ejected.";
}
