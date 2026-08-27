#pragma once

// RB_STAGE44K_PS2_MEDIA_ORIGIN_GATE
//
// Shared Windows/Linux PS2 image-origin probe.
//
// This does not identify games by title, filename, directory or size.
// It reads only filesystem structures stored inside the selected ISO.
//
// UDF uses an ECMA-167 Volume Recognition Sequence with an NSR descriptor
// (NSR02/NSR03). DVD UDF-bridge images also commonly carry a valid Anchor
// Volume Descriptor Pointer at logical block 256.
//
// Presence of either is strong DVD/UDF evidence and is sufficient to reject
// an image selected under the PS2-CD profile. Absence is intentionally NOT
// treated as proof that every unusual image is a genuine CD master.

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

struct Ps2IsoMediaProbeResult final {
    bool inspected = false;
    bool iso9660Present = false;
    bool udfBea = false;
    bool udfNsr02 = false;
    bool udfNsr03 = false;
    bool udfTea = false;
    bool udfAnchor256 = false;
    std::string evidence;
    std::string error;

    [[nodiscard]] bool LooksLikeDvdOrigin() const noexcept {
        return
            udfNsr02 ||
            udfNsr03 ||
            udfAnchor256;
    }
};

namespace retroburner_ps2_probe_detail {

constexpr std::uint64_t kLogicalBlockBytes = 2048ULL;

[[nodiscard]] inline bool ReadLogicalBlock(
    std::ifstream& stream,
    const std::uint64_t logicalBlock,
    std::array<unsigned char, kLogicalBlockBytes>& data)
{
    stream.clear();

    const std::uint64_t offset =
        logicalBlock *
        kLogicalBlockBytes;

    stream.seekg(
        static_cast<std::streamoff>(offset),
        std::ios::beg);

    if (!stream)
        return false;

    stream.read(
        reinterpret_cast<char*>(data.data()),
        static_cast<std::streamsize>(data.size()));

    return
        stream.gcount() ==
        static_cast<std::streamsize>(data.size());
}

[[nodiscard]] inline bool IdentifierAtByte1(
    const std::array<unsigned char, kLogicalBlockBytes>& block,
    const char (&identifier)[6])
{
    return
        std::equal(
            identifier,
            identifier + 5,
            block.begin() + 1);
}

[[nodiscard]] inline std::uint16_t Le16(
    const unsigned char* bytes)
{
    return
        static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1]) << 8);
}

[[nodiscard]] inline std::uint32_t Le32(
    const unsigned char* bytes)
{
    return
        static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

[[nodiscard]] inline bool ValidUdfAnchorAt256(
    const std::array<unsigned char, kLogicalBlockBytes>& block)
{
    // ECMA-167 descriptor tag:
    //   0..1  Tag Identifier (2 = Anchor Volume Descriptor Pointer)
    //   2..3  Descriptor Version
    //   4     Tag Checksum
    //   12..15 Tag Location
    const std::uint16_t tagIdentifier =
        Le16(block.data());

    const std::uint16_t descriptorVersion =
        Le16(block.data() + 2);

    const std::uint32_t tagLocation =
        Le32(block.data() + 12);

    if (tagIdentifier != 2 ||
        (descriptorVersion != 2 &&
         descriptorVersion != 3) ||
        tagLocation != 256) {
        return false;
    }

    unsigned checksum = 0;

    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4)
            continue;

        checksum +=
            static_cast<unsigned>(
                block[i]);
    }

    return
        static_cast<unsigned char>(
            checksum & 0xFFU) ==
        block[4];
}

inline void AppendEvidence(
    std::string& evidence,
    const std::string& item)
{
    if (!evidence.empty())
        evidence += ", ";

    evidence += item;
}

} // namespace retroburner_ps2_probe_detail

[[nodiscard]] inline Ps2IsoMediaProbeResult
ProbePs2IsoMedia(
    const std::filesystem::path& imagePath)
{
    namespace detail =
        retroburner_ps2_probe_detail;

    Ps2IsoMediaProbeResult result;

    std::error_code sizeError;
    const std::uintmax_t bytes =
        std::filesystem::file_size(
            imagePath,
            sizeError);

    if (sizeError) {
        result.error =
            "Could not determine ISO size: " +
            sizeError.message();
        return result;
    }

    if (bytes <
        17ULL *
            detail::kLogicalBlockBytes) {
        result.error =
            "ISO is too small to contain normal optical filesystem descriptors.";
        return result;
    }

    std::ifstream stream(
        imagePath,
        std::ios::binary);

    if (!stream) {
        result.error =
            "Could not open ISO for filesystem inspection.";
        return result;
    }

    const std::uint64_t logicalBlocks =
        static_cast<std::uint64_t>(
            bytes /
            detail::kLogicalBlockBytes);

    std::array<
        unsigned char,
        detail::kLogicalBlockBytes> block{};

    const std::uint64_t lastVrsBlock =
        std::min<std::uint64_t>(
            64ULL,
            logicalBlocks - 1ULL);

    for (std::uint64_t lba = 16;
         lba <= lastVrsBlock;
         ++lba) {
        if (!detail::ReadLogicalBlock(
                stream,
                lba,
                block)) {
            result.error =
                "Could not read ISO filesystem descriptor at logical block " +
                std::to_string(lba) +
                ".";
            return result;
        }

        if (detail::IdentifierAtByte1(
                block,
                "CD001")) {
            result.iso9660Present = true;
        }

        // ECMA-167/UDF VRS descriptors use structure type 0.
        if (block[0] != 0)
            continue;

        if (detail::IdentifierAtByte1(
                block,
                "BEA01")) {
            result.udfBea = true;
        } else if (
            detail::IdentifierAtByte1(
                block,
                "NSR02")) {
            result.udfNsr02 = true;
        } else if (
            detail::IdentifierAtByte1(
                block,
                "NSR03")) {
            result.udfNsr03 = true;
        } else if (
            detail::IdentifierAtByte1(
                block,
                "TEA01")) {
            result.udfTea = true;
        }
    }

    if (logicalBlocks > 256ULL) {
        if (!detail::ReadLogicalBlock(
                stream,
                256ULL,
                block)) {
            result.error =
                "Could not read ISO logical block 256 for UDF inspection.";
            return result;
        }

        result.udfAnchor256 =
            detail::ValidUdfAnchorAt256(
                block);
    }

    if (result.udfNsr02) {
        detail::AppendEvidence(
            result.evidence,
            "UDF NSR02");
    }

    if (result.udfNsr03) {
        detail::AppendEvidence(
            result.evidence,
            "UDF NSR03");
    }

    if (result.udfBea) {
        detail::AppendEvidence(
            result.evidence,
            "BEA01");
    }

    if (result.udfTea) {
        detail::AppendEvidence(
            result.evidence,
            "TEA01");
    }

    if (result.udfAnchor256) {
        detail::AppendEvidence(
            result.evidence,
            "UDF anchor at LBA 256");
    }

    if (result.evidence.empty()) {
        result.evidence =
            result.iso9660Present
                ? "ISO9660 present; no UDF/DVD markers detected"
                : "no UDF/DVD markers detected";
    }

    result.inspected = true;
    return result;
}
