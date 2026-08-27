// AUTO-GENERATED. DO NOT EDIT BY HAND.
// Source of truth: src/main.cpp :: DrawApp()
// Windows DrawApp SHA-256: 3fee1a41b47bcaadfd79ffc8937b411882b236cdd5a44abf4cef73a702c33925
// Linux platform substitutions: optical roots=2, BurnerMAX roots=1, D3D texture handle -> OpenGL ID.

void DrawApp(
    AppState& state,
    BurnEngine& burnEngine,
    const std::array<LinuxTexture, kConsoleProfileCount>& artworks,
    const LinuxTexture& disc) {
    const ImGuiIO& io = ImGui::GetIO();
    const BurnSnapshot burn = burnEngine.Snapshot();
    const int consoleIndex = std::clamp(
        static_cast<int>(state.selectedConsole),
        0,
        kConsoleProfileCount - 1);
    const LinuxTexture& artwork =
        artworks[static_cast<std::size_t>(consoleIndex)];
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);
    constexpr ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("Retro Burner", nullptr, windowFlags);

    ImGui::SetWindowFontScale(1.55F);
    ImGui::TextUnformatted("RETRO BURNER");
    ImGui::SetWindowFontScale(1.0F);
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", ConsoleSubtitle(state.selectedConsole));
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTable(
            "MainLayout",
            2,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Artwork", ImGuiTableColumnFlags_WidthFixed, 390.0F);
        ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();

        const float artSize = std::min(372.0F, ImGui::GetContentRegionAvail().x);
        if (artwork.IsValid()) {
            ImGui::Image(
                LinuxTextureId(artwork),
                ImVec2(artSize, artSize));
        } else {
            ImGui::Dummy(ImVec2(artSize, artSize));
            ImGui::TextDisabled("Artwork could not be loaded.");
        }

        constexpr float discSize = 142.0F;
        const float discStart = ImGui::GetCursorPosX() +
            (artSize - discSize) * 0.5F;
        ImGui::SetCursorPosX(discStart);
        const ImVec2 discTopLeft = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(discSize, discSize));
        static float discAngle = 0.0F;
        if (burn.writing) {
            discAngle = std::fmod(discAngle + io.DeltaTime * 3.4F, 6.283185307F);
        }
        DrawRotatedImage(
            disc,
            ImVec2(
                discTopLeft.x + discSize * 0.5F,
                discTopLeft.y + discSize * 0.5F),
            discSize,
            discAngle);

        if (burn.writing || burn.stage == BurnStage::Complete ||
            (burn.stage == BurnStage::Failed && burn.progress > 0.0F)) {
            const int percent = static_cast<int>(std::lround(burn.progress * 100.0F));
            char progressLabel[32]{};
            snprintf(progressLabel, sizeof(progressLabel), "%d%%", percent);
            ImGui::SetNextItemWidth(artSize);
            ImGui::ProgressBar(
                std::clamp(burn.progress, 0.0F, 1.0F),
                ImVec2(artSize, 23.0F),
                progressLabel);

            // RB_IMGBURN_STYLE_BUFFER_BARS_V84B
            if (burn.writing && burn.ringBufferPercent >= 0) {
                ImGui::TextDisabled("Buffer");
                char fifoLabel[16]{};
                snprintf(
                    fifoLabel,
                    sizeof(fifoLabel),
                    "%d%%",
                    std::clamp(burn.ringBufferPercent, 0, 100));
                ImGui::ProgressBar(
                    static_cast<float>(
                        std::clamp(burn.ringBufferPercent, 0, 100)) / 100.0F,
                    ImVec2(artSize, 15.0F),
                    fifoLabel);
            }

            if (burn.writing && burn.driveBufferPercent >= 0) {
                ImGui::TextDisabled("Device Buffer");
                char deviceLabel[16]{};
                snprintf(
                    deviceLabel,
                    sizeof(deviceLabel),
                    "%d%%",
                    std::clamp(burn.driveBufferPercent, 0, 100));
                ImGui::ProgressBar(
                    static_cast<float>(
                        std::clamp(burn.driveBufferPercent, 0, 100)) / 100.0F,
                    ImVec2(artSize, 15.0F),
                    deviceLabel);
            }

            if (burn.stage == BurnStage::Complete) {
                DrawCenteredSuccessText(artSize);
            } else {
                std::string burnDetails;
            // RB_FIXED_WIDTH_BURN_SPEED_V2
            // Reserve five columns for live optical write speed so
            // 9.x -> 10.x/48.x never moves the following status fields.
            const auto FixedBurnSpeed =
                [](const std::string& speed) {
                    if (speed.size() >= 5) {
                        return speed;
                    }
                    return std::string(
                               5 - speed.size(),
                               ' ') +
                           speed;
                };
                if (burn.writing) {
                    if (IsDvdProfile(state.selectedConsole)) {
                        // RB_COMPACT_DVD_BURN_STATUS_V84L
                        // Buffer health already has dedicated graphical bars.
                        // Keep the compact text row to the two values that are
                        // not otherwise obvious at a glance.
                        if (!burn.actualSpeed.empty()) {
                            burnDetails =
                                "Speed " +
                                FixedBurnSpeed(burn.actualSpeed);
                        }
                        if (!burn.remainingTime.empty()) {
                            if (!burnDetails.empty()) {
                                burnDetails += "  |  ";
                            }
                            burnDetails +=
                                "Remaining " +
                                burn.remainingTime;
                        }
                    } else if (state.selectedConsole ==
                               ConsoleProfile::Dreamcast) {
                        burnDetails =
                            "Session " +
                            std::to_string(burn.session) +
                            " of 2";
                        if (!burn.actualSpeed.empty()) {
                            burnDetails +=
                                "  |  " +
                                FixedBurnSpeed(burn.actualSpeed);
                        }
                // RB_UNIVERSAL_BURN_METRICS
                if (burn.ringBufferPercent >= 0) {
                    if (!burnDetails.empty()) {
                        burnDetails += "  |  ";
                    }
                    burnDetails +=
                        "FIFO / Read Buffer " +
                        (std::string(
    burn.ringBufferPercent < 10
        ? "  "
        : (burn.ringBufferPercent < 100 ? " " : "")) +
 std::to_string(burn.ringBufferPercent)) +
                        "%";
                }

                if (burn.driveBufferPercent >= 0) {
                    if (!burnDetails.empty()) {
                        burnDetails += "  |  ";
                    }
                    burnDetails +=
                        "Drive Buffer " +
                        (std::string(
    burn.driveBufferPercent < 10
        ? "  "
        : (burn.driveBufferPercent < 100 ? " " : "")) +
 std::to_string(burn.driveBufferPercent)) +
                        "%";
                } else if (burn.bufferPercent >= 0) {
                    if (!burnDetails.empty()) {
                        burnDetails += "  |  ";
                    }
                    burnDetails +=
                        "Buffer " +
                        (std::string(
    burn.bufferPercent < 10
        ? "  "
        : (burn.bufferPercent < 100 ? " " : "")) +
 std::to_string(burn.bufferPercent)) +
                        "%";
                }
                        if (burn.bufferPercent >= 0) {
                            burnDetails +=
                                "  |  buffer " +
                                (std::string(
    burn.bufferPercent < 10
        ? "  "
        : (burn.bufferPercent < 100 ? " " : "")) +
 std::to_string(burn.bufferPercent)) +
                                "%";
                        }
                    } else {
                        burnDetails = FixedBurnSpeed(burn.actualSpeed);
                        if (burn.bufferPercent >= 0) {
                            if (!burnDetails.empty()) {
                                burnDetails += "  |  ";
                            }
                            burnDetails +=
                                "buffer " +
                                (std::string(
    burn.bufferPercent < 10
        ? "  "
        : (burn.bufferPercent < 100 ? " " : "")) +
 std::to_string(burn.bufferPercent)) +
                                "%";
                        }
                    }
                } else {
                    burnDetails = "Burn failed";
                }

                const float detailWidth =
                    ImGui::CalcTextSize(burnDetails.c_str()).x;
                ImGui::SetCursorPosX(
                    ImGui::GetCursorPosX() +
                    std::max(
                        0.0F,
                        (artSize - detailWidth) * 0.5F));
                ImGui::TextUnformatted(burnDetails.c_str());
            }
        }

        ImGui::TableNextColumn();
        ImGui::PushID("Controls");

        ImGui::TextDisabled("TARGET CONSOLE");
        ImGui::BeginDisabled(burn.busy);
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::BeginCombo(
                "##TargetConsole",
                ConsoleName(state.selectedConsole))) {
            for (int index = 0; index < kConsoleProfileCount; ++index) {
                const ConsoleProfile profile =
                    static_cast<ConsoleProfile>(index);
                const bool selected =
                    profile == state.selectedConsole;

                if (ImGui::Selectable(
                        ConsoleName(profile),
                        selected)) {
                    if (state.selectedConsole != profile) {
                        state.selectedConsole = profile;
                        state.selectedCdi.clear();
                        state.selectedSpeed = 0;
                        burnEngine.Reset();

                        state.status =
                            std::string(ConsoleName(profile)) +
                            " selected. Choose an image, run Check Image, then insert a " +
                            ExpectedMediaName(profile) +
                            ".";
                    }
                }

                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        if (state.selectedConsole == ConsoleProfile::Xbox360) {
            ImGui::Spacing();
            ImGui::TextDisabled("XBOX 360 DISC FORMAT");
            ImGui::BeginDisabled(burn.busy);
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::BeginCombo(
                    "##Xbox360DiscType",
                    Xbox360DiscTypeName(state.xbox360DiscType))) {
                for (int mode = 0; mode < 2; ++mode) {
                    const Xbox360DiscType type =
                        mode == 0
                            ? Xbox360DiscType::Xgd2
                            : Xbox360DiscType::Xgd3;
                    const bool selected =
                        state.xbox360DiscType == type;
                    if (ImGui::Selectable(
                            Xbox360DiscTypeName(type),
                            selected)) {
                        state.xbox360DiscType = type;
                        state.selectedSpeed = 0;
                        burnEngine.Reset();
                        state.status =
                            type == Xbox360DiscType::Xgd3
                                ? "XGD3 selected. ABGX360 AutoFix Level 3 and verified BurnerMAX capacity are required before writing."
                                : "XGD2 selected. Use blank DVD+R DL media.";
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();

            if (state.xbox360DiscType == Xbox360DiscType::Xgd3) {
                ImGui::TextColored(
                    ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                    "XGD3 prerequisite: ABGX360 AutoFix Level 3 runs on a temporary ISO copy, then BurnerMAX expanded DVD+R DL capacity is verified.");
            }
        }

        ImGui::Spacing();

        ImGui::TextDisabled("DISC IMAGE");
        ImGui::BeginChild("CdiPath", ImVec2(0.0F, 58.0F), ImGuiChildFlags_Borders);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0F);
        if (state.selectedCdi.empty()) {
            ImGui::TextDisabled("%s", ImageHint(state.selectedConsole));
        } else {
            const std::string path = WideToUtf8(state.selectedCdi);
            ImGui::TextWrapped("%s", path.c_str());
        }
        ImGui::EndChild();

        ImGui::BeginDisabled(burn.busy);
        if (ImGui::Button("Browse...", ImVec2(128.0F, 0.0F))) {
            ShowCdiPicker(state, burnEngine);
        }
        ImGui::SameLine();
        const bool canCheck = !state.selectedCdi.empty();
        ImGui::BeginDisabled(!canCheck);
        if (ImGui::Button("Check Image", ImVec2(128.0F, 0.0F))) {
            BurnRequest request;
            request.cdiPath = state.selectedCdi;
            request.target = ToBurnTarget(state.selectedConsole);
            request.xbox360DiscType = state.xbox360DiscType;
            request.checkOnly = true;
            (void)burnEngine.Start(std::move(request));
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::TextDisabled("OPTICAL BURNER");
        const std::string drivePreview = state.drives.empty()
            ? "No optical burners found"
            : state.drives[static_cast<std::size_t>(state.selectedDrive)]
                  .DisplayName();
        ImGui::BeginDisabled(burn.busy);
        ImGui::SetNextItemWidth(-92.0F);
        if (ImGui::BeginCombo("##Burner", drivePreview.c_str())) {
            for (int index = 0; index < static_cast<int>(state.drives.size()); ++index) {
                const bool selected = index == state.selectedDrive;
                const std::string name = state.drives[static_cast<std::size_t>(index)]
                                             .DisplayName();
                if (ImGui::Selectable(name.c_str(), selected)) {
                    state.selectedDrive = index;
                    state.selectedSpeed = 0;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(82.0F, 0.0F))) {
            RefreshDrives(state);
        }
        ImGui::EndDisabled();

        const OpticalDrive* drive = state.drives.empty()
            ? nullptr
            : &state.drives[static_cast<std::size_t>(state.selectedDrive)];
        if (drive != nullptr) {
            DrawDriveDetails(*drive);
        }

        ImGui::Spacing();

        // RB_RECORDING_BACKEND_SELECTOR_V84H
        //
        // Keep backend selection visible as a first-class part of Retro Burner.
        // The project intentionally integrates multiple open-source recording
        // engines and more may be added later. Availability is profile-aware,
        // but the selector itself must not disappear.
        ImGui::TextDisabled("RECORDING BACKEND");

        const bool dvdBackendProfile =
            IsDvdProfile(state.selectedConsole);

        const bool growisofsEffective =
            dvdBackendProfile &&
            state.useGrowisofsForDvd;

        const char* backendPreview =
            growisofsEffective
                ? "growisofs"
                : "RetroBeam (default)";

        ImGui::BeginDisabled(burn.busy);
        ImGui::SetNextItemWidth(-1.0F);

        if (ImGui::BeginCombo(
                "##RecordingBackend",
                backendPreview)) {
            if (ImGui::Selectable(
                    "RetroBeam (default)",
                    !growisofsEffective)) {
                state.useGrowisofsForDvd = false;
            }

            ImGui::BeginDisabled(!dvdBackendProfile);
            if (ImGui::Selectable(
                    dvdBackendProfile
                        ? "growisofs"
                        : "growisofs (DVD profiles only)",
                    growisofsEffective)) {
                state.useGrowisofsForDvd = true;
            }
            ImGui::EndDisabled();

            ImGui::EndCombo();
        }

        ImGui::EndDisabled();

        if (!dvdBackendProfile) {
            ImGui::TextDisabled(
                "Current CD profile uses RetroBeam; growisofs becomes "
                "selectable for PS2 DVD and Xbox 360.");
        } else if (state.useGrowisofsForDvd) {
            ImGui::TextDisabled(
                "growisofs selected; console DVD writes are explicitly DAO.");
        } else {
            ImGui::TextDisabled(
                "RetroBeam selected (default); Advanced Settings apply to it.");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("WRITE SPEED");

        // RB_STAGE44A_SHARED_MEDIA_SPEED_GATE
        //
        // The mounted MMC media profile is authoritative. Console selection
        // decides which media family is acceptable; it must never decide how
        // a foreign media descriptor is formatted. In particular, a DVD-R
        // descriptor such as 22160 KB/s must never reach a CD console's list.
        const bool blankWritableMedia =
            drive != nullptr &&
            drive->mediaPresent &&
            drive->blankMediaKnown &&
            drive->blankMedia;

        const std::uint16_t mountedProfile =
            drive != nullptr
                ? drive->currentProfile
                : 0;

        const bool mountedCdR =
            blankWritableMedia &&
            mountedProfile == 0x0009;

        const bool mountedDvdRecordable =
            blankWritableMedia &&
            (mountedProfile == 0x0011 ||
             mountedProfile == 0x0015 ||
             mountedProfile == 0x0016 ||
             mountedProfile == 0x001B ||
             mountedProfile == 0x002B);

        const bool mountedDvdPlusRDl =
            blankWritableMedia &&
            mountedProfile == 0x002B;

        const bool speedMediaCompatible =
            state.selectedConsole == ConsoleProfile::PlayStation2Dvd
                ? mountedDvdRecordable
                : (state.selectedConsole == ConsoleProfile::Xbox360
                    ? mountedDvdPlusRDl
                    : mountedCdR);

        const bool dvdSpeedMode =
            mountedProfile == 0x0011 ||
            mountedProfile == 0x0015 ||
            mountedProfile == 0x0016 ||
            mountedProfile == 0x001B ||
            mountedProfile == 0x002B;

        const char* speedMediaPrompt =
            state.selectedConsole == ConsoleProfile::Xbox360
                ? "Insert blank DVD+R DL"
                : (state.selectedConsole == ConsoleProfile::PlayStation2Dvd
                    ? "Insert blank DVD-R / DVD+R / DVD-DL"
                    : "Insert blank CD-R");

        // A selection made for a previous media/profile must not survive after
        // the user inserts incompatible media or changes console profile.
        if (!speedMediaCompatible) {
            state.selectedSpeed = 0;
        }

        std::string speedPreview =
            drive == nullptr
                ? "Select optical writer"
                : (speedMediaCompatible
                    ? "Automatic (drive/media)"
                    : speedMediaPrompt);

        if (speedMediaCompatible &&
            drive != nullptr &&
            state.selectedSpeed > 0 &&
            state.selectedSpeed <=
                static_cast<int>(
                    drive->writeSpeeds.size())) {
            speedPreview =
                FormatSpeed(
                    drive->writeSpeeds[
                        static_cast<std::size_t>(
                            state.selectedSpeed - 1)],
                    dvdSpeedMode);
        }

        ImGui::BeginDisabled(
            burn.busy ||
            !speedMediaCompatible);

        ImGui::SetNextItemWidth(-1.0F);

        if (ImGui::BeginCombo(
                "##WriteSpeed",
                speedPreview.c_str())) {
            if (ImGui::Selectable(
                    "Automatic (drive/media)",
                    state.selectedSpeed == 0)) {
                state.selectedSpeed = 0;
            }

            if (drive != nullptr) {
                for (int index = 0;
                     index <
                         static_cast<int>(
                             drive->writeSpeeds.size());
                     ++index) {
                    const std::string label =
                        FormatSpeed(
                            drive->writeSpeeds[
                                static_cast<std::size_t>(
                                    index)],
                            dvdSpeedMode);

                    const bool selected =
                        state.selectedSpeed ==
                        index + 1;

                    if (ImGui::Selectable(
                            label.c_str(),
                            selected)) {
                        state.selectedSpeed =
                            index + 1;
                    }

                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
            }

            ImGui::EndCombo();
        }

        ImGui::EndDisabled();

        if (drive == nullptr) {
            ImGui::TextDisabled(
                "Connect a USB or internal CD/DVD writer, then press Refresh.");
        } else if (!speedMediaCompatible) {
            ImGui::TextDisabled(
                "%s",
                speedMediaPrompt);
        } else {
            ImGui::TextDisabled(
                "%s",
                drive->speedQueryMessage.c_str());
        }

        ImGui::Spacing();
                // RB_ADVANCED_SETTINGS_EXPLICIT_STATE_V84I
        // Advanced means opt-in: always begin a fresh RetroBurner process
        // collapsed, then preserve the user's open/closed choice normally
        // for the rest of that process.
        static bool advancedSettingsOpen = false;
        ImGui::SetNextItemOpen(
            advancedSettingsOpen,
            ImGuiCond_Always);

        const bool advancedSettingsVisible =
            ImGui::CollapsingHeader("ADVANCED SETTINGS");

        advancedSettingsOpen = advancedSettingsVisible;

        if (advancedSettingsVisible) {
            if (drive == nullptr) {
                ImGui::TextDisabled(
                    "Select an optical writer to load RetroBeam capabilities.");
            } else {
                const bool growisofsDvdSelected =
                    IsDvdProfile(state.selectedConsole) &&
                    state.useGrowisofsForDvd;

                if (growisofsDvdSelected) {
                    ImGui::TextDisabled(
                        "RetroBeam Advanced Settings are not applied while growisofs is selected.");
                }

                ImGui::BeginDisabled(growisofsDvdSelected);

                ImGui::TextDisabled(
                    "%s",
                    drive->advancedCapabilityMessage.empty()
                        ? "Capability fingerprint not available."
                        : drive->advancedCapabilityMessage.c_str());

                ImGui::BeginDisabled(burn.busy);
                if (ImGui::SmallButton("Reset advanced defaults")) {
                    state.advanced = RetroBeamAdvancedOptions{};
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("default: BURN-Free off, explicit OPC skipped");

                ImGui::BeginDisabled(burn.busy || !drive->burnFreeSupported);
                ImGui::Checkbox(
                    "BURN-Free",
                    &state.advanced.burnFree);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled(
                    drive->burnFreeSupported
                        ? "detected by current capability probe"
                        : "not detected by current capability probe");

                if (state.advanced.burnFree) {
                    ImGui::TextColored(
                        ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                        "Warning: BURN-Free allows underrun recovery/linking. "
                        "RetroBurner's default is continuous write.");
                }

                ImGui::BeginDisabled(burn.busy || !drive->forceSpeedSupported);
                ImGui::Checkbox(
                    "Force speed",
                    &state.advanced.forceSpeed);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled(
                    drive->forceSpeedSupported
                        ? "available"
                        : "not advertised");

                if (IsDvdProfile(state.selectedConsole)) {
                    ImGui::Separator();
                    ImGui::TextDisabled("OPC POLICY");
                const char* opcPreview =
                    state.advanced.opcPolicy == RetroBeamOpcPolicy::Force
                        ? "Force explicit OPC"
                        : (state.advanced.opcPolicy == RetroBeamOpcPolicy::Skip
                            ? "Skip explicit OPC (default / recommended)"
                            : "Automatic");
                ImGui::BeginDisabled(burn.busy);
                ImGui::SetNextItemWidth(-1.0F);
                if (ImGui::BeginCombo("##OpcPolicy", opcPreview)) {
                    if (ImGui::Selectable(
                            "Automatic",
                            state.advanced.opcPolicy == RetroBeamOpcPolicy::Automatic)) {
                        state.advanced.opcPolicy = RetroBeamOpcPolicy::Automatic;
                    }
                    if (ImGui::Selectable(
                            "Force explicit OPC",
                            state.advanced.opcPolicy == RetroBeamOpcPolicy::Force)) {
                        state.advanced.opcPolicy = RetroBeamOpcPolicy::Force;
                    }
                    if (ImGui::Selectable(
                            "Skip explicit OPC (default / recommended)",
                            state.advanced.opcPolicy == RetroBeamOpcPolicy::Skip)) {
                        state.advanced.opcPolicy = RetroBeamOpcPolicy::Skip;
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();

                if (drive->opcDescriptorCountKnown) {
                    ImGui::TextDisabled(
                        "OPC descriptors currently recorded: %u",
                        drive->opcDescriptorCount);
                } else {
                    ImGui::TextDisabled(
                        "OPC descriptor count: unavailable for current media.");
                }

                    ImGui::Separator();
                    ImGui::TextDisabled("MMC STREAMING / SPEED CONTROL");

                    const bool streamPolicyAvailable =
                        drive->realTimeStreamingKnown &&
                        drive->streamRecordingSupported &&
                        (drive->getPerformanceWriteSpeedSupported ||
                         drive->modePage2AWriteSpeedSupported);

                    ImGui::BeginDisabled(
                        burn.busy || !streamPolicyAvailable);
                    ImGui::Checkbox(
                        "Use explicit SET STREAMING policy",
                        &state.advanced.useStreamingPolicy);
                    ImGui::EndDisabled();

                    ImGui::TextDisabled(
                        "SET STREAMING: %s  |  SET CD SPEED: %s",
                        streamPolicyAvailable ? "available" : "not available",
                        drive->setCdSpeedSupported ? "available" : "not reported");

                    if (drive->realTimeStreamingKnown) {
                        ImGui::TextDisabled(
                            "Realtime streaming state: current=%s, persistent=%s",
                            drive->realTimeStreamingCurrent ? "on" : "off",
                            drive->realTimeStreamingPersistent ? "on" : "off");
                    }
                    ImGui::TextDisabled(
                        "Write-speed descriptors: GET PERFORMANCE=%s, Mode Page 2Ah=%s",
                        drive->getPerformanceWriteSpeedSupported ? "yes" : "no",
                        drive->modePage2AWriteSpeedSupported ? "yes" : "no");

                    if (state.advanced.useStreamingPolicy && streamPolicyAvailable) {
                        ImGui::Indent();
                        const char* rotationPreview =
                            state.advanced.streamRotation == RetroBeamStreamRotation::Cav
                                ? "CAV"
                                : "Drive/media default";
                        ImGui::BeginDisabled(burn.busy);
                        ImGui::SetNextItemWidth(-1.0F);
                        if (ImGui::BeginCombo("Rotation##Streaming", rotationPreview)) {
                            if (ImGui::Selectable(
                                    "Drive/media default",
                                    state.advanced.streamRotation == RetroBeamStreamRotation::Default)) {
                                state.advanced.streamRotation = RetroBeamStreamRotation::Default;
                            }
                            if (ImGui::Selectable(
                                    "CAV",
                                    state.advanced.streamRotation == RetroBeamStreamRotation::Cav)) {
                                state.advanced.streamRotation = RetroBeamStreamRotation::Cav;
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::Checkbox(
                            "Require exact selected streaming speed",
                            &state.advanced.streamExact);
                        ImGui::Checkbox(
                            "Restore streaming defaults after burn",
                            &state.advanced.restoreStreamingDefaults);
                        ImGui::EndDisabled();
                        ImGui::Unindent();
                    }
                }

                ImGui::Separator();
                ImGui::TextDisabled("DRIVE BUFFER");
                if (drive->driveBufferCapacityKnown) {
                    const std::string total =
                        FormatKiB(drive->driveBufferCapacityBytes);
                    const std::string available =
                        FormatKiB(drive->driveBufferAvailableBytes);
                    ImGui::Text(
                        "%s total  |  %s currently available",
                        total.c_str(),
                        available.c_str());
                } else if (drive->readBufferCapacitySupported) {
                    ImGui::TextDisabled(
                        "READ BUFFER CAPACITY supported; size was not returned during refresh.");
                } else {
                    ImGui::TextDisabled(
                        "Drive buffer capacity not reported.");
                }

                ImGui::EndDisabled();
            }
        }

        ImGui::Spacing();
        const std::string& currentStatus =
            burn.stage == BurnStage::Idle ? state.status : burn.status;
        ImGui::TextWrapped("%s", currentStatus.c_str());

        const bool dvdProfile =
            IsDvdProfile(state.selectedConsole);
        const bool xboxProfile =
            state.selectedConsole == ConsoleProfile::Xbox360;
        const bool ps2DvdProfile =
            state.selectedConsole == ConsoleProfile::PlayStation2Dvd;
        const bool ps2NeedsDualLayer =
            SelectedPs2ImageNeedsDualLayer(state);

        const bool profileSupported =
            drive == nullptr ||
            (xboxProfile
                ? drive->currentProfile == 0x002B
                : (ps2DvdProfile
                    ? (ps2NeedsDualLayer
                        ? (drive->currentProfile == 0x0015 ||
                           drive->currentProfile == 0x0016 ||
                           drive->currentProfile == 0x002B)
                        : (drive->currentProfile == 0x0011 ||
                           drive->currentProfile == 0x001B ||
                           drive->currentProfile == 0x0015 ||
                           drive->currentProfile == 0x0016 ||
                           drive->currentProfile == 0x002B))
                    : (drive->currentProfile == 0 ||
                       drive->currentProfile == 0x0009)));

        const bool blankSupported =
            drive == nullptr ||
            (dvdProfile
                ? (drive->blankMediaKnown && drive->blankMedia)
                : (!drive->blankMediaKnown || drive->blankMedia));

        const bool writerSupported =
            drive == nullptr ||
            dvdProfile ||
            !drive->cdWriteCapabilityKnown ||
            drive->canWriteCdR;

        const bool backendReady =
            drive != nullptr &&
            (dvdProfile
                ? ((!drive->rootPath.empty() ||
                    drive->devicePath.size() >= 6) &&
                   (state.useGrowisofsForDvd ||
                    !drive->cdrecordDevice.empty()))
                : !drive->cdrecordDevice.empty());

        const bool xgd3Selected =
            xboxProfile &&
            state.xbox360DiscType == Xbox360DiscType::Xgd3;

        const bool preparedXgd3ForSelection =
            xgd3Selected &&
            burn.xgd3Prepared &&
            !burn.preparedXgd3SourcePath.empty() &&
            _wcsicmp(
                burn.preparedXgd3SourcePath.c_str(),
                state.selectedCdi.c_str()) == 0;

        const bool canTestBurnerMax =
            xgd3Selected &&
            !burn.busy &&
            drive != nullptr &&
            drive->mediaPresent &&
            (!drive->rootPath.empty() || drive->devicePath.size() >= 6);

        const bool canBurn =
            !burn.busy &&
            !state.selectedCdi.empty() &&
            drive != nullptr &&
            drive->mediaPresent &&
            profileSupported &&
            blankSupported &&
            writerSupported &&
            backendReady;

        if (xgd3Selected) {
            if (preparedXgd3ForSelection && !burn.busy) {
                ImGui::TextColored(
                    ImVec4(0.42F, 0.88F, 0.52F, 1.00F),
                    "XGD3 READY: successful preflight/preparation copy is cached.");
                ImGui::TextWrapped(
                    "BURN XBOX 360 will reuse the ABGX360-verified working ISO. "
                    "The ISO copy, AutoFix and verification passes will not run again. "
                    "DVD+R DL and BurnerMAX checks are still repeated before writing.");
            } else {
                ImGui::TextColored(
                    ImVec4(1.00F, 0.72F, 0.28F, 1.00F),
                    "RECOMMENDED: run FULL XGD3 PREFLIGHT before a real burn.");
                ImGui::TextWrapped(
                    "If preflight passes, Retro Burner keeps that prepared working copy in a session cache so Burn can reuse it without repeating ABGX360.");
            }

            if (burn.busy && !burn.writing) {
                ImGui::Spacing();
                ImGui::TextDisabled("XGD3 PREPARATION");
                ImGui::TextWrapped("%s", burn.status.c_str());

                char preparationLabel[32]{};
                snprintf(
                    preparationLabel,
                    sizeof(preparationLabel),
                    "%d%%",
                    static_cast<int>(
                        std::lround(
                            std::clamp(
                                burn.progress,
                                0.0F,
                                1.0F) *
                            100.0F)));

                ImGui::ProgressBar(
                    std::clamp(burn.progress, 0.0F, 1.0F),
                    ImVec2(-1.0F, 20.0F),
                    preparationLabel);
                ImGui::Spacing();
            }

            ImGui::BeginDisabled(!canTestBurnerMax);
            if (ImGui::Button(
                    "TEST / ENABLE BURNERMAX",
                    ImVec2(-1.0F, 38.0F))) {
                if (drive != nullptr) {
                    std::wstring opticalDriveRoot = drive->rootPath;

                    (void)burnEngine.StartBurnerMaxTest(
                        std::move(opticalDriveRoot));
                }
            }
            ImGui::EndDisabled();

            if (!canTestBurnerMax && !burn.busy) {
                ImGui::TextDisabled(
                    "Select a drive with a DVD+R DL inserted to test BurnerMAX.");
            } else if (!burn.busy) {
                ImGui::TextDisabled(
                    "No disc sectors are written. The payload is verified by layer boundary and expanded writable capacity.");
            }

            ImGui::Spacing();
        }

        const char* burnButtonLabel =
            xboxProfile
                ? "BURN XBOX 360"
                : (ps2DvdProfile ? "BURN DVD" : "BURN DISC");

        ImGui::BeginDisabled(!canBurn);
        if (ImGui::Button(
                burnButtonLabel,
                ImVec2(-1.0F, 48.0F))) {
            ImGui::OpenPopup("Confirm burn");
        }
        ImGui::EndDisabled();

        if (dvdProfile) {
            ImGui::BeginDisabled(!canBurn);

            const char* dryRunLabel =
                xboxProfile
                    ? (state.xbox360DiscType == Xbox360DiscType::Xgd3
                        ? "RECOMMENDED: FULL XGD3 PREFLIGHT - NO DISC WRITE"
                        : "DRY RUN DVD+R DL - NO WRITE")
                    : "DRY RUN DVD - NO WRITE";

            if (ImGui::Button(
                    dryRunLabel,
                    ImVec2(-1.0F, 35.0F))) {
                if (drive != nullptr) {
                    BurnRequest request;
                    request.cdiPath = state.selectedCdi;
                    request.target =
                        ToBurnTarget(state.selectedConsole);
                    request.xbox360DiscType =
                        state.xbox360DiscType;
                    request.cdrecordDevice =
                        drive->cdrecordDevice;
                    request.opticalDriveRoot = drive->rootPath;
                    request.requestedSpeedX =
                        SelectedSpeedX(state, drive);
                    request.advanced =
                        EffectiveAdvancedOptions(state, drive);
                    request.useGrowisofsForDvd =
                        state.useGrowisofsForDvd;
                    request.checkOnly = false;
                    request.simulate = true;

                    (void)burnEngine.Start(
                        std::move(request));
                }
            }

            ImGui::EndDisabled();

            if (xgd3Selected && !burn.busy) {
                ImGui::TextDisabled(
                    preparedXgd3ForSelection
                        ? (state.useGrowisofsForDvd
                            ? "Prepared ISO is cached. Re-running preflight reuses it and repeats media/BurnerMAX + growisofs dry-run checks."
                            : "Prepared ISO is cached. Re-running preflight reuses it and repeats media/BurnerMAX + RetroBeam no-write checks.")
                        : (state.useGrowisofsForDvd
                            ? "Full preflight: ABGX360 AutoFix + verification, DVD+R DL/BurnerMAX checks and growisofs dry run."
                            : "Full preflight: ABGX360 AutoFix + verification, DVD+R DL/BurnerMAX checks and RetroBeam no-write preflight."));
            }
        }

        if (!canBurn && !burn.busy) {
            if (state.selectedCdi.empty()) {
                ImGui::TextDisabled(
                    "Choose a compatible disc image first.");
            } else if (drive == nullptr) {
                ImGui::TextDisabled(
                    "Connect and select an optical writer.");
            } else if (!drive->mediaPresent) {
                if (xboxProfile) {
                    ImGui::TextDisabled(
                        "Insert a blank DVD+R DL, then press Refresh.");
                } else if (ps2DvdProfile) {
                    ImGui::TextDisabled(
                        ps2NeedsDualLayer
                            ? "Insert a blank dual-layer DVD, then press Refresh."
                            : "Insert a blank DVD-R / DVD+R, then press Refresh.");
                } else {
                    ImGui::TextDisabled(
                        "Insert a blank CD-R, then press Refresh.");
                }
            } else if (!profileSupported) {
                if (xboxProfile) {
                    ImGui::TextDisabled(
                        "Xbox 360 burning requires blank DVD+R DL media.");
                } else if (ps2DvdProfile) {
                    ImGui::TextDisabled(
                        ps2NeedsDualLayer
                            ? "This PS2 image requires DVD-R DL or DVD+R DL media."
                            : "PS2 DVD supports blank DVD-R, DVD+R, DVD-R DL or DVD+R DL media.");
                } else {
                    ImGui::TextDisabled(
                        "This console profile currently requires CD-R media.");
                }
            } else if (!blankSupported) {
                ImGui::TextDisabled(
                    dvdProfile
                        ? "The inserted DVD must be positively reported as blank."
                        : "The inserted CD-R is not blank.");
            } else if (!writerSupported) {
                ImGui::TextDisabled(
                    "The selected optical drive cannot write CD-R media.");
            } else if (!backendReady) {
                ImGui::TextDisabled(
                    dvdProfile
                        ? (state.useGrowisofsForDvd
                            ? "Could not access the selected Linux DVD writer for growisofs. Press Refresh."
                            : "Could not map this drive to RetroBeam. Press Refresh and check the Burn log.")
                        : "Could not map this drive to RetroBeam. Press Refresh and check the Burn log.");
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("BURN LOG");
        ImGui::SameLine();

        ImGui::BeginDisabled(burn.log.empty());
        if (ImGui::SmallButton("Copy log")) {
            ImGui::SetClipboardText(burn.log.c_str());
        }
        ImGui::EndDisabled();

        ImGui::BeginChild(
            "BurnLogText",
            ImVec2(0.0F, 145.0F),
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_HorizontalScrollbar);

        if (burn.log.empty()) {
            ImGui::TextDisabled("CDIrip / RetroBeam / ABGX360 / BurnerMAX output will appear here.");
        } else {
            ImGui::TextUnformatted(burn.log.c_str());
        }

        ImGui::EndChild();
        state.lastBurnStage = burn.stage;

        if (ImGui::BeginPopupModal(
                "Confirm burn",
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            const char* mediaLabel =
                xboxProfile
                    ? "DVD+R DL"
                    : (ps2DvdProfile ? "DVD" : "CD-R");

            ImGui::Text(
                "This will permanently write the selected %s.",
                mediaLabel);

            ImGui::Text(
                "Target: %s",
                ConsoleName(state.selectedConsole));

            if (xboxProfile) {
                ImGui::Text(
                    "Format: %s",
                    Xbox360DiscTypeName(state.xbox360DiscType));
            }

            if (drive != nullptr) {
                ImGui::Text(
                    "Burner: %s",
                    drive->DisplayName().c_str());

                const int selectedSpeed =
                    SelectedSpeedX(state, drive);

                ImGui::Text(
                    "Speed: %s",
                    selectedSpeed == 0
                        ? (dvdProfile
                            ? "Automatic - DVD drive/media negotiates"
                            : "Automatic - firmware negotiates")
                        : (std::to_string(selectedSpeed) +
                           "x requested")
                              .c_str());

                const RetroBeamAdvancedOptions effectiveAdvanced =
                    EffectiveAdvancedOptions(state, drive);

                if (dvdProfile) {
                    ImGui::Text(
                        "Backend: %s",
                        state.useGrowisofsForDvd
                            ? "growisofs"
                            : "RetroBeam");

                    if (state.useGrowisofsForDvd) {
                        ImGui::Text(
                            "growisofs: DAO | dvd-compat | selected write speed");
                    } else {
                        ImGui::Text(
                            "RetroBeam: BURN-Free %s | Force speed %s",
                            effectiveAdvanced.burnFree ? "ON" : "OFF",
                            effectiveAdvanced.forceSpeed ? "ON" : "OFF");
                        ImGui::Text(
                            "OPC: %s | SET STREAMING: %s",
                            OpcPolicyDisplayName(effectiveAdvanced.opcPolicy),
                            effectiveAdvanced.useStreamingPolicy
                                ? (effectiveAdvanced.streamRotation == RetroBeamStreamRotation::Cav
                                    ? "CAV"
                                    : "explicit/default rotation")
                                : "automatic");

                        if (effectiveAdvanced.burnFree) {
                            ImGui::TextColored(
                                ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                                "BURN-Free is ENABLED: underrun recovery/linking is allowed.");
                        }
                    }
                } else {
                    ImGui::Text(
                        "RetroBeam: BURN-Free %s | Force speed %s",
                        effectiveAdvanced.burnFree ? "ON" : "OFF",
                        effectiveAdvanced.forceSpeed ? "ON" : "OFF");
                }

                if (!drive->blankMediaKnown) {
                    ImGui::TextColored(
                        ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                        "The drive did not report blank state; the recording backend will verify it before writing.");
                }
            }

            if (ps2DvdProfile) {
                ImGui::TextColored(
                    ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                    ps2NeedsDualLayer
                        ? "Dual-layer PS2 image selected. The inserted DL medium must have enough writable capacity."
                        : "Single-layer PS2 DVD burning is proven; DVD+R and dual-layer media are also accepted when compatible.");
            }

            if (xboxProfile) {
                if (state.xbox360DiscType == Xbox360DiscType::Xgd3) {
                    ImGui::TextColored(
                        preparedXgd3ForSelection
                            ? ImVec4(0.42F, 0.88F, 0.52F, 1.00F)
                            : ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                        preparedXgd3ForSelection
                            ? "Using the ABGX360-verified working copy from the successful preflight. ABGX360 will not repeat; BurnerMAX/media checks still run before writing."
                            : "No prepared XGD3 copy is cached. Retro Burner will create a temporary copy, run ABGX360 AutoFix + verification, then test/enable BurnerMAX before writing.");
                } else {
                    ImGui::TextColored(
                        ImVec4(1.00F, 0.62F, 0.24F, 1.00F),
                        "Xbox 360 XGD2 requires blank DVD+R DL media and uses the 1913760 layer break.");
                }
            }

            ImGui::Spacing();

            const char* confirmButtonLabel =
                xboxProfile
                    ? "Burn Xbox 360 now"
                    : (ps2DvdProfile ? "Burn DVD now" : "Burn now");

            if (ImGui::Button(
                    confirmButtonLabel,
                    ImVec2(170.0F, 0.0F))) {
                if (drive != nullptr) {
                    BurnRequest request;
                    request.cdiPath = state.selectedCdi;
                    request.target =
                        ToBurnTarget(state.selectedConsole);
                    request.xbox360DiscType =
                        state.xbox360DiscType;
                    request.cdrecordDevice =
                        drive->cdrecordDevice;
                    request.opticalDriveRoot = drive->rootPath;
                    request.requestedSpeedX =
                        SelectedSpeedX(state, drive);
                    request.advanced =
                        EffectiveAdvancedOptions(state, drive);
                    request.useGrowisofsForDvd =
                        state.useGrowisofsForDvd;
                    request.checkOnly = false;
                    request.simulate = false;

                    if (burnEngine.Start(
                            std::move(request))) {
                        ImGui::CloseCurrentPopup();
                    }
                }
            }

            ImGui::SameLine();

            if (ImGui::Button(
                    "Cancel",
                    ImVec2(120.0F, 0.0F))) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::EndTable();
    }

    ImGui::End();
}
