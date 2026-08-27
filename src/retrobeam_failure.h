#pragma once

// RB_STAGE44L_RETROBEAM_FAILURE_SUMMARY
//
// Convert backend diagnostics into a concise message suitable for the GUI.
// The full backend output remains in Burn Log.
//
// This deliberately ignores the normal non-fatal Linux scheduling/memlock
// warnings and only returns a message for evidence that is useful as the
// terminal reason for a failed recording operation.

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace retroburner_failure_detail {

[[nodiscard]] inline std::string Lower(
    std::string_view value)
{
    std::string result(
        value.begin(),
        value.end());

    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](const unsigned char c) {
            return static_cast<char>(
                std::tolower(c));
        });

    return result;
}

[[nodiscard]] inline bool Contains(
    const std::string& lower,
    const std::string_view needle)
{
    return
        lower.find(needle) !=
        std::string::npos;
}

[[nodiscard]] inline std::string Trim(
    std::string value)
{
    while (!value.empty() &&
           (value.back() == '\r' ||
            value.back() == '\n' ||
            value.back() == ' ' ||
            value.back() == '\t')) {
        value.pop_back();
    }

    std::size_t first = 0;

    while (first < value.size() &&
           (value[first] == ' ' ||
            value[first] == '\t' ||
            value[first] == '\r' ||
            value[first] == '\n')) {
        ++first;
    }

    if (first != 0)
        value.erase(0, first);

    return value;
}

[[nodiscard]] inline std::vector<std::string>
Lines(
    std::string_view text)
{
    std::vector<std::string> lines;
    std::string current;

    for (const char c : text) {
        if (c == '\r' || c == '\n') {
            current = Trim(
                std::move(current));

            if (!current.empty())
                lines.emplace_back(
                    std::move(current));

            current.clear();
            continue;
        }

        current.push_back(c);
    }

    current = Trim(
        std::move(current));

    if (!current.empty())
        lines.emplace_back(
            std::move(current));

    return lines;
}

} // namespace retroburner_failure_detail

[[nodiscard]] inline std::string
SummarizeRetroBeamFailure(
    const std::string_view output)
{
    using namespace
        retroburner_failure_detail;

    if (output.empty())
        return {};

    const std::string lower =
        Lower(output);

    const bool invalidParameter =
        Contains(
            lower,
            "invalid field in parameter list") ||
        Contains(
            lower,
            "sense code: 0x26");

    if (Contains(
            lower,
            "cue sheet still not accepted") ||
        (Contains(
             lower,
             "cannot send cue sheet") &&
         Contains(
             lower,
             "could not write lead-in"))) {
        std::string result =
            "Drive rejected the requested CD track/lead-in layout before track data was written";

        if (invalidParameter) {
            result +=
                " (SCSI: invalid field in parameter list)";
        }

        result += ".";
        return result;
    }

    if (Contains(
            lower,
            "could not write lead-in")) {
        return
            "Drive could not write the disc lead-in.";
    }

    if (Contains(
            lower,
            "power calibration area error") ||
        Contains(
            lower,
            "power calibration error")) {
        return
            "Drive/media power calibration failed.";
    }

    if (Contains(
            lower,
            "buffer underrun")) {
        return
            "Recording stopped because of a buffer underrun.";
    }

    if ((Contains(
             lower,
             "fixat") ||
         Contains(
             lower,
             "lead-out") ||
         Contains(
             lower,
             "leadout")) &&
        (Contains(
             lower,
             "failed") ||
         Contains(
             lower,
             "cannot") ||
         Contains(
             lower,
             "could not") ||
         Contains(
             lower,
             "input/output error"))) {
        return
            "Drive could not finalise the disc.";
    }

    if (Contains(
            lower,
            "medium error")) {
        return
            "The drive reported a media error while recording.";
    }

    if (Contains(
            lower,
            "no media") ||
        Contains(
            lower,
            "no disk")) {
        return
            "The drive no longer reports usable recording media.";
    }

    if (Contains(
            lower,
            "permission denied") &&
        (Contains(
             lower,
             "/dev/sg") ||
         Contains(
             lower,
             "scsi"))) {
        return
            "RetroBeam lost permission to access the optical SCSI device.";
    }

    // Last-resort backend line. Ignore the Linux warnings that are already
    // known to be non-fatal when SG_IO itself is functioning.
    const std::vector<std::string> lines =
        Lines(output);

    for (auto it = lines.rbegin();
         it != lines.rend();
         ++it) {
        const std::string lineLower =
            Lower(*it);

        const bool benignSchedulingWarning =
            Contains(
                lineLower,
                "cannot set priority") ||
            Contains(
                lineLower,
                "cannot raise rlimit_memlock") ||
            Contains(
                lineLower,
                "high risk for buffer underruns");

        if (benignSchedulingWarning)
            continue;

        const bool looksFatal =
            Contains(
                lineLower,
                "retrobeam: cannot ") ||
            Contains(
                lineLower,
                "retrobeam: could not ") ||
            Contains(
                lineLower,
                "retrobeam: failed") ||
            Contains(
                lineLower,
                "retrobeam: input/output error") ||
            Contains(
                lineLower,
                "sense key:") ||
            Contains(
                lineLower,
                "sense code:");

        if (!looksFatal)
            continue;

        std::string result = *it;

        constexpr std::string_view prefix =
            "retrobeam:";

        if (Lower(result).rfind(
                prefix,
                0) == 0) {
            result.erase(
                0,
                prefix.size());

            result =
                Trim(
                    std::move(result));
        }

        if (!result.empty()) {
            if (result.back() != '.')
                result.push_back('.');

            return result;
        }
    }

    return {};
}
