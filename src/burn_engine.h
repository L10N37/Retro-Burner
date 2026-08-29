#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

enum class BurnStage {
    Idle,
    Preparing,
    Ready,
    BurningSession1,
    BurningSession2,
    Complete,
    Failed,
};

enum class Xbox360DiscType {
    Xgd2,
    Xgd3,
};

enum class BurnTarget {
    Dreamcast,
    PlayStation,
    PlayStation2Cd,
    PlayStation2Dvd,
    Saturn,
    Xbox360,
};

enum class RetroBeamOpcPolicy {
    Automatic,
    Force,
    Skip,
};

enum class RetroBeamStreamRotation {
    Default,
    Cav,
};

struct RetroBeamAdvancedOptions final {
    // RetroBurner prefers an uninterrupted physical recording pass.
    // BURN-Free is opt-in only; explicit OPC is skipped by default.
    bool burnFree = false;
    bool forceSpeed = false;
    RetroBeamOpcPolicy opcPolicy = RetroBeamOpcPolicy::Skip;

    // Explicit MMC SET STREAMING policy. When disabled, RetroBeam chooses the
    // normal speed-control path for the selected drive/media.
    bool useStreamingPolicy = false;
    RetroBeamStreamRotation streamRotation = RetroBeamStreamRotation::Default;
    bool streamExact = false;
    bool restoreStreamingDefaults = true;
};

struct BurnRequest final {
    // Kept as cdiPath for compatibility with the existing UI/state while
    // Retro Burner grows beyond CDI. It now contains the selected image path.
    std::wstring cdiPath;
    BurnTarget target = BurnTarget::Dreamcast;
    Xbox360DiscType xbox360DiscType = Xbox360DiscType::Xgd2;
    std::string cdrecordDevice; // Legacy field name: now maps to RetroBeam/libscg.
    std::wstring opticalDriveRoot; // e.g. L"K:" for native Windows DVD tools.
    int requestedSpeedX = 0; // 0 lets the drive/media choose.
    RetroBeamAdvancedOptions advanced;

    // DVD backend selector. RetroBeam is the default; growisofs remains
    // available for A/B testing and drive/media compatibility.
    bool useGrowisofsForDvd = false;

    // RB_STAGE44M_OPTIONAL_VERIFY_FLAG
    //
    // Optional, explicit post-burn full logical-sector readback.
    // OFF by default. The 0.5.0 UI currently exposes this only for
    // single-track PS2 CD .iso burns, whose 2048-byte logical sectors
    // can be compared directly with the source ISO on both OSes.
    bool verifyAfterBurn = false;

    bool checkOnly = false;
    bool simulate = false;   // no-write preflight for DVD targets.
    bool burnerMaxOnly = false; // Test/enable BurnerMAX without writing disc sectors.
};

struct BurnSnapshot final {
    BurnStage stage = BurnStage::Idle;
    bool busy = false;
    bool writing = false;
    float progress = 0.0F;
    int session = 0;
    int bufferPercent = -1;
    int ringBufferPercent = -1;
    int driveBufferPercent = -1;
    std::string actualSpeed;
    std::string remainingTime;
    std::string layout;
    std::string status = "Choose a disc image.";
    std::string log;
    bool xgd3Prepared = false;
    std::wstring preparedXgd3SourcePath;
    std::wstring preparedXgd3WorkingPath;
};

class BurnEngine final {
public:
    BurnEngine() = default;
    ~BurnEngine();

    BurnEngine(const BurnEngine&) = delete;
    BurnEngine& operator=(const BurnEngine&) = delete;

    [[nodiscard]] bool Start(BurnRequest request);

    // Dedicated no-image, no-disc-sector-write BurnerMAX action.
    // This intentionally bypasses the generic image-validation/burn path.
    [[nodiscard]] bool StartBurnerMaxTest(
        std::wstring opticalDriveRoot);

    [[nodiscard]] BurnSnapshot Snapshot() const;

    // RB_STAGE44X_UI_BURN_SIMULATION
    // Engineering-only synthetic snapshot injection. This does not launch
    // any backend or access an optical device.
    void InjectUiSimulationSnapshot(
        const BurnSnapshot& snapshot) {
        std::lock_guard lock(mutex_);
        state_ = snapshot;
    }

    void Reset();

    // Used by XGD3 preparation callbacks to surface copy/ABGX360 progress.
    void SetPreparationProgress(
        float progress,
        std::string status);

private:
    void Run(BurnRequest request);
    void RunStandardImage(
        BurnRequest request,
        const std::wstring& retrobeamPath,
        const std::wstring& growisofsPath,
        const std::wstring& dvdMediaInfoPath,
        const std::wstring& abgx360Path);
    void RunBurnerMaxOnly(
        BurnRequest request,
        const std::wstring& dvdMediaInfoPath);
    void SetFailure(std::string message);
    void AppendLog(const std::string& text);
    void ClearPreparedXgd3();
    [[nodiscard]] bool TryReusePreparedXgd3(
        const std::wstring& sourcePath,
        std::wstring& workingImagePath);
    void StorePreparedXgd3(
        const std::wstring& sourcePath,
        const std::wstring& workingDirectory,
        const std::wstring& workingImagePath);

    struct PreparedXgd3Cache final {
        bool valid = false;
        std::wstring sourcePath;
        std::wstring workingDirectory;
        std::wstring workingImagePath;
        std::uint64_t sourceBytes = 0;
        std::uint64_t sourceWriteTime = 0;
    };

    mutable std::mutex mutex_;
    BurnSnapshot state_;
    // Pending CR lets us distinguish a split CRLF from a live-line rewrite.
    bool logPendingCarriageReturn_ = false;
    PreparedXgd3Cache preparedXgd3_;
    std::jthread worker_;
};
