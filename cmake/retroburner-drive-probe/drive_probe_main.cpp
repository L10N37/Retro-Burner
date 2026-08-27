#include "drive_manager.h"

#include <iostream>
#include <string>
#include <vector>

static std::string NarrowAscii(const std::wstring& value) {
    return std::string(value.begin(), value.end());
}

int main() {
    const std::vector<OpticalDrive> drives = EnumerateOpticalDrives();

    std::cout << "Retro Burner Linux drive-manager probe\n";
    std::cout << "optical_count=" << drives.size() << "\n";

    for (std::size_t i = 0; i < drives.size(); ++i) {
        const OpticalDrive& drive = drives[i];
        std::cout << "\n[" << i << "]\n";
        std::cout << "display=" << drive.DisplayName() << "\n";
        std::cout << "sg=" << NarrowAscii(drive.devicePath) << "\n";
        std::cout << "block=" << NarrowAscii(drive.rootPath) << "\n";
        std::cout << "retrobeam_device=" << drive.cdrecordDevice << "\n";
        std::cout << "vendor=" << drive.vendor << "\n";
        std::cout << "product=" << drive.product << "\n";
        std::cout << "firmware=" << drive.firmware << "\n";
        std::cout << "bus=" << drive.bus << "\n";
        std::cout << "status=" << drive.speedQueryMessage << "\n";
    }

    return drives.empty() ? 2 : 0;
}
