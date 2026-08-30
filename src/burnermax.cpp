#include "burnermax.h"

#include <windows.h>
#include <ntddscsi.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// RB_WIDE_TO_UTF8_V84O
[[nodiscard]] std::string WideToUtf8(
    const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int requiredBytes =
        WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

    if (requiredBytes <= 0) {
        return {};
    }

    std::string result(
        static_cast<std::size_t>(requiredBytes),
        '\0');

    const int convertedBytes =
        WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            requiredBytes,
            nullptr,
            nullptr);

    if (convertedBytes != requiredBytes) {
        return {};
    }

    return result;
}
constexpr std::uint16_t kRegisterBase = 0x8000;
constexpr std::uint16_t kRegisterEnd = 0x9000;
constexpr std::uint32_t kXgd3LayerBoundary = 2133520;
constexpr std::uint32_t kXgd3ImageSectors = 4267015;
constexpr std::size_t kRegisterCount =
    static_cast<std::size_t>(kRegisterEnd - kRegisterBase);
constexpr std::size_t kSenseBytes = 32;
constexpr ULONG kScsiTimeoutSeconds = 15;

using RegisterDump = std::array<std::uint8_t, kRegisterCount>;

enum class VendorFamily {
    F1,
    Df,
};

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

    [[nodiscard]] bool Valid() const noexcept {
        return value != INVALID_HANDLE_VALUE && value != nullptr;
    }
};

struct ScsiPacket final {
    SCSI_PASS_THROUGH_DIRECT sptd{};
    std::array<UCHAR, kSenseBytes> sense{};
};

[[nodiscard]] std::string HexValue(
    const std::uint32_t value,
    const int width = 4) {
    std::ostringstream stream;
    stream << "0x"
           << std::uppercase
           << std::hex
           << std::setfill('0')
           << std::setw(width)
           << value;
    return stream.str();
}

void Log(
    const BurnerMaxLogCallback& callback,
    const std::string& text) {
    if (callback) {
        callback(text);
    }
}

[[nodiscard]] std::string LastWindowsError(
    const DWORD error) {
    return std::system_category().message(
        static_cast<int>(error));
}

[[nodiscard]] std::wstring DevicePathFromRoot(
    const std::wstring& root) {
    if (root.size() >= 6 &&
        root.rfind(L"\\\\.\\", 0) == 0 &&
        root[5] == L':') {
        return root.substr(0, 6);
    }
    if (root.size() >= 2 && root[1] == L':') {
        return L"\\\\.\\" + root.substr(0, 2);
    }
    return {};
}


[[nodiscard]] std::string RbStorageDescriptorString(
    const std::array<UCHAR, 2048>& buffer,
    const DWORD returned,
    const DWORD offset) {
    if (offset == 0 || offset >= returned) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char*>(buffer.data() + offset));
}

