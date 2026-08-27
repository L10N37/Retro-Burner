#pragma once

#include <SDL3/SDL_opengl.h>

#include <filesystem>

struct LinuxTexture final {
    GLuint id = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return id != 0 && width > 0 && height > 0;
    }

    void Reset() noexcept;
};

[[nodiscard]] bool LoadLinuxTexture(
    const std::filesystem::path& path,
    LinuxTexture& texture);
