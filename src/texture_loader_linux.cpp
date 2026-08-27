#include "texture_loader_linux.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <cstdio>

void LinuxTexture::Reset() noexcept
{
    if (id != 0) {
        glDeleteTextures(1, &id);
        id = 0;
    }
    width = 0;
    height = 0;
}

bool LoadLinuxTexture(
    const std::filesystem::path& path,
    LinuxTexture& texture)
{
    texture.Reset();

    SDL_Surface* source =
        IMG_Load(path.c_str());

    if (source == nullptr) {
        std::fprintf(
            stderr,
            "IMG_Load failed for %s: %s\n",
            path.c_str(),
            SDL_GetError());
        return false;
    }

    SDL_Surface* rgba =
        SDL_ConvertSurface(
            source,
            SDL_PIXELFORMAT_RGBA32);

    SDL_DestroySurface(source);

    if (rgba == nullptr) {
        std::fprintf(
            stderr,
            "SDL_ConvertSurface failed for %s: %s\n",
            path.c_str(),
            SDL_GetError());
        return false;
    }

    GLuint id = 0;
    glGenTextures(1, &id);

    if (id == 0) {
        SDL_DestroySurface(rgba);
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_MIN_FILTER,
        GL_LINEAR);
    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_MAG_FILTER,
        GL_LINEAR);
    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_WRAP_S,
        GL_CLAMP_TO_EDGE);
    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_WRAP_T,
        GL_CLAMP_TO_EDGE);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(
        GL_UNPACK_ROW_LENGTH,
        rgba->pitch / 4);

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        rgba->w,
        rgba->h,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        rgba->pixels);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    texture.id = id;
    texture.width = rgba->w;
    texture.height = rgba->h;

    SDL_DestroySurface(rgba);
    return true;
}