void RbLogDeviceIdentity(
    const std::wstring& opticalDriveRoot,
    const HANDLE drive,
    const BurnerMaxLogCallback& log) {
    STORAGE_DEVICE_NUMBER number{};
    DWORD returned = 0;

    std::ostringstream line;
    line << "RBDEVICE BURNERMAX: handle=0x"
         << std::uppercase << std::hex
         << reinterpret_cast<std::uintptr_t>(drive)
         << std::dec;

    if (DeviceIoControl(
            drive,
            IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr,
            0,
            &number,
            static_cast<DWORD>(sizeof(number)),
            &returned,
            nullptr)) {
        line << " device_type=" << number.DeviceType
             << " device_number=" << number.DeviceNumber
             << " partition=" << number.PartitionNumber;
    } else {
        line << " device_number_query_failed=" << GetLastError();
    }

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    alignas(64) std::array<UCHAR, 2048> descriptorBuffer{};
    returned = 0;

    if (DeviceIoControl(
            drive,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            static_cast<DWORD>(sizeof(query)),
            descriptorBuffer.data(),
            static_cast<DWORD>(descriptorBuffer.size()),
            &returned,
            nullptr) &&
        returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto* descriptor =
            reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(
                descriptorBuffer.data());

        line << " vendor='"
             << RbStorageDescriptorString(
                    descriptorBuffer,
                    returned,
                    descriptor->VendorIdOffset)
             << "' product='"
             << RbStorageDescriptorString(
                    descriptorBuffer,
                    returned,
                    descriptor->ProductIdOffset)
             << "' revision='"
             << RbStorageDescriptorString(
                    descriptorBuffer,
                    returned,
                    descriptor->ProductRevisionOffset)
             << "' serial='"
             << RbStorageDescriptorString(
                    descriptorBuffer,
                    returned,
                    descriptor->SerialNumberOffset)
             << "' bus_type="
             << static_cast<unsigned>(descriptor->BusType)
             << " removable="
             << static_cast<unsigned>(descriptor->RemovableMedia);
    } else {
        line << " device_descriptor_query_failed=" << GetLastError();
    }

    if (opticalDriveRoot.size() >= 2 &&
        opticalDriveRoot[1] == L':') {
        const std::wstring dosName =
            opticalDriveRoot.substr(0, 2);
        std::array<wchar_t, 1024> target{};
        if (QueryDosDeviceW(
                dosName.c_str(),
                target.data(),
                static_cast<DWORD>(target.size())) != 0) {
            const std::wstring wideTarget(target.data());
            line << " dos_name="
                 << WideToUtf8(dosName)
                 << " dos_target='"
                 << WideToUtf8(wideTarget)
                 << "'";
        } else {
            line << " dos_target_query_failed=" << GetLastError();
        }
    }

    line << "\r\n";
    Log(log, line.str());

    query = {};
    query.PropertyId = StorageAdapterProperty;
    query.QueryType = PropertyStandardQuery;
    descriptorBuffer.fill(0);
    returned = 0;

    if (DeviceIoControl(
            drive,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            static_cast<DWORD>(sizeof(query)),
            descriptorBuffer.data(),
            static_cast<DWORD>(descriptorBuffer.size()),
            &returned,
            nullptr) &&
        returned >= sizeof(STORAGE_ADAPTER_DESCRIPTOR)) {
        const auto* adapter =
            reinterpret_cast<const STORAGE_ADAPTER_DESCRIPTOR*>(
                descriptorBuffer.data());

        Log(
            log,
            "RBDEVICE BURNERMAX ADAPTER: alignment_mask=" +
                HexValue(adapter->AlignmentMask, 8) +
                " max_transfer=" +
                std::to_string(adapter->MaximumTransferLength) +
                " max_pages=" +
                std::to_string(adapter->MaximumPhysicalPages) +
                " command_queueing=" +
                std::to_string(
                    static_cast<unsigned>(adapter->CommandQueueing)) +
                " bus_type=" +
                std::to_string(
                    static_cast<unsigned>(adapter->BusType)) +
                "\r\n");
    } else {
        Log(
            log,
            "RBDEVICE BURNERMAX ADAPTER: query failed, Windows error " +
                std::to_string(GetLastError()) + "\r\n");
    }
}
[[nodiscard]] ULONG QueryAlignmentMask(
    const HANDLE drive,
    const BurnerMaxLogCallback& log) {
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageAdapterProperty;
    query.QueryType = PropertyStandardQuery;

    alignas(64) std::array<UCHAR, 512> buffer{};
    DWORD returned = 0;
    if (DeviceIoControl(
            drive,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            static_cast<DWORD>(sizeof(query)),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &returned,
            nullptr) &&
        returned >=
            offsetof(STORAGE_ADAPTER_DESCRIPTOR, AlignmentMask) +
                sizeof(ULONG)) {
        const auto* descriptor =
            reinterpret_cast<const STORAGE_ADAPTER_DESCRIPTOR*>(
                buffer.data());
        return descriptor->AlignmentMask;
    }

    // 4095 yields a 4096-byte aligned transfer address. That is deliberately
    // conservative for optical SPTD transfers if a USB bridge does not expose
    // StorageAdapterProperty cleanly.
    Log(
        log,
        "BurnerMAX: storage alignment query unavailable; using 4096-byte transfer alignment.\r\n");
    return 4095;
}

[[nodiscard]] bool SendScsi(
    const HANDLE drive,
    const ULONG alignmentMask,
    const std::array<UCHAR, 12>& cdb,
    const UCHAR dataDirection,
    void* const data,
    const ULONG dataLength,
    std::string& error,
    const UCHAR cdbLength = 12) {
    ScsiPacket packet{};
    packet.sptd.Length =
        static_cast<USHORT>(sizeof(SCSI_PASS_THROUGH_DIRECT));
    packet.sptd.CdbLength = cdbLength;
    packet.sptd.SenseInfoLength =
        static_cast<UCHAR>(packet.sense.size());
    packet.sptd.DataIn = dataDirection;
    packet.sptd.DataTransferLength = dataLength;
    packet.sptd.TimeOutValue = kScsiTimeoutSeconds;

    std::vector<UCHAR> transferStorage;
    UCHAR* alignedTransfer = nullptr;
    if (dataLength > 0) {
        transferStorage.resize(
            static_cast<std::size_t>(dataLength) +
            static_cast<std::size_t>(alignmentMask));
        const std::uintptr_t raw =
            reinterpret_cast<std::uintptr_t>(
                transferStorage.data());
        const std::uintptr_t aligned =
            (raw + static_cast<std::uintptr_t>(alignmentMask)) &
            ~static_cast<std::uintptr_t>(alignmentMask);
        alignedTransfer =
            reinterpret_cast<UCHAR*>(aligned);

        if (dataDirection == SCSI_IOCTL_DATA_OUT &&
            data != nullptr) {
            std::memcpy(
                alignedTransfer,
                data,
                static_cast<std::size_t>(dataLength));
        }
    }

    packet.sptd.DataBuffer = alignedTransfer;
    packet.sptd.SenseInfoOffset =
        static_cast<ULONG>(offsetof(ScsiPacket, sense));
    std::copy(cdb.begin(), cdb.end(), packet.sptd.Cdb);

    DWORD returned = 0;
    if (!DeviceIoControl(
            drive,
            IOCTL_SCSI_PASS_THROUGH_DIRECT,
            &packet,
            static_cast<DWORD>(sizeof(packet)),
            &packet,
            static_cast<DWORD>(sizeof(packet)),
            &returned,
            nullptr)) {
        const DWORD winError = GetLastError();
        error =
            "DeviceIoControl(IOCTL_SCSI_PASS_THROUGH_DIRECT) failed: " +
            LastWindowsError(winError) +
            " (Windows error " + std::to_string(winError) + ")";
        return false;
    }

    if (packet.sptd.ScsiStatus != 0) {
        const unsigned senseKey =
            packet.sense.size() > 2
                ? static_cast<unsigned>(packet.sense[2] & 0x0F)
                : 0U;
        const unsigned asc =
            packet.sense.size() > 12
                ? static_cast<unsigned>(packet.sense[12])
                : 0U;
        const unsigned ascq =
            packet.sense.size() > 13
                ? static_cast<unsigned>(packet.sense[13])
                : 0U;

        error =
            "SCSI command failed: status " +
            HexValue(packet.sptd.ScsiStatus, 2) +
            ", sense " + HexValue(senseKey, 2) +
            "/" + HexValue(asc, 2) +
            "/" + HexValue(ascq, 2);
        return false;
    }

    if (dataDirection == SCSI_IOCTL_DATA_IN &&
        data != nullptr &&
        alignedTransfer != nullptr &&
        dataLength > 0) {
        const ULONG transferred =
            std::min(dataLength, packet.sptd.DataTransferLength);
        std::memcpy(
            data,
            alignedTransfer,
            static_cast<std::size_t>(transferred));
    }

    return true;
}

