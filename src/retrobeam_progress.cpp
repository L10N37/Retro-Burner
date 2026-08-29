#include "retrobeam_progress.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <string_view>

namespace {

std::string Lower(std::string_view value)
{
    std::string out(value);
    std::transform(
        out.begin(), out.end(), out.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return out;
}

double UnitMultiplier(const std::string& unit)
{
    if (unit == "KB" || unit == "kB") return 1024.0;
    if (unit == "MB") return 1024.0 * 1024.0;
    if (unit == "GB") return 1024.0 * 1024.0 * 1024.0;
    if (unit == "TB") return 1024.0 * 1024.0 * 1024.0 * 1024.0;
    return 1.0;
}

std::size_t LastOf(
    const std::string& text,
    std::initializer_list<std::string_view> markers)
{
    std::size_t best = std::string::npos;

    for (std::string_view marker : markers) {
        const std::size_t p = text.rfind(marker);
        if (p != std::string::npos &&
            (best == std::string::npos || p > best)) {
            best = p;
        }
    }

    return best;
}

} // namespace

RetroBeamProgressUpdate ParseRetroBeamProgressText(
    std::string_view textView)
{
    RetroBeamProgressUpdate update;

    const std::string text(textView);
    const std::string lower = Lower(textView);

    // Stage 42C: Windows-visible recording phase parity.
    //
    // DVD RetroBeam does not always emit a literal "lead-in" line.
    // Treat the real-write setup / FIFO / OPC / cue/pregap sequence as
    // the lead-in phase until the backend announces the first track.
    const std::size_t leadIn =
        LastOf(
            lower,
            {
                "lead-in",
                "leadin",
                "starting real sao write",
                "waiting for reader process",
                "performing opc",
                "sending cue sheet",
                "writing pregap"
            });
    const std::size_t track =
        LastOf(lower, {"starting new track", "writing track"});
    const std::size_t finalise =
        LastOf(lower, {
            "fixating",
            "closing session",
            "lead-out",
            "leadout"
        });

    std::size_t latest = std::string::npos;

    if (leadIn != std::string::npos) {
        latest = leadIn;
        update.phaseStatus = "Writing Lead-In...";
    }

    if (track != std::string::npos &&
        (latest == std::string::npos || track > latest)) {
        latest = track;
        update.phaseStatus = "Writing Sectors...";
    }

    if (finalise != std::string::npos &&
        (latest == std::string::npos || finalise > latest)) {
        update.phaseStatus = "Finalising Disc...";
    }

    static const std::regex progressPattern(
        R"(Track\s+(\d+):\s+([0-9]+(?:\.[0-9]+)?)\s+of\s+([0-9]+(?:\.[0-9]+)?)\s+([kMGT]?B)\s+written)",
        std::regex::icase);

    // RB_STAGE44AD_CD_DISC_PROGRESS
    static const std::regex discProgressPattern(
        R"(\[disc\s*([0-9]+(?:\.[0-9]+)?)%\])",
        std::regex::icase);

    static const std::regex fifoPattern(
        R"(\(fifo\s*([0-9]+)%\))",
        std::regex::icase);

    static const std::regex bufferPattern(
        R"(\[buf\s*([0-9]+)%\])",
        std::regex::icase);

    static const std::regex speedPattern(
        R"(([0-9]+(?:\.[0-9]+)?)x)",
        std::regex::icase);

    for (std::sregex_iterator i(text.begin(), text.end(), progressPattern), e;
         i != e; ++i) {
        const std::smatch& m = *i;
        const double written =
            std::stod(m[2].str()) * UnitMultiplier(m[4].str());
        const double total =
            std::stod(m[3].str()) * UnitMultiplier(m[4].str());

        if (total > 0.0) {
            update.progress = static_cast<float>(
                std::clamp(written / total, 0.0, 0.999));
        }
    }

    // A CUE burn reports X/Y independently for every track. Prefer
    // RetroBeam's whole-disc CD telemetry when it is available.
    for (std::sregex_iterator i(
             text.begin(),
             text.end(),
             discProgressPattern), e;
         i != e; ++i) {
        const double percent =
            std::stod((*i)[1].str());

        update.progress =
            static_cast<float>(
                std::clamp(
                    percent / 100.0,
                    0.0,
                    0.999));
    }

    // Keep the track-writing phase alive even after the original
    // "Starting new track" text has rolled out of the progress window.
    if (update.progress >= 0.0F &&
        update.phaseStatus.empty()) {
        update.phaseStatus =
            "Writing Sectors...";
    }

    for (std::sregex_iterator i(text.begin(), text.end(), fifoPattern), e;
         i != e; ++i) {
        update.fifoPercent =
            std::clamp(std::stoi((*i)[1].str()), 0, 100);
    }

    for (std::sregex_iterator i(text.begin(), text.end(), bufferPattern), e;
         i != e; ++i) {
        update.bufferPercent =
            std::clamp(std::stoi((*i)[1].str()), 0, 100);
    }

    for (std::sregex_iterator i(text.begin(), text.end(), speedPattern), e;
         i != e; ++i) {
        update.speed = (*i)[1].str() + "x";
    }

    return update;
}
