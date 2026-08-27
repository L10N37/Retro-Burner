#include "burnermax.h"

#include <fcntl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::uint16_t kRegisterBase = 0x8000;
constexpr std::uint16_t kRegisterEnd = 0x9000;
constexpr std::uint32_t kXgd3LayerBoundary = 2133520;
constexpr std::size_t kRegisterCount =
    static_cast<std::size_t>(kRegisterEnd - kRegisterBase);
constexpr unsigned kScsiTimeoutMs = 15000;

using RegisterDump = std::array<std::uint8_t, kRegisterCount>;

enum class VendorFamily {
    F1,
    Df,
};

struct FileDescriptor final {
    int value = -1;

    explicit FileDescriptor(const int fd = -1) noexcept
        : value(fd) {}

    ~FileDescriptor() {
        if (value >= 0)
            ::close(value);
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
};

[[nodiscard]] std::string HexValue(
    const std::uint32_t value,
    const int width = 4)
{
    std::ostringstream stream;
    stream
        << "0x"
        << std::uppercase
        << std::hex
        << std::setfill('0')
        << std::setw(width)
        << value;
    return stream.str();
}

void Log(
    const BurnerMaxLogCallback& callback,
    const std::string& text)
{
    if (callback)
        callback(text);
}

[[nodiscard]] bool ValidateExactOpticalSg(
    const std::string& device,
    std::string& error)
{
    const fs::path path(device);

    if (path.parent_path() != "/dev") {
        error = "BurnerMAX requires an exact /dev/sgX path";
        return false;
    }

    const std::string name = path.filename().string();

    if (name.size() < 3 ||
        name.rfind("sg", 0) != 0 ||
        !std::all_of(
            name.begin() + 2,
            name.end(),
            [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
        error = "BurnerMAX requires an exact /dev/sgX path";
        return false;
    }

    std::ifstream typeFile(
        fs::path("/sys/class/scsi_generic") /
        name /
        "device/type");

    int type = -1;
    typeFile >> type;

    if (!typeFile || type != 5) {
        error =
            "Refusing BurnerMAX: selected SG node is not a type-5 optical device";
        return false;
    }

    if (::access(path.c_str(), R_OK | W_OK) != 0) {
        error =
            "Current desktop session does not have read/write SG_IO access";
        return false;
    }

    return true;
}

[[nodiscard]] std::string SenseDescription(
    const std::array<unsigned char, 64>& sense,
    const unsigned length)
{
    if (length < 14)
        return "sense unavailable";

    return
        "sense " +
        HexValue(sense[2] & 0x0F, 2) +
        "/" +
        HexValue(sense[12], 2) +
        "/" +
        HexValue(sense[13], 2);
}

[[nodiscard]] bool SendScsi(
    const int drive,
    const std::array<unsigned char, 12>& cdb,
    const int direction,
    void* const data,
    const unsigned dataLength,
    std::string& error)
{
    std::array<unsigned char, 64> sense{};

    sg_io_hdr_t io{};
    io.interface_id = 'S';
    io.dxfer_direction = direction;
    io.cmd_len =
        static_cast<unsigned char>(cdb.size());
    io.mx_sb_len =
        static_cast<unsigned char>(sense.size());
    io.dxfer_len = dataLength;
    io.dxferp = data;
    io.cmdp =
        const_cast<unsigned char*>(cdb.data());
    io.sbp = sense.data();
    io.timeout = kScsiTimeoutMs;

    if (::ioctl(drive, SG_IO, &io) < 0) {
        error =
            std::string("SG_IO ioctl failed: ") +
            std::strerror(errno);
        return false;
    }

    if ((io.info & SG_INFO_OK_MASK) != SG_INFO_OK ||
        io.status != 0 ||
        io.host_status != 0 ||
        io.driver_status != 0 ||
        io.resid != 0) {
        std::ostringstream text;
        text
            << "SCSI command failed: status="
            << HexValue(io.status, 2)
            << " host="
            << HexValue(io.host_status, 2)
            << " driver="
            << HexValue(io.driver_status, 2)
            << " resid="
            << io.resid;

        if (io.sb_len_wr != 0) {
            text
                << " "
                << SenseDescription(
                    sense,
                    io.sb_len_wr);
        }

        error = text.str();
        return false;
    }

    return true;
}

[[nodiscard]] bool ReadLayerBoundary(
    const int drive,
    std::uint32_t& boundary,
    std::string& error)
{
    std::array<unsigned char, 12> data{};
    std::array<unsigned char, 12> cdb{};

    cdb[0] = 0xAD;
    cdb[7] = 0x20;
    cdb[9] =
        static_cast<unsigned char>(data.size());

    if (!SendScsi(
            drive,
            cdb,
            SG_DXFER_FROM_DEV,
            data.data(),
            static_cast<unsigned>(data.size()),
            error)) {
        return false;
    }

    boundary =
        (static_cast<std::uint32_t>(data[9]) << 16U) |
        (static_cast<std::uint32_t>(data[10]) << 8U) |
        static_cast<std::uint32_t>(data[11]);

    return true;
}

[[nodiscard]] bool ReadRegisterF1(
    const int drive,
    const std::uint16_t address,
    std::uint8_t& value,
    std::string& error)
{
    std::array<unsigned char, 4> data{};
    std::array<unsigned char, 12> cdb{};

    cdb[0] = 0xF1;
    cdb[1] = 0x02;
    cdb[4] =
        static_cast<unsigned char>(address >> 8U);
    cdb[5] =
        static_cast<unsigned char>(address & 0xFFU);
    cdb[6] = 0x01;

    if (!SendScsi(
            drive,
            cdb,
            SG_DXFER_FROM_DEV,
            data.data(),
            static_cast<unsigned>(data.size()),
            error)) {
        return false;
    }

    value = data[3];
    return true;
}

[[nodiscard]] bool WriteRegisterF1(
    const int drive,
    const std::uint16_t address,
    const std::uint8_t value,
    std::string& error)
{
    std::array<unsigned char, 12> cdb{};

    cdb[0] = 0xF1;
    cdb[1] = 0x01;
    cdb[4] =
        static_cast<unsigned char>(address >> 8U);
    cdb[5] =
        static_cast<unsigned char>(address & 0xFFU);
    cdb[9] = value;

    return SendScsi(
        drive,
        cdb,
        SG_DXFER_NONE,
        nullptr,
        0,
        error);
}

[[nodiscard]] bool ReadRegistersF1(
    const int drive,
    RegisterDump& dump,
    const BurnerMaxLogCallback& log,
    std::string& error)
{
    Log(
        log,
        "BurnerMAX: trying MTK F1 register access...\n");

    for (std::uint32_t address = kRegisterBase;
         address < kRegisterEnd;
         ++address) {
        std::uint8_t value = 0;

        if (!ReadRegisterF1(
                drive,
                static_cast<std::uint16_t>(address),
                value,
                error)) {
            return false;
        }

        dump[
            static_cast<std::size_t>(
                address - kRegisterBase)] =
            value;

        if (((address - kRegisterBase + 1U) % 512U) == 0U) {
            Log(
                log,
                "BurnerMAX: F1 scan " +
                std::to_string(
                    address - kRegisterBase + 1U) +
                "/4096 registers\n");
        }
    }

    return true;
}

[[nodiscard]] bool ReadRegistersDf(
    const int drive,
    RegisterDump& dump,
    const BurnerMaxLogCallback& log,
    std::string& error)
{
    Log(
        log,
        "BurnerMAX: trying MTK DF block register access...\n");

    constexpr std::uint32_t blockSize = 128;

    for (std::uint32_t address = kRegisterBase;
         address < kRegisterEnd;
         address += blockSize) {
        std::array<unsigned char, blockSize> data{};
        std::array<unsigned char, 12> cdb{};

        cdb[0] = 0xDF;
        cdb[1] = 0x85;
        cdb[3] = 0xFF;
        cdb[7] =
            static_cast<unsigned char>(address >> 8U);
        cdb[8] =
            static_cast<unsigned char>(address & 0xFFU);

        if (!SendScsi(
                drive,
                cdb,
                SG_DXFER_FROM_DEV,
                data.data(),
                static_cast<unsigned>(data.size()),
                error)) {
            return false;
        }

        const std::size_t offset =
            static_cast<std::size_t>(
                address - kRegisterBase);

        std::copy(
            data.begin(),
            data.end(),
            dump.begin() +
                static_cast<std::ptrdiff_t>(offset));
    }

    return true;
}

[[nodiscard]] bool WriteRegisterDf(
    const int drive,
    const std::uint16_t address,
    const std::uint8_t value,
    std::string& error)
{
    std::array<unsigned char, 12> cdb{};

    cdb[0] = 0xDF;
    cdb[1] = 0x84;
    cdb[4] = 0x01;
    cdb[7] =
        static_cast<unsigned char>(address >> 8U);
    cdb[8] =
        static_cast<unsigned char>(address & 0xFFU);
    cdb[9] = value;

    return SendScsi(
        drive,
        cdb,
        SG_DXFER_NONE,
        nullptr,
        0,
        error);
}

[[nodiscard]] std::vector<std::uint16_t> FindPattern(
    const RegisterDump& dump,
    const std::array<std::uint8_t, 3>& pattern)
{
    std::vector<std::uint16_t> result;

    for (std::size_t index = 0;
         index + pattern.size() <= dump.size();
         ++index) {
        if (dump[index] == pattern[0] &&
            dump[index + 1] == pattern[1] &&
            dump[index + 2] == pattern[2]) {
            result.push_back(
                static_cast<std::uint16_t>(
                    kRegisterBase + index));
        }
    }

    return result;
}

[[nodiscard]] bool ReadRegisterMap(
    const VendorFamily family,
    const int drive,
    RegisterDump& dump,
    const BurnerMaxLogCallback& log,
    std::string& error)
{
    return
        family == VendorFamily::F1
            ? ReadRegistersF1(
                  drive,
                  dump,
                  log,
                  error)
            : ReadRegistersDf(
                  drive,
                  dump,
                  log,
                  error);
}

[[nodiscard]] bool WriteRegister(
    const VendorFamily family,
    const int drive,
    const std::uint16_t address,
    const std::uint8_t value,
    std::string& error)
{
    return
        family == VendorFamily::F1
            ? WriteRegisterF1(
                  drive,
                  address,
                  value,
                  error)
            : WriteRegisterDf(
                  drive,
                  address,
                  value,
                  error);
}

[[nodiscard]] bool VerifyPatternAtAddresses(
    const RegisterDump& dump,
    const std::vector<std::uint16_t>& addresses,
    const std::array<std::uint8_t, 3>& expected,
    const char* const name,
    const BurnerMaxLogCallback& log,
    std::string& error)
{
    for (const std::uint16_t address : addresses) {
        const std::size_t index =
            static_cast<std::size_t>(
                address - kRegisterBase);

        if (index + expected.size() > dump.size()) {
            error =
                std::string(name) +
                " verification address is outside register map";
            return false;
        }

        for (std::size_t offset = 0;
             offset < expected.size();
             ++offset) {
            if (dump[index + offset] != expected[offset]) {
                error =
                    std::string(name) +
                    " read-back mismatch at " +
                    HexValue(
                        static_cast<std::uint16_t>(
                            address + offset));
                return false;
            }
        }

        Log(
            log,
            std::string("BurnerMAX: verified ") +
                name +
                " replacement at " +
                HexValue(address) +
                "\n");
    }

    return true;
}

[[nodiscard]] bool VerifyActivePayloadFamily(
    const VendorFamily family,
    const int drive,
    BurnerMaxResult& result,
    const BurnerMaxLogCallback& log,
    std::string& error)
{
    RegisterDump dump{};

    if (!ReadRegisterMap(
            family,
            drive,
            dump,
            log,
            error)) {
        return false;
    }

    constexpr std::array<std::uint8_t, 3> layer0Signature = {
        0x22, 0xD8, 0x00};
    constexpr std::array<std::uint8_t, 3> layer1Signature = {
        0x42, 0xB0, 0x00};
    constexpr std::array<std::uint8_t, 3> layer0Replacement = {
        0x23, 0x8E, 0x10};
    constexpr std::array<std::uint8_t, 3> layer1Replacement = {
        0x44, 0x1C, 0x20};

    const auto stockL0 =
        FindPattern(dump, layer0Signature);
    const auto stockL1 =
        FindPattern(dump, layer1Signature);
    const auto activeL0 =
        FindPattern(dump, layer0Replacement);
    const auto activeL1 =
        FindPattern(dump, layer1Replacement);

    Log(
        log,
        "BurnerMAX: active-state scan via " +
            std::string(
                family == VendorFamily::F1
                    ? "F1"
                    : "DF") +
            ": replacement L0=" +
            std::to_string(activeL0.size()) +
            ", replacement L1=" +
            std::to_string(activeL1.size()) +
            ", stock L0=" +
            std::to_string(stockL0.size()) +
            ", stock L1=" +
            std::to_string(stockL1.size()) +
            "\n");

    if (activeL0.empty() ||
        activeL1.empty()) {
        error =
            "XGD3 boundary is visible but active BurnerMAX replacement signatures were not both found";
        return false;
    }

    if (!stockL0.empty() ||
        !stockL1.empty()) {
        error =
            "XGD3 boundary is visible but unpatched stock signatures remain";
        return false;
    }

    result.backend =
        family == VendorFamily::F1
            ? "F1"
            : "DF";
    result.layer0Registers = activeL0;
    result.layer1Registers = activeL1;

    return true;
}

[[nodiscard]] bool PatchPatternSet(
    const VendorFamily family,
    const int drive,
    const std::vector<std::uint16_t>& addresses,
    const std::array<std::uint8_t, 3>& replacement,
    const char* const name,
    const BurnerMaxLogCallback& log,
    std::string& error)
{
    for (const std::uint16_t address : addresses) {
        Log(
            log,
            std::string("BurnerMAX: patching ") +
                name +
                " at " +
                HexValue(address) +
                "\n");

        for (std::size_t offset = 0;
             offset < replacement.size();
             ++offset) {
            if (!WriteRegister(
                    family,
                    drive,
                    static_cast<std::uint16_t>(
                        address + offset),
                    replacement[offset],
                    error)) {
                return false;
            }
        }
    }

    return true;
}

[[nodiscard]] bool TryPayloadFamily(
    const VendorFamily family,
    const int drive,
    BurnerMaxResult& result,
    const BurnerMaxLogCallback& log,
    std::string& error,
    bool& writeAttempted)
{
    writeAttempted = false;

    RegisterDump dump{};

    if (!ReadRegisterMap(
            family,
            drive,
            dump,
            log,
            error)) {
        Log(
            log,
            "BurnerMAX: " +
                std::string(
                    family == VendorFamily::F1
                        ? "F1"
                        : "DF") +
                " access unavailable (" +
                error +
                ")\n");
        return false;
    }

    constexpr std::array<std::uint8_t, 3> layer0Signature = {
        0x22, 0xD8, 0x00};
    constexpr std::array<std::uint8_t, 3> layer1Signature = {
        0x42, 0xB0, 0x00};
    constexpr std::array<std::uint8_t, 3> layer0Replacement = {
        0x23, 0x8E, 0x10};
    constexpr std::array<std::uint8_t, 3> layer1Replacement = {
        0x44, 0x1C, 0x20};

    const auto layer0 =
        FindPattern(dump, layer0Signature);
    const auto layer1 =
        FindPattern(dump, layer1Signature);

    if (layer0.empty() ||
        layer1.empty()) {
        error =
            "required MTK register signatures were not found";
        return false;
    }

    constexpr std::size_t kMaximumSignatureMatches = 16;

    if (layer0.size() > kMaximumSignatureMatches ||
        layer1.size() > kMaximumSignatureMatches) {
        error =
            "ambiguous BurnerMAX register signature scan";
        return false;
    }

    result.backend =
        family == VendorFamily::F1
            ? "F1"
            : "DF";
    result.layer0Registers = layer0;
    result.layer1Registers = layer1;
    writeAttempted = true;

    for (const std::uint16_t address : layer0) {
        Log(
            log,
            "BurnerMAX: L0 register " +
                HexValue(address) +
                "\n");
    }

    for (const std::uint16_t address : layer1) {
        Log(
            log,
            "BurnerMAX: L1 register " +
                HexValue(address) +
                "\n");
    }

    if (!PatchPatternSet(
            family,
            drive,
            layer0,
            layer0Replacement,
            "L0",
            log,
            error)) {
        return false;
    }

    if (!PatchPatternSet(
            family,
            drive,
            layer1,
            layer1Replacement,
            "L1",
            log,
            error)) {
        return false;
    }

    Log(
        log,
        "BurnerMAX: re-scanning MTK registers for read-back verification...\n");

    RegisterDump verifyDump{};
    std::string verifyError;

    if (!ReadRegisterMap(
            family,
            drive,
            verifyDump,
            log,
            verifyError)) {
        error =
            "post-payload register re-scan failed: " +
            verifyError;
        return false;
    }

    if (!VerifyPatternAtAddresses(
            verifyDump,
            layer0,
            layer0Replacement,
            "L0",
            log,
            error) ||
        !VerifyPatternAtAddresses(
            verifyDump,
            layer1,
            layer1Replacement,
            "L1",
            log,
            error)) {
        return false;
    }

    const auto remainingStockL0 =
        FindPattern(
            verifyDump,
            layer0Signature);
    const auto remainingStockL1 =
        FindPattern(
            verifyDump,
            layer1Signature);
    const auto activeL0 =
        FindPattern(
            verifyDump,
            layer0Replacement);
    const auto activeL1 =
        FindPattern(
            verifyDump,
            layer1Replacement);

    if (!remainingStockL0.empty() ||
        !remainingStockL1.empty()) {
        error =
            "post-payload verification found unpatched stock capacity signatures";
        return false;
    }

    if (activeL0.size() < layer0.size() ||
        activeL1.size() < layer1.size()) {
        error =
            "post-payload replacement signature count is too low";
        return false;
    }

    Log(
        log,
        "BurnerMAX: post-payload register read-back verified.\n");

    return true;
}

} // namespace

BurnerMaxResult EnableBurnerMax(
    const std::string& opticalSgDevice,
    const BurnerMaxLogCallback& log)
{
    BurnerMaxResult result;

    std::string validationError;
    if (!ValidateExactOpticalSg(
            opticalSgDevice,
            validationError)) {
        result.status =
            BurnerMaxStatus::IoError;
        result.message =
            validationError;
        return result;
    }

    Log(
        log,
        "\nBurnerMAX payload preflight\n");
    Log(
        log,
        "BurnerMAX Linux transport: SG_IO\n");
    Log(
        log,
        "BurnerMAX: opening " +
            opticalSgDevice +
            " for vendor SCSI access.\n");

    FileDescriptor drive(
        ::open(
            opticalSgDevice.c_str(),
            O_RDWR | O_NONBLOCK));

    if (drive.value < 0) {
        result.status =
            BurnerMaxStatus::IoError;
        result.message =
            std::string(
                "Could not open optical SG node: ") +
            std::strerror(errno);
        return result;
    }

    std::uint32_t boundary = 0;
    std::string error;

    if (!ReadLayerBoundary(
            drive.value,
            boundary,
            error)) {
        result.status =
            BurnerMaxStatus::Unsupported;
        result.message =
            "Drive did not return the DVD+R DL layer-boundary structure: " +
            error;
        return result;
    }

    result.layerBoundary = boundary;

    Log(
        log,
        "BurnerMAX: current layer boundary " +
            std::to_string(boundary) +
            " (" +
            HexValue(boundary, 6) +
            ").\n");

    if (boundary == kXgd3LayerBoundary) {
        Log(
            log,
            "BurnerMAX: XGD3 boundary already present; verifying active register state.\n");

        std::string f1Error;

        if (VerifyActivePayloadFamily(
                VendorFamily::F1,
                drive.value,
                result,
                log,
                f1Error)) {
            result.status =
                BurnerMaxStatus::AlreadyEnabled;
            result.message =
                "BurnerMAX is already active; MTK register replacements and XGD3 boundary verified.";
            return result;
        }

        Log(
            log,
            "BurnerMAX: active F1 verification failed (" +
                f1Error +
                "); trying DF.\n");

        std::string dfError;

        if (VerifyActivePayloadFamily(
                VendorFamily::Df,
                drive.value,
                result,
                log,
                dfError)) {
            result.status =
                BurnerMaxStatus::AlreadyEnabled;
            result.message =
                "BurnerMAX is already active; MTK register replacements and XGD3 boundary verified.";
            return result;
        }

        result.status =
            BurnerMaxStatus::IoError;
        result.message =
            "XGD3 boundary is visible but strict BurnerMAX register verification failed. F1: " +
            f1Error +
            "; DF: " +
            dfError;
        return result;
    }

    bool f1WriteAttempted = false;
    std::string f1Error;

    if (TryPayloadFamily(
            VendorFamily::F1,
            drive.value,
            result,
            log,
            f1Error,
            f1WriteAttempted)) {
        // Success via F1.
    } else if (f1WriteAttempted) {
        result.status =
            BurnerMaxStatus::IoError;
        result.message =
            "BurnerMAX found F1 capacity registers but a vendor-register write/read-back failed: " +
            f1Error +
            ". Eject/reinsert the blank disc before retrying.";
        return result;
    } else {
        Log(
            log,
            "BurnerMAX: falling back to DF register access.\n");

        bool dfWriteAttempted = false;
        std::string dfError;

        if (!TryPayloadFamily(
                VendorFamily::Df,
                drive.value,
                result,
                log,
                dfError,
                dfWriteAttempted)) {
            if (dfWriteAttempted) {
                result.status =
                    BurnerMaxStatus::IoError;
                result.message =
                    "BurnerMAX found DF capacity registers but a vendor-register write/read-back failed: " +
                    dfError +
                    ". Eject/reinsert the blank disc before retrying.";
            } else {
                result.status =
                    BurnerMaxStatus::Unsupported;
                result.message =
                    "BurnerMAX payload unsupported on this drive/firmware. F1: " +
                    f1Error +
                    "; DF: " +
                    dfError;
            }
            return result;
        }
    }

    std::uint32_t afterBoundary = 0;
    error.clear();

    if (!ReadLayerBoundary(
            drive.value,
            afterBoundary,
            error)) {
        result.status =
            BurnerMaxStatus::IoError;
        result.message =
            "Register writes completed, but post-payload layer-boundary verification failed: " +
            error;
        return result;
    }

    result.layerBoundary =
        afterBoundary;

    Log(
        log,
        "BurnerMAX: post-payload layer boundary " +
            std::to_string(afterBoundary) +
            " (" +
            HexValue(afterBoundary, 6) +
            ").\n");

    if (afterBoundary !=
        kXgd3LayerBoundary) {
        result.status =
            BurnerMaxStatus::Unsupported;
        result.message =
            "BurnerMAX register writes did not produce XGD3 layer boundary 2133520.";
        return result;
    }

    result.status =
        BurnerMaxStatus::Enabled;
    result.message =
        "BurnerMAX payload enabled via " +
        result.backend +
        "; XGD3 layer boundary 2133520 verified.";

    Log(
        log,
        "BurnerMAX: payload verification SUCCESS.\n");

    return result;
}