[[nodiscard]] bool ReadLayerBoundary(
    const HANDLE drive,
    const ULONG alignmentMask,
    std::uint32_t& boundary,
    std::string& error) {
    alignas(64) std::array<UCHAR, 12> data{};
    std::array<UCHAR, 12> cdb{};
    cdb[0] = 0xAD; // READ DVD STRUCTURE
    cdb[7] = 0x20;
    cdb[9] = static_cast<UCHAR>(data.size());

    if (!SendScsi(
            drive,
            alignmentMask,
            cdb,
            SCSI_IOCTL_DATA_IN,
            data.data(),
            static_cast<ULONG>(data.size()),
            error)) {
        return false;
    }

    boundary =
        (static_cast<std::uint32_t>(data[9]) << 16U) |
        (static_cast<std::uint32_t>(data[10]) << 8U) |
        static_cast<std::uint32_t>(data[11]);
    return true;
}

[[nodiscard]] bool ReadTrackCapacity(
    const HANDLE drive,
    const ULONG alignmentMask,
    std::uint32_t& freeBlocks,
    std::uint32_t& trackSize,
    std::string& error) {
    alignas(64) std::array<UCHAR, 32> data{};
    std::array<UCHAR, 12> cdb{};

    // MMC READ TRACK INFORMATION (10-byte CDB), addressed by
    // track number. Blank DVD+R DL media exposes writable
    // capacity through track 1.
    cdb[0] = 0x52;
    cdb[1] = 0x01; // address type: track number
    cdb[5] = 0x01; // track 1
    cdb[8] = static_cast<UCHAR>(data.size());

    if (!SendScsi(
            drive,
            alignmentMask,
            cdb,
            SCSI_IOCTL_DATA_IN,
            data.data(),
            static_cast<ULONG>(data.size()),
            error,
            10)) {
        return false;
    }

    const std::uint16_t informationLength =
        (static_cast<std::uint16_t>(data[0]) << 8U) |
        static_cast<std::uint16_t>(data[1]);

    // Need response bytes through 27 for Track Size.
    if (informationLength < 26U) {
        error =
            "READ TRACK INFORMATION returned a response too short "
            "to contain writable-capacity fields";
        return false;
    }

    const auto readBe32 =
        [&data](const std::size_t offset) -> std::uint32_t {
            return
                (static_cast<std::uint32_t>(data[offset]) << 24U) |
                (static_cast<std::uint32_t>(data[offset + 1U]) << 16U) |
                (static_cast<std::uint32_t>(data[offset + 2U]) << 8U) |
                static_cast<std::uint32_t>(data[offset + 3U]);
        };

    freeBlocks = readBe32(16U);
    trackSize = readBe32(24U);

    return true;
}

[[nodiscard]] bool ReadRegisterF1(
    const HANDLE drive,
    const ULONG alignmentMask,
    const std::uint16_t address,
    std::uint8_t& value,
    std::string& error) {
    alignas(64) std::array<UCHAR, 4> data{};
    std::array<UCHAR, 12> cdb{};
    cdb[0] = 0xF1;
    cdb[1] = 0x02;
    cdb[4] = static_cast<UCHAR>(address >> 8U);
    cdb[5] = static_cast<UCHAR>(address & 0xFFU);
    cdb[6] = 0x01;

    if (!SendScsi(
            drive,
            alignmentMask,
            cdb,
            SCSI_IOCTL_DATA_IN,
            data.data(),
            static_cast<ULONG>(data.size()),
            error)) {
        return false;
    }

    value = data[3];
    return true;
}

