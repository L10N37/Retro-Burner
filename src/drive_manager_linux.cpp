#include "drive_manager.h"

#ifndef _WIN32

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string TrimAscii(std::string value) {
    const auto is_not_space = [](unsigned char c) {
        return std::isspace(c) == 0;
    };

    const auto first = std::find_if(value.begin(), value.end(), is_not_space);
    const auto last = std::find_if(value.rbegin(), value.rend(), is_not_space).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

[[nodiscard]] std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }

    std::string value;
    std::getline(file, value);
    return TrimAscii(std::move(value));
}

[[nodiscard]] std::wstring WidenAscii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

[[nodiscard]] bool IsOpticalScsiType(const std::string& value) {
    // Linux SCSI peripheral types:
    //   4 = write-once optical
    //   5 = CD/DVD/BD device
    return value == "4" || value == "5";
}

[[nodiscard]] std::optional<std::string> FindBlockPeer(
    const fs::path& generic_device_path) {

    const fs::path block_dir = generic_device_path / "device" / "block";
    std::error_code ec;
    if (!fs::exists(block_dir, ec) || ec) {
        return std::nullopt;
    }

    for (const fs::directory_entry& entry :
         fs::directory_iterator(block_dir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            break;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind("sr", 0) == 0) {
            return "/dev/" + name;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string DetectBus(const fs::path& generic_device_path) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(generic_device_path / "device", ec);
    if (ec) {
        return "Linux SG";
    }

    const std::string path = canonical.string();

    // USB optical bridges show a USB device/interface component in the sysfs
    // ancestry. Keep this deliberately conservative; the exact /dev/sgX node
    // is the transport identity RetroBeam actually uses.
    if (path.find("/usb") != std::string::npos) {
        return "USB";
    }

    if (path.find("/ata") != std::string::npos ||
        path.find("/host") != std::string::npos) {
        return "SCSI/ATAPI";
    }

    return "Linux SG";
}

[[nodiscard]] bool CurrentUserCanReadWrite(const std::string& path) {
    return ::access(path.c_str(), R_OK | W_OK) == 0;
}

} // namespace

std::string OpticalDrive::DisplayName() const {
    std::string identity;
    if (!vendor.empty()) {
        identity += vendor;
    }
    if (!product.empty()) {
        if (!identity.empty()) {
            identity += ' ';
        }
        identity += product;
    }
    if (identity.empty()) {
        identity = "Optical drive";
    }

    std::string location;
    if (!cdrecordDevice.empty()) {
        location = cdrecordDevice;
    } else if (!rootPath.empty()) {
        location.assign(rootPath.begin(), rootPath.end());
    }

    if (!location.empty()) {
        return location + "  " + identity;
    }
    return identity;
}

std::vector<OpticalDrive> EnumerateOpticalDrives() {
    std::vector<OpticalDrive> drives;

    const fs::path generic_root{"/sys/class/scsi_generic"};
    std::error_code ec;
    if (!fs::exists(generic_root, ec) || ec) {
        return drives;
    }

    for (const fs::directory_entry& entry :
         fs::directory_iterator(
             generic_root,
             fs::directory_options::skip_permission_denied,
             ec)) {

        if (ec) {
            break;
        }

        const std::string sg_name = entry.path().filename().string();
        if (sg_name.rfind("sg", 0) != 0) {
            continue;
        }

        const fs::path generic_path = entry.path();
        const std::string type = ReadTextFile(generic_path / "device" / "type");
        if (!IsOpticalScsiType(type)) {
            continue;
        }

        OpticalDrive drive;
        const std::string sg_device = "/dev/" + sg_name;
        const std::optional<std::string> block_peer = FindBlockPeer(generic_path);

        drive.devicePath = WidenAscii(sg_device);
        if (block_peer) {
            drive.rootPath = WidenAscii(*block_peer);
        }

        drive.vendor = ReadTextFile(generic_path / "device" / "vendor");
        drive.product = ReadTextFile(generic_path / "device" / "model");
        drive.firmware = ReadTextFile(generic_path / "device" / "rev");
        drive.bus = DetectBus(generic_path);

        // Linux RetroBeam intentionally opens the exact SG node. No global
        // scanbus matching is needed and no non-optical /dev/sg* node is used.
        drive.cdrecordDevice = sg_device;

        if (CurrentUserCanReadWrite(sg_device)) {
            drive.mediaDescription = "Media status not queried yet";
            drive.speedQueryMessage =
                "Native Linux optical node ready for RetroBeam: " + sg_device;
            drive.advancedCapabilityMessage =
                "RetroBeam capability query pending Linux process integration.";
        } else {
            drive.mediaDescription = "Drive unavailable";
            drive.speedQueryMessage =
                "Current user cannot read/write " + sg_device +
                " (expected systemd-logind uaccess on an active desktop session).";
            drive.advancedCapabilityMessage = drive.speedQueryMessage;
        }

        drives.push_back(std::move(drive));
    }

    std::sort(
        drives.begin(),
        drives.end(),
        [](const OpticalDrive& left, const OpticalDrive& right) {
            return left.cdrecordDevice < right.cdrecordDevice;
        });

    return drives;
}

#endif // !_WIN32
