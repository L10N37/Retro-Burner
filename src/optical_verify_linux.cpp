#include "optical_verify_linux.h"

#include <fcntl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kSectorBytes = 2048;
constexpr std::uint16_t kSectorsPerCommand = 32;

struct FileDescriptor final {
    int value = -1;

    explicit FileDescriptor(int fd) noexcept : value(fd) {}
    ~FileDescriptor() {
        if (value >= 0)
            ::close(value);
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
};

[[nodiscard]] bool ValidateExactOpticalSg(
    const std::string& device,
    std::string& error)
{
    const fs::path path(device);

    if (path.parent_path() != "/dev") {
        error = "Device is not an exact /dev/sgX path.";
        return false;
    }

    const std::string name = path.filename().string();
    if (name.size() < 3U || name.rfind("sg", 0) != 0) {
        error = "Device is not an exact /dev/sgX path.";
        return false;
    }

    if (!std::all_of(
            name.begin() + 2,
            name.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; })) {
        error = "Invalid /dev/sgX numeric suffix.";
        return false;
    }

    std::ifstream typeFile(
        fs::path("/sys/class/scsi_generic") / name / "device/type");

    int type = -1;
    typeFile >> type;

    if (!typeFile || (type != 4 && type != 5)) {
        error =
            "Refusing verification: SG node is not sysfs SCSI type 4/5 optical.";
        return false;
    }

    if (::access(path.c_str(), R_OK | W_OK) != 0) {
        error =
            "Current desktop session does not have SG_IO access to "
            "the optical node.";
        return false;
    }

    return true;
}

[[nodiscard]] std::string SenseHex(
    const unsigned char* data,
    const unsigned length)
{
    std::ostringstream text;
    text << std::hex << std::setfill('0');

    for (unsigned i = 0; i < length; ++i) {
        if (i != 0)
            text << ' ';
        text << std::setw(2) << static_cast<unsigned>(data[i]);
    }

    return text.str();
}

} // namespace

std::string OpticalVerifyResult::ToText() const
{
    std::ostringstream text;
    text << "Retro Burner direct optical verification\n";
    text << "device=" << device << '\n';
    text << "source=" << sourcePath.string() << '\n';
    text << "start_lba=" << startLba << '\n';
    text << "sector_count=" << sectorCount << '\n';
    text << "bytes_compared=" << bytesCompared << '\n';

    if (!error.empty())
        text << "error=" << error << '\n';

    text << "verify_success=" << (success ? "yes" : "no") << '\n';
    return text.str();
}