[[nodiscard]] bool WriteRegisterF1(
    const HANDLE drive,
    const ULONG alignmentMask,
    const std::uint16_t address,
    const std::uint8_t value,
    std::string& error) {
    std::array<UCHAR, 12> cdb{};
    cdb[0] = 0xF1;
    cdb[1] = 0x01;
    cdb[4] = static_cast<UCHAR>(address >> 8U);
    cdb[5] = static_cast<UCHAR>(address & 0xFFU);
    cdb[9] = value;

    return SendScsi(
        drive,
        alignmentMask,
        cdb,
        SCSI_IOCTL_DATA_UNSPECIFIED,
        nullptr,
        0,
        error);
}

[[nodiscard]] bool ReadRegistersF1(
    const HANDLE drive,
    const ULONG alignmentMask,
    RegisterDump& dump,
    const BurnerMaxLogCallback& log,
    std::string& error) {
    Log(log, "BurnerMAX: trying MTK F1 register access...\r\n");

    for (std::uint32_t address = kRegisterBase;
         address < kRegisterEnd;
         ++address) {
        std::uint8_t value = 0;
        if (!ReadRegisterF1(
                drive,
                alignmentMask,
                static_cast<std::uint16_t>(address),
                value,
                error)) {
            return false;
        }
        dump[static_cast<std::size_t>(address - kRegisterBase)] = value;

        if (((address - kRegisterBase + 1U) % 512U) == 0U) {
            Log(
                log,
                "BurnerMAX: F1 scan " +
                    std::to_string(address - kRegisterBase + 1U) +
                    "/4096 registers\r\n");
        }
    }

    return true;
}

