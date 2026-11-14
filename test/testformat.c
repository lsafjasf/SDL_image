#include <SDL3_image/SDL_image.h>

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdio.h>

typedef struct
{
    Uint8 *data;
    size_t size;
} ByteBuffer;

typedef struct
{
    Uint8 r, g, b, a;
} Color;

static int checks;
static int failures;
static unsigned int temp_file_counter;

#if defined(SDL_PLATFORM_WIN32) || defined(SDL_PLATFORM_OS2)
static const char pathsep[] = "\\";
#elif defined(SDL_PLATFORM_RISCOS)
static const char pathsep[] = ".";
#else
static const char pathsep[] = "/";
#endif

static bool Check(bool condition, const char *fmt, ...)
{
    va_list ap;

    ++checks;
    if (condition) {
        return true;
    }

    ++failures;
    fprintf(stderr, "FAIL: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (SDL_GetError()[0] != '\0') {
        fprintf(stderr, " (%s)", SDL_GetError());
    }
    fputc('\n', stderr);
    return false;
}

static void Skip(const char *fmt, ...)
{
    va_list ap;

    printf("SKIP: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

static char *GetTestFilename(const char *file)
{
    const char *base = SDL_getenv("SDL_TEST_SRCDIR");
    const char *base_path = NULL;
    char *path;
    size_t len;

    if (base == NULL || *base == '\0') {
        base_path = SDL_GetBasePath();
        base = base_path;
    }
    if (base == NULL) {
        return NULL;
    }

    len = SDL_strlen(base) + SDL_strlen(pathsep) + SDL_strlen(file) + 1;
    path = (char *)SDL_malloc(len);
    if (path) {
        SDL_snprintf(path, len, "%s%s%s", base,
                     (base[SDL_strlen(base) - 1] == pathsep[0]) ? "" : pathsep, file);
    }
    return path;
}

static bool ReadTestFile(const char *name, ByteBuffer *buffer)
{
    char *path = GetTestFilename(name);
    SDL_IOStream *io;
    Sint64 size;

    SDL_zerop(buffer);
    if (!path) {
        return false;
    }

    io = SDL_IOFromFile(path, "rb");
    SDL_free(path);
    if (!io) {
        return false;
    }

    size = SDL_GetIOSize(io);
    if (size <= 0 || (Uint64)size > SIZE_MAX) {
        SDL_CloseIO(io);
        return SDL_SetError("Invalid test resource size");
    }

    buffer->data = (Uint8 *)SDL_malloc((size_t)size);
    if (!buffer->data) {
        SDL_CloseIO(io);
        return false;
    }
    buffer->size = (size_t)size;

    if (SDL_ReadIO(io, buffer->data, buffer->size) != buffer->size) {
        SDL_CloseIO(io);
        SDL_free(buffer->data);
        SDL_zerop(buffer);
        return SDL_SetError("Failed to read test resource");
    }
    SDL_CloseIO(io);
    return true;
}

static bool CloneBuffer(const ByteBuffer *source, ByteBuffer *clone)
{
    clone->data = (Uint8 *)SDL_malloc(source->size);
    if (!clone->data) {
        clone->size = 0;
        return false;
    }
    SDL_memcpy(clone->data, source->data, source->size);
    clone->size = source->size;
    return true;
}

static void FreeBuffer(ByteBuffer *buffer)
{
    SDL_free(buffer->data);
    SDL_zerop(buffer);
}

static SDL_IOStream *OpenConstMem(const ByteBuffer *buffer)
{
    return SDL_IOFromConstMem(buffer->data, buffer->size);
}

static SDL_Surface *LoadSurfaceMem(const ByteBuffer *buffer)
{
    SDL_IOStream *io = OpenConstMem(buffer);
    return io ? IMG_Load_IO(io, true) : NULL;
}

static IMG_Animation *LoadAnimationMem(const ByteBuffer *buffer, const char *type)
{
    SDL_IOStream *io = OpenConstMem(buffer);
    return io ? IMG_LoadAnimationTyped_IO(io, true, type) : NULL;
}

static bool DetectMem(bool (*detect)(SDL_IOStream *), const ByteBuffer *buffer)
{
    SDL_IOStream *io = OpenConstMem(buffer);
    bool result;

    if (!io) {
        return false;
    }
    result = detect(io);
    SDL_CloseIO(io);
    return result;
}

static char *WriteTempFile(const ByteBuffer *buffer, const char *suffix)
{
    const char *base = SDL_GetBasePath();
    char name[96];
    char *path;
    size_t len;
    SDL_IOStream *io;

    if (!base) {
        return NULL;
    }

    SDL_snprintf(name, sizeof(name), "sdl-image-format-%u%s",
                 ++temp_file_counter, suffix);
    len = SDL_strlen(base) + SDL_strlen(name) + 1;
    path = (char *)SDL_malloc(len);
    if (!path) {
        return NULL;
    }
    SDL_snprintf(path, len, "%s%s", base, name);

    io = SDL_IOFromFile(path, "wb");
    if (!io) {
        SDL_free(path);
        return NULL;
    }
    if (SDL_WriteIO(io, buffer->data, buffer->size) != buffer->size) {
        SDL_CloseIO(io);
        SDL_RemovePath(path);
        SDL_free(path);
        return NULL;
    }
    SDL_CloseIO(io);
    return path;
}

static Uint32 ReadU32BE(const Uint8 *p)
{
    return ((Uint32)p[0] << 24) | ((Uint32)p[1] << 16) | ((Uint32)p[2] << 8) | p[3];
}

static Uint32 ReadU32LE(const Uint8 *p)
{
    return ((Uint32)p[3] << 24) | ((Uint32)p[2] << 16) | ((Uint32)p[1] << 8) | p[0];
}

static void WriteU32BE(Uint8 *p, Uint32 value)
{
    p[0] = (Uint8)(value >> 24);
    p[1] = (Uint8)(value >> 16);
    p[2] = (Uint8)(value >> 8);
    p[3] = (Uint8)value;
}

static void WriteU16LE(Uint8 *p, Uint16 value)
{
    p[0] = (Uint8)value;
    p[1] = (Uint8)(value >> 8);
}

static Sint64 FindChunk(const ByteBuffer *buffer, const char *type)
{
    size_t offset = 8;

    while (offset + 12 <= buffer->size) {
        Uint32 length = ReadU32BE(buffer->data + offset);
        if ((size_t)length > buffer->size - offset - 12) {
            return -1;
        }
        if (SDL_memcmp(buffer->data + offset + 4, type, 4) == 0) {
            return (Sint64)offset + 8;
        }
        offset += 12 + length;
    }
    return -1;
}

static Sint64 FindRIFFChunk(const ByteBuffer *buffer, const char *type)
{
    size_t offset = 12;

    while (offset + 8 <= buffer->size) {
        Uint32 length = ReadU32LE(buffer->data + offset + 4);
        if ((size_t)length > buffer->size - offset - 8) {
            return -1;
        }
        if (SDL_memcmp(buffer->data + offset, type, 4) == 0) {
            return (Sint64)offset + 8;
        }
        offset += 8 + length + (length & 1);
    }
    return -1;
}

static Sint64 FindLastGifImage(const ByteBuffer *buffer)
{
    size_t offset;
    Sint64 last_image = -1;
    Uint8 packed;

    if (buffer->size < 13) {
        return -1;
    }
    offset = 13;
    packed = buffer->data[10];
    if (packed & 0x80) {
        offset += 3u * (1u << ((packed & 0x07) + 1));
    }

    while (offset < buffer->size) {
        Uint8 block = buffer->data[offset++];

        if (block == 0x2C) {
            size_t descriptor = offset - 1;

            last_image = (Sint64)descriptor;
            if (offset + 9 > buffer->size) {
                break;
            }
            packed = buffer->data[offset + 8];
            offset += 9;
            if (packed & 0x80) {
                offset += 3u * (1u << ((packed & 0x07) + 1));
            }
            if (offset >= buffer->size) {
                break;
            }
            ++offset;
            while (offset < buffer->size) {
                Uint8 count = buffer->data[offset++];
                if (count == 0) {
                    break;
                }
                offset += count;
            }
        } else if (block == 0x21) {
            if (offset >= buffer->size) {
                break;
            }
            ++offset;
            while (offset < buffer->size) {
                Uint8 count = buffer->data[offset++];
                if (count == 0) {
                    break;
                }
                offset += count;
            }
        } else if (block == 0x3B) {
            break;
        } else {
            break;
        }
    }
    return last_image;
}

static Sint64 FindBytes(const ByteBuffer *buffer, const char *needle)
{
    size_t needle_len = SDL_strlen(needle);

    if (needle_len == 0 || needle_len > buffer->size) {
        return -1;
    }
    for (size_t i = 0; i <= buffer->size - needle_len; ++i) {
        if (SDL_memcmp(buffer->data + i, needle, needle_len) == 0) {
            return (Sint64)i;
        }
    }
    return -1;
}

static bool CheckAnimation(const char *label, IMG_Animation *anim,
                           int width, int height, int count, Uint64 delay,
                           Sint64 loop_count)
{
    static const Color colors[] = {
        { 255, 0, 0, 255 },
        { 0, 255, 0, 255 },
        { 0, 0, 255, 255 }
    };

    if (!anim) {
        return Check(false, "%s should load", label);
    }

    Check(anim->w == width, "%s width should be %d, got %d", label, width, anim->w);
    Check(anim->h == height, "%s height should be %d, got %d", label, height, anim->h);
    if (!Check(anim->count == count, "%s frame count should be %d, got %d",
               label, count, anim->count)) {
        return false;
    }

    for (int i = 0; i < anim->count; ++i) {
        SDL_Surface *frame = anim->frames[i];
        Color expected = colors[i % SDL_arraysize(colors)];
        Uint8 r, g, b, a;

        if (!Check(frame != NULL, "%s frame %d should exist", label, i)) {
            return false;
        }
        Check(anim->delays[i] == (int)delay,
              "%s frame %d delay should be %" SDL_PRIu64 ", got %d",
              label, i, delay, anim->delays[i]);
        if (!Check(SDL_ReadSurfacePixel(frame, frame->w / 2, frame->h / 2,
                                        &r, &g, &b, &a),
                   "%s frame %d should be readable", label, i)) {
            return false;
        }
        Check(r == expected.r && g == expected.g && b == expected.b && a == expected.a,
              "%s frame %d color should be %u,%u,%u,%u, got %u,%u,%u,%u",
              label, i, expected.r, expected.g, expected.b, expected.a, r, g, b, a);
    }

    if (loop_count >= 0) {
        Sint64 actual = SDL_GetNumberProperty(SDL_GetSurfaceProperties(anim->frames[0]),
                                              IMG_PROP_METADATA_LOOP_COUNT_NUMBER, -1);
        Check(actual == loop_count,
              "%s loop count should be %" SDL_PRIs64 ", got %" SDL_PRIs64,
              label, loop_count, actual);
    }
    return true;
}

static bool AnimationFormatEnabled(const char *format)
{
    char env_name[64];
    const char *value;

    SDL_snprintf(env_name, sizeof(env_name), "SDL_IMAGE_ANIM_%s", format);
    SDL_strupr(env_name);
    value = SDL_getenv(env_name);
    if (!value || *value == '\0') {
        return true;
    }
    return SDL_strcasecmp(value, "0") != 0 &&
           SDL_strcasecmp(value, "no") != 0 &&
           SDL_strcasecmp(value, "false") != 0;
}

static void TestAnimations(void)
{
    ByteBuffer gif = { 0 };
    ByteBuffer webp = { 0 };
    ByteBuffer apng = { 0 };
    ByteBuffer finite_loop = { 0 };
    IMG_Animation *anim;
    char *path;

    if (!Check(ReadTestFile("rgbrgb.gif", &gif), "read rgbrgb.gif") ||
        !Check(ReadTestFile("rgbrgb.webp", &webp), "read rgbrgb.webp") ||
        !Check(ReadTestFile("rgbrgb.png", &apng), "read rgbrgb.png")) {
        goto done;
    }

    if (AnimationFormatEnabled("GIF") && DetectMem(IMG_isGIF, &gif)) {
        path = GetTestFilename("rgbrgb.gif");
        anim = path ? IMG_LoadAnimation(path) : NULL;
        CheckAnimation("GIF", anim, 256, 256, 6, 20, -1);
        IMG_FreeAnimation(anim);
        SDL_free(path);

        anim = LoadAnimationMem(&gif, "gif");
        CheckAnimation("GIF typed memory", anim, 256, 256, 6, 20, -1);
        IMG_FreeAnimation(anim);
    } else {
        Skip("GIF animation support disabled");
    }

    if (AnimationFormatEnabled("WEBP") && DetectMem(IMG_isWEBP, &webp)) {
        path = GetTestFilename("rgbrgb.webp");
        anim = path ? IMG_LoadAnimation(path) : NULL;
        CheckAnimation("WebP", anim, 256, 256, 6, 20, -1);
        IMG_FreeAnimation(anim);
        SDL_free(path);

        anim = LoadAnimationMem(&webp, "webp");
        CheckAnimation("WebP typed memory", anim, 256, 256, 6, 20, -1);
        IMG_FreeAnimation(anim);
    } else {
        Skip("WebP animation support disabled");
    }

    if (AnimationFormatEnabled("APNG") && DetectMem(IMG_isPNG, &apng)) {
        path = GetTestFilename("rgbrgb.png");
        anim = path ? IMG_LoadAnimation(path) : NULL;
        CheckAnimation("APNG", anim, 256, 256, 6, 20, 0);
        IMG_FreeAnimation(anim);
        SDL_free(path);

        if (Check(CloneBuffer(&apng, &finite_loop), "clone APNG for loop test")) {
            Sint64 actl = FindChunk(&finite_loop, "acTL");
            if (Check(actl >= 0, "APNG should contain acTL")) {
                WriteU32BE(finite_loop.data + actl + 4, 3);
                anim = LoadAnimationMem(&finite_loop, "png");
                CheckAnimation("APNG finite loop", anim, 256, 256, 6, 20, 3);
                IMG_FreeAnimation(anim);
            }
        }
    } else {
        Skip("APNG support disabled");
    }

done:
    FreeBuffer(&gif);
    FreeBuffer(&webp);
    FreeBuffer(&apng);
    FreeBuffer(&finite_loop);
}

static void TestExtensionMismatch(void)
{
    ByteBuffer png = { 0 };
    ByteBuffer gif = { 0 };
    ByteBuffer webp = { 0 };
    ByteBuffer apng = { 0 };
    SDL_Surface *surface;
    IMG_Animation *anim;
    char *path;

    if (!Check(ReadTestFile("png16.png", &png), "read png16.png for mismatch") ||
        !Check(ReadTestFile("rgbrgb.gif", &gif), "read rgbrgb.gif for mismatch") ||
        !Check(ReadTestFile("rgbrgb.webp", &webp), "read rgbrgb.webp for mismatch") ||
        !Check(ReadTestFile("rgbrgb.png", &apng), "read rgbrgb.png for mismatch")) {
        goto done;
    }

    if (DetectMem(IMG_isPNG, &png)) {
        path = WriteTempFile(&png, ".gif");
        if (Check(path != NULL, "write PNG data with .gif extension")) {
            surface = IMG_Load(path);
            Check(surface != NULL, "IMG_Load should detect PNG content with .gif extension");
            if (surface) {
                Check(surface->w == 2 && surface->h == 2,
                      "mismatched PNG dimensions should be 2x2");
            }
            SDL_DestroySurface(surface);
            SDL_RemovePath(path);
            SDL_free(path);
        }
    }

    if (DetectMem(IMG_isGIF, &gif)) {
        path = WriteTempFile(&gif, ".png");
        if (Check(path != NULL, "write GIF data with .png extension")) {
            anim = IMG_LoadAnimation(path);
            CheckAnimation("GIF with .png extension", anim, 256, 256, 6, 20, -1);
            IMG_FreeAnimation(anim);

            surface = IMG_Load(path);
            Check(surface != NULL, "IMG_Load should detect GIF content with .png extension");
            if (surface) {
                Check(surface->w == 256 && surface->h == 256,
                      "mismatched GIF dimensions should be 256x256");
            }
            SDL_DestroySurface(surface);
            SDL_RemovePath(path);
            SDL_free(path);
        }
    }

    if (DetectMem(IMG_isWEBP, &webp)) {
        path = WriteTempFile(&webp, ".gif");
        if (Check(path != NULL, "write WebP data with .gif extension")) {
            anim = IMG_LoadAnimation(path);
            CheckAnimation("WebP with .gif extension", anim, 256, 256, 6, 20, -1);
            IMG_FreeAnimation(anim);
            SDL_RemovePath(path);
            SDL_free(path);
        }
    }

    if (DetectMem(IMG_isPNG, &apng)) {
        path = WriteTempFile(&apng, ".gif");
        if (Check(path != NULL, "write APNG data with .gif extension")) {
            anim = IMG_LoadAnimation(path);
            CheckAnimation("APNG with .gif extension", anim, 256, 256, 6, 20, 0);
            IMG_FreeAnimation(anim);
            SDL_RemovePath(path);
            SDL_free(path);
        }
    }

done:
    FreeBuffer(&png);
    FreeBuffer(&gif);
    FreeBuffer(&webp);
    FreeBuffer(&apng);
}

static void ExpectSurfaceFailure(const ByteBuffer *buffer, const char *label)
{
    SDL_Surface *surface;

    SDL_ClearError();
    surface = LoadSurfaceMem(buffer);
    Check(surface == NULL, "%s should fail", label);
    Check(SDL_GetError()[0] != '\0', "%s should set an error", label);
    SDL_DestroySurface(surface);
}

static void ExpectAnimationFailure(const ByteBuffer *buffer, const char *type,
                                   const char *label)
{
    IMG_Animation *anim;

    SDL_ClearError();
    anim = LoadAnimationMem(buffer, type);
    Check(anim == NULL, "%s should fail", label);
    Check(SDL_GetError()[0] != '\0', "%s should set an error", label);
    IMG_FreeAnimation(anim);
}

static void TestFailuresAndRecovery(void)
{
    ByteBuffer png = { 0 };
    ByteBuffer gif = { 0 };
    ByteBuffer webp = { 0 };
    ByteBuffer apng = { 0 };
    ByteBuffer bad = { 0 };
    ByteBuffer view;
    IMG_Animation *anim;
    SDL_Surface *surface;
    Sint64 offset;

    if (!Check(ReadTestFile("png16.png", &png), "read png16.png for recovery") ||
        !Check(ReadTestFile("rgbrgb.gif", &gif), "read rgbrgb.gif for recovery") ||
        !Check(ReadTestFile("rgbrgb.webp", &webp), "read rgbrgb.webp for recovery") ||
        !Check(ReadTestFile("rgbrgb.png", &apng), "read rgbrgb.png for recovery")) {
        goto done;
    }

    view.data = png.data;
    view.size = 4;
    SDL_ClearError();
    Check(!DetectMem(IMG_isPNG, &view), "four-byte PNG prefix should not be detected as PNG");
    ExpectSurfaceFailure(&view, "four-byte PNG prefix load");

    view.size = png.size / 2;
    Check(DetectMem(IMG_isPNG, &view), "truncated PNG should still be detected by signature");
    ExpectSurfaceFailure(&view, "truncated PNG load");
    SDL_ClearError();
    surface = LoadSurfaceMem(&png);
    Check(surface != NULL, "PNG load should recover after truncation failure");
    if (surface) {
        Check(surface->w == 2 && surface->h == 2, "recovered PNG dimensions should be 2x2");
    }
    SDL_DestroySurface(surface);

    if (DetectMem(IMG_isGIF, &gif)) {
        offset = FindLastGifImage(&gif);
        if (Check(offset >= 0, "GIF should contain an image descriptor")) {
            view.data = gif.data;
            view.size = (size_t)(offset + 10);
            Check(DetectMem(IMG_isGIF, &view), "truncated GIF should still be detected");
            ExpectAnimationFailure(&view, "gif", "GIF truncated in last frame");
        }

        if (Check(CloneBuffer(&gif, &bad), "clone GIF for bad loop test")) {
            offset = FindBytes(&bad, "NETSCAPE2.0");
            if (Check(offset >= 0 && (size_t)(offset + 16) < bad.size,
                      "GIF should contain a Netscape loop extension")) {
                bad.data[offset + 15] = 1;
                ExpectAnimationFailure(&bad, "gif", "GIF with malformed loop extension");
                ExpectSurfaceFailure(&bad, "GIF image with malformed loop extension");
            }
            FreeBuffer(&bad);
        }

        SDL_ClearError();
        anim = LoadAnimationMem(&gif, "gif");
        CheckAnimation("GIF recovery", anim, 256, 256, 6, 20, -1);
        IMG_FreeAnimation(anim);
    }

    if (DetectMem(IMG_isWEBP, &webp)) {
        view.data = webp.data;
        view.size = webp.size - 8;
        Check(DetectMem(IMG_isWEBP, &view), "truncated WebP should still be detected");
        ExpectAnimationFailure(&view, "webp", "truncated WebP animation");

        if (Check(CloneBuffer(&webp, &bad), "clone WebP for oversized test")) {
            offset = FindRIFFChunk(&bad, "VP8X");
            if (Check(offset >= 0 && (size_t)(offset + 10) <= bad.size,
                      "WebP should contain a VP8X chunk")) {
                SDL_memset(bad.data + offset + 4, 0xff, 3);
                SDL_memset(bad.data + offset + 7, 0xff, 3);
                ExpectAnimationFailure(&bad, "webp", "WebP with oversized canvas");
            }
            FreeBuffer(&bad);
        }

        SDL_ClearError();
        anim = LoadAnimationMem(&webp, "webp");
        CheckAnimation("WebP recovery", anim, 256, 256, 6, 20, -1);
        IMG_FreeAnimation(anim);
    }

    if (DetectMem(IMG_isPNG, &apng) && AnimationFormatEnabled("APNG")) {
        if (Check(CloneBuffer(&apng, &bad), "clone APNG for missing frame test")) {
            offset = FindChunk(&bad, "acTL");
            if (Check(offset >= 0, "APNG should contain acTL")) {
                WriteU32BE(bad.data + offset, 7);
                ExpectAnimationFailure(&bad, "png", "APNG with a missing frame");
            }
            FreeBuffer(&bad);
        }

        if (Check(CloneBuffer(&apng, &bad), "clone APNG for bad acTL test")) {
            offset = FindChunk(&bad, "acTL");
            if (Check(offset >= 8, "APNG should contain acTL")) {
                WriteU32BE(bad.data + offset - 8, 4);
                ExpectAnimationFailure(&bad, "png", "APNG with malformed acTL");
            }
            FreeBuffer(&bad);
        }

        if (Check(CloneBuffer(&apng, &bad), "clone APNG for oversized test")) {
            offset = FindChunk(&bad, "IHDR");
            if (Check(offset >= 0, "APNG should contain IHDR")) {
                WriteU32BE(bad.data + offset, 0x7fffffff);
                WriteU32BE(bad.data + offset + 4, 0x7fffffff);
                ExpectAnimationFailure(&bad, "png", "APNG with oversized dimensions");
            }
            FreeBuffer(&bad);
        }

        SDL_ClearError();
        anim = LoadAnimationMem(&apng, "png");
        CheckAnimation("APNG recovery", anim, 256, 256, 6, 20, 0);
        IMG_FreeAnimation(anim);
    }

    if (DetectMem(IMG_isGIF, &gif)) {
        if (Check(CloneBuffer(&gif, &bad), "clone GIF for oversized test")) {
            WriteU16LE(bad.data + 6, 0xffff);
            WriteU16LE(bad.data + 8, 0xffff);
            ExpectAnimationFailure(&bad, "gif", "GIF with oversized dimensions");
            FreeBuffer(&bad);
        }

        SDL_ClearError();
        anim = LoadAnimationMem(&gif, "gif");
        CheckAnimation("GIF recovery after oversized test", anim, 256, 256, 6, 20, -1);
        IMG_FreeAnimation(anim);
    }

done:
    FreeBuffer(&png);
    FreeBuffer(&gif);
    FreeBuffer(&webp);
    FreeBuffer(&apng);
    FreeBuffer(&bad);
}

static void TestPngVariants(void)
{
    static const Uint16 rgba16_expected[] = {
        0x1234, 0x5678, 0x9abc, 0xffff,
        0xffff, 0x8000, 0x0000, 0x4000,
        0x0000, 0x1111, 0x2222, 0x8000,
        0xaaaa, 0xbbbb, 0xcccc, 0x0000
    };
    static const Uint16 rgb16_expected[] = {
        0x1234, 0x5678, 0x9abc, 0xffff, 0x8000, 0x0000
    };
    static const Uint8 palette_indices[] = {
        0, 1, 2, 3, 3, 2, 1, 0
    };
    static const Color palette_colors[] = {
        { 255, 0, 0, 255 },
        { 0, 255, 0, 128 },
        { 0, 0, 255, 0 },
        { 255, 255, 255, 255 }
    };
    ByteBuffer rgba16 = { 0 };
    ByteBuffer rgb16 = { 0 };
    ByteBuffer palette = { 0 };
    SDL_Surface *surface;
    SDL_Palette *palette_data;
    char *path;

    if (!Check(ReadTestFile("png16.png", &rgba16), "read png16.png") ||
        !Check(ReadTestFile("png16-rgb.png", &rgb16), "read png16-rgb.png") ||
        !Check(ReadTestFile("palette-alpha.png", &palette), "read palette-alpha.png")) {
        goto done;
    }
    if (!DetectMem(IMG_isPNG, &rgba16)) {
        Skip("PNG support disabled");
        goto done;
    }

    SDL_ClearError();
    Check(DetectMem(IMG_isPNG, &rgba16), "IMG_isPNG should detect 16-bit RGBA");
    Check(!DetectMem(IMG_isGIF, &rgba16), "IMG_isGIF should reject 16-bit RGBA PNG");
    Check(!DetectMem(IMG_isWEBP, &rgba16), "IMG_isWEBP should reject 16-bit RGBA PNG");

    surface = LoadSurfaceMem(&rgba16);
    if (Check(surface != NULL, "IMG_Load_IO should load 16-bit RGBA PNG")) {
        Check(surface->w == 2 && surface->h == 2,
              "16-bit RGBA PNG dimensions should be 2x2, got %dx%d",
              surface->w, surface->h);
        Check(surface->format == SDL_PIXELFORMAT_RGBA64,
              "16-bit RGBA PNG should load as RGBA64");
        if (surface->format == SDL_PIXELFORMAT_RGBA64) {
            const Uint16 *pixels = (const Uint16 *)surface->pixels;
            for (int i = 0; i < (int)SDL_arraysize(rgba16_expected); ++i) {
                Check(pixels[i] == rgba16_expected[i],
                      "16-bit RGBA component %d should be 0x%04x, got 0x%04x",
                      i, rgba16_expected[i], pixels[i]);
            }
        }
    }
    SDL_DestroySurface(surface);
    SDL_ClearError();

    surface = LoadSurfaceMem(&rgb16);
    if (Check(surface != NULL, "IMG_Load_IO should load 16-bit RGB PNG")) {
        Check(surface->w == 2 && surface->h == 1,
              "16-bit RGB PNG dimensions should be 2x1, got %dx%d",
              surface->w, surface->h);
        Check(surface->format == SDL_PIXELFORMAT_RGB48,
              "16-bit RGB PNG should load as RGB48");
        if (surface->format == SDL_PIXELFORMAT_RGB48) {
            const Uint16 *pixels = (const Uint16 *)surface->pixels;
            for (int i = 0; i < (int)SDL_arraysize(rgb16_expected); ++i) {
                Check(pixels[i] == rgb16_expected[i],
                      "16-bit RGB component %d should be 0x%04x, got 0x%04x",
                      i, rgb16_expected[i], pixels[i]);
            }
        }
    }
    SDL_DestroySurface(surface);
    SDL_ClearError();

    surface = LoadSurfaceMem(&palette);
    if (Check(surface != NULL, "IMG_Load_IO should load indexed PNG with alpha")) {
        Check(surface->w == 4 && surface->h == 2,
              "palette PNG dimensions should be 4x2, got %dx%d",
              surface->w, surface->h);
        Check(surface->format == SDL_PIXELFORMAT_INDEX8,
              "palette PNG should load as INDEX8");
        palette_data = SDL_GetSurfacePalette(surface);
        if (Check(palette_data != NULL && palette_data->ncolors >= 4,
                  "palette PNG should have at least four palette entries")) {
            for (int i = 0; i < 4; ++i) {
                Check(palette_data->colors[i].r == palette_colors[i].r &&
                      palette_data->colors[i].g == palette_colors[i].g &&
                      palette_data->colors[i].b == palette_colors[i].b &&
                      palette_data->colors[i].a == palette_colors[i].a,
                      "palette entry %d should be %u,%u,%u,%u",
                      i, palette_colors[i].r, palette_colors[i].g,
                      palette_colors[i].b, palette_colors[i].a);
            }
            for (int i = 4; i < palette_data->ncolors; ++i) {
                Check(palette_data->colors[i].r == 0 &&
                      palette_data->colors[i].g == 0 &&
                      palette_data->colors[i].b == 0 &&
                      palette_data->colors[i].a == 0,
                      "unused palette entry %d should be fully initialized", i);
            }
        }
        if (surface->format == SDL_PIXELFORMAT_INDEX8) {
            const Uint8 *pixels = (const Uint8 *)surface->pixels;
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 4; ++x) {
                    Uint8 actual = pixels[y * surface->pitch + x];
                    Uint8 expected = palette_indices[y * 4 + x];
                    Check(actual == expected,
                          "palette pixel %d,%d should be index %u, got %u",
                          x, y, expected, actual);
                }
            }
        }
        {
            SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
            if (Check(rgba != NULL, "palette PNG should convert to RGBA32")) {
                for (int y = 0; y < 2; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        Color expected = palette_colors[palette_indices[y * 4 + x]];
                        Uint8 r, g, b, a;
                        Check(SDL_ReadSurfacePixel(rgba, x, y, &r, &g, &b, &a),
                              "converted palette pixel %d,%d should be readable", x, y);
                        Check(r == expected.r && g == expected.g &&
                              b == expected.b && a == expected.a,
                              "converted palette pixel %d,%d should be %u,%u,%u,%u",
                              x, y, expected.r, expected.g, expected.b, expected.a);
                    }
                }
            }
            SDL_DestroySurface(rgba);
        }
    }
    SDL_DestroySurface(surface);

    path = GetTestFilename("png16.png");
    if (Check(path != NULL, "build png16.png path")) {
        SDL_ClearError();
        surface = IMG_Load(path);
        Check(surface != NULL, "IMG_Load should load 16-bit PNG by path");
        if (surface) {
            Check(surface->w == 2 && surface->h == 2,
                  "IMG_Load 16-bit PNG dimensions should be 2x2");
        }
        SDL_DestroySurface(surface);
    }
    SDL_free(path);

done:
    FreeBuffer(&rgba16);
    FreeBuffer(&rgb16);
    FreeBuffer(&palette);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    TestPngVariants();
    TestAnimations();
    TestExtensionMismatch();
    TestFailuresAndRecovery();

    printf("testformat: %d checks, %d failed\n", checks, failures);
    SDL_Quit();
    return failures == 0 ? 0 : 1;
}