OpticalVerifyResult VerifyOpticalSectors(
    const std::string& exactSgDevice,
    const fs::path& sourcePath,
    const std::uint32_t startLba,
    OpticalVerifyProgressCallback progressCallback)
{
    OpticalVerifyResult result;
    result.device = exactSgDevice;
    result.sourcePath = sourcePath;
    result.startLba = startLba;

    if (!ValidateExactOpticalSg(exactSgDevice, result.error))
        return result;

    std::error_code fsError;
    const std::uintmax_t sourceBytes = fs::file_size(sourcePath, fsError);
    if (fsError) {
        result.error =
            "Could not determine source size: " + fsError.message();
        return result;
    }

    if (sourceBytes == 0 || (sourceBytes % kSectorBytes) != 0) {
        result.error =
            "Verification source must be non-empty and a multiple of 2048 bytes.";
        return result;
    }

    const std::uintmax_t sectorCountWide = sourceBytes / kSectorBytes;
    if (sectorCountWide >
        static_cast<std::uintmax_t>(std::numeric_limits<std::uint32_t>::max())) {
        result.error = "Verification source contains too many sectors.";
        return result;
    }

    result.sectorCount = static_cast<std::uint32_t>(sectorCountWide);

    if (progressCallback)
        progressCallback(0, result.sectorCount);

    if (startLba >
        std::numeric_limits<std::uint32_t>::max() - result.sectorCount) {
        result.error = "Requested LBA range overflows READ(10).";
        return result;
    }

    std::ifstream source(sourcePath, std::ios::binary);
    if (!source) {
        result.error = "Could not open verification source.";
        return result;
    }

    FileDescriptor sg(
        ::open(exactSgDevice.c_str(), O_RDWR | O_NONBLOCK));

    if (sg.value < 0) {
        result.error =
            std::string("Could not open optical SG node: ") +
            std::strerror(errno);
        return result;
    }

    std::vector<unsigned char> sourceBuffer(
        static_cast<std::size_t>(kSectorsPerCommand) * kSectorBytes);
    std::vector<unsigned char> mediaBuffer(sourceBuffer.size());

    std::uint32_t completed = 0;

    // About 200 UI updates for a whole disc (~0.5% granularity), rather than
    // one callback per 32-sector SG_IO READ(10).
    const std::uint32_t progressStep =
        std::max<std::uint32_t>(
            1U,
            result.sectorCount / 200U);
    std::uint32_t nextProgressReport =
        progressStep;

    while (completed < result.sectorCount) {
        const std::uint16_t sectorsThisCommand =
            static_cast<std::uint16_t>(
                std::min<std::uint32_t>(
                    kSectorsPerCommand,
                    result.sectorCount - completed));

        const std::size_t bytesThisCommand =
            static_cast<std::size_t>(sectorsThisCommand) * kSectorBytes;

        source.read(
            reinterpret_cast<char*>(sourceBuffer.data()),
            static_cast<std::streamsize>(bytesThisCommand));

        if (source.gcount() !=
            static_cast<std::streamsize>(bytesThisCommand)) {
            result.error =
                "Short read from source image at byte offset " +
                std::to_string(result.bytesCompared) + ".";
            return result;
        }

        const std::uint32_t lba = startLba + completed;

        std::array<unsigned char, 10> cdb{};
        std::array<unsigned char, 64> sense{};

        cdb[0] = 0x28;
        cdb[2] = static_cast<unsigned char>((lba >> 24) & 0xFF);
        cdb[3] = static_cast<unsigned char>((lba >> 16) & 0xFF);
        cdb[4] = static_cast<unsigned char>((lba >> 8) & 0xFF);
        cdb[5] = static_cast<unsigned char>(lba & 0xFF);
        cdb[7] =
            static_cast<unsigned char>((sectorsThisCommand >> 8) & 0xFF);
        cdb[8] =
            static_cast<unsigned char>(sectorsThisCommand & 0xFF);

        sg_io_hdr_t io{};
        io.interface_id = 'S';
        io.dxfer_direction = SG_DXFER_FROM_DEV;
        io.cmd_len = static_cast<unsigned char>(cdb.size());
        io.mx_sb_len = static_cast<unsigned char>(sense.size());
        io.dxfer_len = static_cast<unsigned int>(bytesThisCommand);
        io.dxferp = mediaBuffer.data();
        io.cmdp = cdb.data();
        io.sbp = sense.data();
        io.timeout = 20000;

        if (::ioctl(sg.value, SG_IO, &io) < 0) {
            result.error =
                "SG_IO READ(10) ioctl failed at LBA " +
                std::to_string(lba) + ": " + std::strerror(errno);
            return result;
        }

        if ((io.info & SG_INFO_OK_MASK) != SG_INFO_OK ||
            io.status != 0 ||
            io.host_status != 0 ||
            io.driver_status != 0 ||
            io.resid != 0) {
            std::ostringstream error;
            error
                << "READ(10) failed at LBA " << lba
                << " status=0x" << std::hex
                << static_cast<unsigned>(io.status)
                << " host=0x" << io.host_status
                << " driver=0x" << io.driver_status
                << std::dec
                << " resid=" << io.resid;

            if (io.sb_len_wr != 0) {
                error << " sense="
                      << SenseHex(sense.data(), io.sb_len_wr);
            }

            result.error = error.str();
            return result;
        }

        const auto sourceEnd =
            sourceBuffer.begin() +
            static_cast<std::ptrdiff_t>(bytesThisCommand);

        const auto mismatch =
            std::mismatch(sourceBuffer.begin(), sourceEnd, mediaBuffer.begin());

        if (mismatch.first != sourceEnd) {
            const std::size_t offsetInChunk =
                static_cast<std::size_t>(
                    std::distance(sourceBuffer.begin(), mismatch.first));

            const std::uint64_t absoluteOffset =
                result.bytesCompared + offsetInChunk;

            const std::uint32_t mismatchSector =
                lba +
                static_cast<std::uint32_t>(
                    offsetInChunk / kSectorBytes);

            std::ostringstream error;
            error
                << "Media differs at byte " << absoluteOffset
                << " (LBA " << mismatchSector
                << ", byte-in-sector "
                << (offsetInChunk % kSectorBytes)
                << "): expected 0x"
                << std::hex
                << static_cast<unsigned>(*mismatch.first)
                << " got 0x"
                << static_cast<unsigned>(*mismatch.second);

            result.error = error.str();
            return result;
        }

        result.bytesCompared += bytesThisCommand;
        completed += sectorsThisCommand;

        if (progressCallback &&
            (completed >= nextProgressReport ||
             completed == result.sectorCount)) {
            progressCallback(
                completed,
                result.sectorCount);

            if (completed <=
                std::numeric_limits<std::uint32_t>::max() -
                    progressStep) {
                nextProgressReport =
                    completed +
                    progressStep;
            } else {
                nextProgressReport =
                    result.sectorCount;
            }
        }
    }

    result.success = true;
    return result;
}