[[nodiscard]] bool ReadRegistersDf(
    const HANDLE drive,
    const ULONG alignmentMask,
    RegisterDump& dump,
    const BurnerMaxLogCallback& log,
    std::string& error) {
    Log(log, "BurnerMAX: trying MTK DF block register access...\r\n");

    constexpr std::uint32_t blockSize = 128;
    for (std::uint32_t address = kRegisterBase;
         address < kRegisterEnd;
         address += blockSize) {
        alignas(64) std::array<UCHAR, blockSize> data{};
        std::array<UCHAR, 12> cdb{};
        cdb[0] = 0xDF;
        cdb[1] = 0x85;
        cdb[3] = 0xFF;
        cdb[7] = static_cast<UCHAR>(address >> 8U);
        cdb[8] = static_cast<UCHAR>(address & 0xFFU);

        if (!SendScsi(
                drive,
                alignmentMask,
                cdb,
                SCSI_IOCTL_DATA_IN,
                data.data(),
                static_cast<ULONG>(data.size()),
                error)) {
            return false;
        }

        const std::size_t offset =
            static_cast<std::size_t>(address - kRegisterBase);
        std::copy(
            data.begin(),
            data.end(),
            dump.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    return true;
}

[[nodiscard]] bool WriteRegisterDf(
    const HANDLE drive,
    const ULONG alignmentMask,
    const std::uint16_t address,
    const std::uint8_t value,
    std::string& error) {
    std::array<UCHAR, 12> cdb{};
    cdb[0] = 0xDF;
    cdb[1] = 0x84;
    cdb[4] = 0x01;
    cdb[7] = static_cast<UCHAR>(address >> 8U);
    cdb[8] = static_cast<UCHAR>(address & 0xFFU);
    cdb[9] = value;

    return SendScsi(
        drive,
        alignmentMask,
        cdb,
        SCSI_IOCTL_DATA_UNSPECIFIED,
        nullptr,
        0,
        error);
}

[[nodiscard]] std::vector<std::uint16_t> FindPattern(
    const RegisterDump& dump,
    const std::array<std::uint8_t, 3>& pattern) {
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

[[nodiscard]] bool VerifyPatternAtAddresses(
    const RegisterDump& dump,
    const std::vector<std::uint16_t>& addresses,
    const std::array<std::uint8_t, 3>& expected,
    const char* const name,
    const BurnerMaxLogCallback& log,
    std::string& error) {
    for (const std::uint16_t address : addresses) {
        const std::size_t index =
            static_cast<std::size_t>(address - kRegisterBase);

        if (index + expected.size() > dump.size()) {
            error =
                std::string(name) +
                " verification address is outside the scanned register range";
            return false;
        }

        for (std::size_t offset = 0;
             offset < expected.size();
             ++offset) {
            if (dump[index + offset] != expected[offset]) {
                error =
                    std::string(name) +
                    " register read-back mismatch at " +
                    HexValue(
                        static_cast<std::uint16_t>(
                            address + offset)) +
                    ": expected " +
                    HexValue(expected[offset], 2) +
                    ", got " +
                    HexValue(dump[index + offset], 2);
                Log(
                    log,
                    "BurnerMAX: " + error + ".\r\n");
                return false;
            }
        }

        Log(
            log,
            std::string("BurnerMAX: verified ") + name +
                " replacement at " + HexValue(address) +
                "\r\n");
    }

    return true;
}

void RbLogRegisterDump(
    const char* const label,
    const RegisterDump& dump,
    const BurnerMaxLogCallback& log);
[[nodiscard]] bool VerifyActivePayloadFamily(
    const VendorFamily family,
    const HANDLE drive,
    const ULONG alignmentMask,
    BurnerMaxResult& result,
    const BurnerMaxLogCallback& log,
    std::string& error) {
    RegisterDump dump{};
    const bool readOk = family == VendorFamily::F1
        ? ReadRegistersF1(drive, alignmentMask, dump, log, error)
        : ReadRegistersDf(drive, alignmentMask, dump, log, error);

    if (!readOk) {
        return false;
    }

        RbLogRegisterDump(
        family == VendorFamily::F1
            ? "ACTIVE-PAYLOAD-F1"
            : "ACTIVE-PAYLOAD-DF",
        dump,
        log);
constexpr std::array<std::uint8_t, 3> layer0Signature = {
        0x22, 0xD8, 0x00};
    constexpr std::array<std::uint8_t, 3> layer1Signature = {
        0x42, 0xB0, 0x00};
    constexpr std::array<std::uint8_t, 3> layer0Replacement = {
        0x23, 0x8E, 0x10};
    constexpr std::array<std::uint8_t, 3> layer1Replacement = {
        0x44, 0x1C, 0x20};

    const auto stockL0 = FindPattern(dump, layer0Signature);
    const auto stockL1 = FindPattern(dump, layer1Signature);
    const auto activeL0 = FindPattern(dump, layer0Replacement);
    const auto activeL1 = FindPattern(dump, layer1Replacement);

    Log(
        log,
        "BurnerMAX: active-state register scan via " +
            std::string(
                family == VendorFamily::F1 ? "F1" : "DF") +
            ": replacement L0=" +
            std::to_string(activeL0.size()) +
            ", replacement L1=" +
            std::to_string(activeL1.size()) +
            ", stock L0=" +
            std::to_string(stockL0.size()) +
            ", stock L1=" +
            std::to_string(stockL1.size()) +
            ".\r\n");

    if (activeL0.empty() || activeL1.empty()) {
        error =
            "XGD3 layer boundary is visible, but the active BurnerMAX "
            "replacement signatures were not both found";
        return false;
    }

    if (!stockL0.empty() || !stockL1.empty()) {
        error =
            "XGD3 layer boundary is visible, but unpatched stock "
            "capacity signatures remain in the MTK register map";
        return false;
    }

    result.backend =
        family == VendorFamily::F1 ? "F1" : "DF";
    result.layer0Registers = activeL0;
    result.layer1Registers = activeL1;

    Log(
        log,
        "BurnerMAX: already-active payload register state verified via " +
            result.backend + ".\r\n");
    return true;
}

[[nodiscard]] bool WriteRegister(
    const VendorFamily family,
    const HANDLE drive,
    const ULONG alignmentMask,
    const std::uint16_t address,
    const std::uint8_t value,
    std::string& error) {
    return family == VendorFamily::F1
        ? WriteRegisterF1(drive, alignmentMask, address, value, error)
        : WriteRegisterDf(drive, alignmentMask, address, value, error);
}

[[nodiscard]] bool PatchPatternSet(
    const VendorFamily family,
    const HANDLE drive,
    const ULONG alignmentMask,
    const std::vector<std::uint16_t>& addresses,
    const std::array<std::uint8_t, 3>& replacement,
    const char* const name,
    const BurnerMaxLogCallback& log,
    std::string& error) {
    for (const std::uint16_t address : addresses) {
        Log(
            log,
            std::string("BurnerMAX: patching ") + name +
                " at " + HexValue(address) + "\r\n");

        for (std::size_t offset = 0;
             offset < replacement.size();
             ++offset) {
            if (!WriteRegister(
                    family,
                    drive,
                    alignmentMask,
                    static_cast<std::uint16_t>(address + offset),
                    replacement[offset],
                    error)) {
                return false;
            }
        }
    }
    return true;
}


void RbLogRegisterDump(
    const char* const label,
    const RegisterDump& dump,
    const BurnerMaxLogCallback& log) {
    Log(log, "\r\nRBREGISTER " + std::string(label) + " BEGIN\r\n");

    for (std::size_t offset = 0; offset < dump.size(); offset += 16) {
        std::ostringstream line;
        line << "RBREGISTER "
             << HexValue(
                    static_cast<std::uint32_t>(
                        kRegisterBase + offset))
             << ":";

        for (std::size_t column = 0;
             column < 16 && offset + column < dump.size();
             ++column) {
            line << " "
                 << std::uppercase
                 << std::hex
                 << std::setfill('0')
                 << std::setw(2)
                 << static_cast<unsigned>(
                        dump[offset + column]);
        }

        line << "\r\n";
        Log(log, line.str());
    }

    Log(log, "RBREGISTER " + std::string(label) + " END\r\n");
}

void RbLogRegisterDiff(
    const RegisterDump& before,
    const RegisterDump& after,
    const BurnerMaxLogCallback& log) {
    std::size_t changed = 0;
    Log(log, "\r\nRBREGISTER DIFF BEGIN\r\n");

    for (std::size_t index = 0;
         index < before.size();
         ++index) {
        if (before[index] == after[index]) {
            continue;
        }

        ++changed;
        Log(
            log,
            "RBREGISTER CHANGED " +
                HexValue(
                    static_cast<std::uint32_t>(
                        kRegisterBase + index)) +
                ": " +
                HexValue(before[index], 2) +
                " -> " +
                HexValue(after[index], 2) +
                "\r\n");
    }

    Log(
        log,
        "RBREGISTER DIFF END: changed_bytes=" +
            std::to_string(changed) +
            "\r\n\r\n");
}
[[nodiscard]] bool TryPayloadFamily(
    const VendorFamily family,
    const HANDLE drive,
    const ULONG alignmentMask,
    BurnerMaxResult& result,
    const BurnerMaxLogCallback& log,
    std::string& error,
    bool& writeAttempted) {
    writeAttempted = false;
    RegisterDump dump{};
    const bool readOk = family == VendorFamily::F1
        ? ReadRegistersF1(drive, alignmentMask, dump, log, error)
        : ReadRegistersDf(drive, alignmentMask, dump, log, error);

    if (!readOk) {
        Log(
            log,
            "BurnerMAX: " +
                std::string(family == VendorFamily::F1 ? "F1" : "DF") +
                " access not available (" + error + ").\r\n");
        return false;
    }

        RbLogRegisterDump(
        family == VendorFamily::F1
            ? "PRE-PAYLOAD-F1"
            : "PRE-PAYLOAD-DF",
        dump,
        log);
constexpr std::array<std::uint8_t, 3> layer0Signature = {
        0x22, 0xD8, 0x00};
    constexpr std::array<std::uint8_t, 3> layer1Signature = {
        0x42, 0xB0, 0x00};
    constexpr std::array<std::uint8_t, 3> layer0Replacement = {
        0x23, 0x8E, 0x10};
    constexpr std::array<std::uint8_t, 3> layer1Replacement = {
        0x44, 0x1C, 0x20};

    const std::vector<std::uint16_t> layer0 =
        FindPattern(dump, layer0Signature);
    const std::vector<std::uint16_t> layer1 =
        FindPattern(dump, layer1Signature);

    if (layer0.empty() || layer1.empty()) {
        Log(
            log,
            "BurnerMAX: " +
                std::string(family == VendorFamily::F1 ? "F1" : "DF") +
                " register access worked, but the BurnerMAX capacity signatures were not found.\r\n");
        error = "required MTK register signatures were not found";
        return false;
    }

    // A wildly large number of signatures is more likely to be bad vendor
    // command data than a real drive register map. Refuse to write in that case.
    constexpr std::size_t kMaximumSignatureMatches = 16;
    if (layer0.size() > kMaximumSignatureMatches ||
        layer1.size() > kMaximumSignatureMatches) {
        error = "ambiguous BurnerMAX register signature scan";
        Log(
            log,
            "BurnerMAX: refusing to patch an ambiguous register map.\r\n");
        return false;
    }

    result.backend = family == VendorFamily::F1 ? "F1" : "DF";
    result.layer0Registers = layer0;
    result.layer1Registers = layer1;
    writeAttempted = true;

    for (const std::uint16_t address : layer0) {
        Log(log, "BurnerMAX: L0 register " + HexValue(address) + "\r\n");
    }
    for (const std::uint16_t address : layer1) {
        Log(log, "BurnerMAX: L1 register " + HexValue(address) + "\r\n");
    }

    if (!PatchPatternSet(
            family,
            drive,
            alignmentMask,
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
            alignmentMask,
            layer1,
            layer1Replacement,
            "L1",
            log,
            error)) {
        return false;
    }

    Log(
        log,
        "BurnerMAX: re-scanning MTK register space for post-payload read-back verification...\r\n");

    RegisterDump verifyDump{};
    std::string verifyReadError;
    const bool verifyReadOk = family == VendorFamily::F1
        ? ReadRegistersF1(
              drive,
              alignmentMask,
              verifyDump,
              log,
              verifyReadError)
        : ReadRegistersDf(
              drive,
              alignmentMask,
              verifyDump,
              log,
              verifyReadError);

    if (!verifyReadOk) {
        error =
            "post-payload register re-scan failed: " +
            verifyReadError;
        Log(log, "BurnerMAX: " + error + ".\r\n");
        return false;
    }

        RbLogRegisterDump(
        family == VendorFamily::F1
            ? "POST-PAYLOAD-F1"
            : "POST-PAYLOAD-DF",
        verifyDump,
        log);
    RbLogRegisterDiff(dump, verifyDump, log);
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
        FindPattern(verifyDump, layer0Signature);
    const auto remainingStockL1 =
        FindPattern(verifyDump, layer1Signature);
    const auto activeL0 =
        FindPattern(verifyDump, layer0Replacement);
    const auto activeL1 =
        FindPattern(verifyDump, layer1Replacement);

    Log(
        log,
        "BurnerMAX: post-payload register scan: replacement L0=" +
            std::to_string(activeL0.size()) +
            ", replacement L1=" +
            std::to_string(activeL1.size()) +
            ", remaining stock L0=" +
            std::to_string(remainingStockL0.size()) +
            ", remaining stock L1=" +
            std::to_string(remainingStockL1.size()) +
            ".\r\n");

    if (!remainingStockL0.empty() ||
        !remainingStockL1.empty()) {
        error =
            "post-payload verification found unpatched stock "
            "capacity signatures";
        Log(log, "BurnerMAX: " + error + ".\r\n");
        return false;
    }

    if (activeL0.size() < layer0.size() ||
        activeL1.size() < layer1.size()) {
        error =
            "post-payload replacement signature count is lower "
            "than the number of patched registers";
        Log(log, "BurnerMAX: " + error + ".\r\n");
        return false;
    }

    Log(
        log,
        "BurnerMAX: post-payload register read-back verified.\r\n");

    return true;
}

} // namespace

BurnerMaxResult EnableBurnerMax(
    const std::wstring& opticalDriveRoot,
    const BurnerMaxLogCallback& log) {
    BurnerMaxResult result;

    const std::wstring devicePath =
        DevicePathFromRoot(opticalDriveRoot);
    if (devicePath.empty()) {
        result.status = BurnerMaxStatus::IoError;
        result.message = "BurnerMAX requires a Windows optical drive letter.";
        return result;
    }

    Log(log, "\r\nBurnerMAX payload preflight\r\n");
    Log(
        log,
        "BurnerMAX: opening " +
            WideToUtf8(opticalDriveRoot) +
            " for vendor SCSI access.\r\n");

    UniqueHandle drive(CreateFileW(
        devicePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr));

    if (!drive.Valid()) {
        const DWORD error = GetLastError();
        result.status = BurnerMaxStatus::IoError;
        result.message =
            "Could not open the DVD writer for BurnerMAX vendor commands: " +
            LastWindowsError(error) +
            " (Windows error " + std::to_string(error) + ")";
        Log(log, "BurnerMAX: " + result.message + "\r\n");
        return result;
    }

    RbLogDeviceIdentity(opticalDriveRoot, drive.value, log);

    const ULONG alignmentMask =
        QueryAlignmentMask(drive.value, log);

    std::uint32_t boundary = 0;
    std::string error;
    if (!ReadLayerBoundary(
            drive.value,
            alignmentMask,
            boundary,
            error)) {
        result.status = BurnerMaxStatus::Unsupported;
        result.message =
            "The drive did not return the DVD+R DL layer-boundary structure needed for BurnerMAX: " +
            error;
        Log(log, "BurnerMAX: " + result.message + "\r\n");
        return result;
    }

    result.layerBoundary = boundary;
    Log(
        log,
        "BurnerMAX: current layer boundary " +
            std::to_string(boundary) +
            " (" + HexValue(boundary, 6) + ").\r\n");

    if (boundary == kXgd3LayerBoundary) {
        Log(
            log,
            "BurnerMAX: XGD3 layer boundary is already present; "
            "verifying active MTK register state instead of trusting the boundary alone.\r\n");

        // Permanently-flashed BurnerMAX firmware does not
        // necessarily expose the same RAM signature pattern as the
        // temporary payload. First verify observable MMC behaviour:
        // expanded writable capacity plus the XGD3 layer boundary.
        std::uint32_t freeBlocks = 0;
        std::uint32_t trackSize = 0;
        std::string capacityError;

        if (ReadTrackCapacity(
                drive.value,
                alignmentMask,
                freeBlocks,
                trackSize,
                capacityError)) {
            const std::uint32_t writableCapacity =
                std::max(freeBlocks, trackSize);

            Log(
                log,
                "BurnerMAX: READ TRACK INFORMATION reports free blocks " +
                    std::to_string(freeBlocks) +
                    ", track size " +
                    std::to_string(trackSize) +
                    " sectors.\r\n");

            if (writableCapacity >= kXgd3ImageSectors) {
                result.status = BurnerMaxStatus::AlreadyEnabled;
                result.backend = "MMC capacity";
                result.message =
                    "BurnerMAX is already active; expanded writable "
                    "capacity (" +
                    std::to_string(writableCapacity) +
                    " sectors) and XGD3 layer boundary 2133520 were "
                    "verified. Temporary payload injection is not required.";

                Log(
                    log,
                    "BurnerMAX: expanded XGD3 writable capacity is "
                    "already exposed by the drive; skipping temporary "
                    "payload register verification.\r\n");

                return result;
            }

            Log(
                log,
                "BurnerMAX: XGD3 boundary is present, but writable "
                "capacity is only " +
                    std::to_string(writableCapacity) +
                    " sectors; falling back to strict MTK register "
                    "verification.\r\n");
        } else {
            Log(
                log,
                "BurnerMAX: READ TRACK INFORMATION capacity check "
                "was unavailable (" +
                    capacityError +
                    "); falling back to strict MTK register "
                    "verification.\r\n");
        }

        std::string activeF1Error;
        if (VerifyActivePayloadFamily(
                VendorFamily::F1,
                drive.value,
                alignmentMask,
                result,
                log,
                activeF1Error)) {
            result.status = BurnerMaxStatus::AlreadyEnabled;
            result.message =
                "BurnerMAX is already active; MTK register replacements and "
                "XGD3 layer boundary 2133520 were verified.";
            return result;
        }

        Log(
            log,
            "BurnerMAX: active-state F1 verification unavailable/failed (" +
                activeF1Error + "); trying DF.\r\n");

        std::string activeDfError;
        if (VerifyActivePayloadFamily(
                VendorFamily::Df,
                drive.value,
                alignmentMask,
                result,
                log,
                activeDfError)) {
            result.status = BurnerMaxStatus::AlreadyEnabled;
            result.message =
                "BurnerMAX is already active; MTK register replacements and "
                "XGD3 layer boundary 2133520 were verified.";
            return result;
        }

        result.status = BurnerMaxStatus::IoError;
        result.message =
            "XGD3 layer boundary 2133520 is visible, but strict BurnerMAX "
            "register verification failed. F1: " +
            activeF1Error + "; DF: " + activeDfError;
        Log(log, "BurnerMAX: " + result.message + "\r\n");
        return result;
    }

    bool payloadSent = false;
    bool f1WriteAttempted = false;
    std::string f1Error;
    if (TryPayloadFamily(
            VendorFamily::F1,
            drive.value,
            alignmentMask,
            result,
            log,
            f1Error,
            f1WriteAttempted)) {
        payloadSent = true;
    } else if (f1WriteAttempted) {
        result.status = BurnerMaxStatus::IoError;
        result.message =
            "BurnerMAX found the F1 capacity registers, but a vendor-register "
            "write failed: " + f1Error +
            ". Eject/reinsert the blank disc before retrying.";
        Log(log, "BurnerMAX: F1 payload write failed; not trying another command family after a partial write.\r\n");
        return result;
    } else {
        Log(log, "BurnerMAX: falling back to DF register access.\r\n");
        bool dfWriteAttempted = false;
        std::string dfError;
        if (TryPayloadFamily(
                VendorFamily::Df,
                drive.value,
                alignmentMask,
                result,
                log,
                dfError,
                dfWriteAttempted)) {
            payloadSent = true;
        } else if (dfWriteAttempted) {
            result.status = BurnerMaxStatus::IoError;
            result.message =
                "BurnerMAX found the DF capacity registers, but a vendor-register "
                "write failed: " + dfError +
                ". Eject/reinsert the blank disc before retrying.";
            Log(log, "BurnerMAX: DF payload write failed.\r\n");
            return result;
        } else {
            result.status = BurnerMaxStatus::Unsupported;
            result.message =
                "BurnerMAX payload unsupported on this drive/firmware. "
                "F1: " + f1Error + "; DF: " + dfError;
            Log(log, "BurnerMAX: no compatible MTK payload path found.\r\n");
            return result;
        }
    }

    if (!payloadSent) {
        result.status = BurnerMaxStatus::Unsupported;
        result.message = "BurnerMAX payload could not be sent.";
        return result;
    }

    std::uint32_t afterBoundary = 0;
    error.clear();
    if (!ReadLayerBoundary(
            drive.value,
            alignmentMask,
            afterBoundary,
            error)) {
        result.status = BurnerMaxStatus::IoError;
        result.message =
            "The BurnerMAX register writes completed, but post-payload verification failed: " +
            error;
        Log(log, "BurnerMAX: " + result.message + "\r\n");
        return result;
    }

    result.layerBoundary = afterBoundary;
    Log(
        log,
        "BurnerMAX: post-payload layer boundary " +
            std::to_string(afterBoundary) +
            " (" + HexValue(afterBoundary, 6) + ").\r\n");

    if (afterBoundary != kXgd3LayerBoundary) {
        result.status = BurnerMaxStatus::Unsupported;
        result.message =
            "BurnerMAX register writes did not produce the XGD3 layer boundary. "
            "Eject/reinsert the blank disc before retrying.";
        Log(log, "BurnerMAX: verification FAILED.\r\n");
        return result;
    }

    result.status = BurnerMaxStatus::Enabled;
    result.message =
        "BurnerMAX payload enabled via " + result.backend +
        "; XGD3 layer boundary 2133520 verified.";
    Log(log, "BurnerMAX: payload verification SUCCESS.\r\n");
    return result;
}
