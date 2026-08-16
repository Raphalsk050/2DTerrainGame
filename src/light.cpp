#include "light.h"

#include "light_shaders.h"

#include "rlgl.h"

// raylib's own GL loader, for one call it does not wrap.
//
// `rlComputeShaderDispatch` is `glDispatchCompute` and nothing else -- there is no
// barrier in it. Compute dispatches that touch the same buffer are not ordered
// against each other without one, and every pass here reads what the pass before it
// wrote. Left out, the result is not a crash but a field that is subtly wrong and
// differently wrong on every driver, which is the worst kind of bug to own.
//
// The header is declarations only here; the loader itself lives inside libraylib,
// which has already run it by the time a window exists.
#include "glad.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace light {
namespace {

// Which way each quadrant faces, in scene cells. The grid's own +x runs along
// `xDir`, its +y along `yDir`, and `anchor` is the scene cell the grid's origin
// stands on -- chosen at whichever corner keeps both axes inside the region.
struct Rotation {
    int xDirX, xDirY;
    int yDirX, yDirY;
};

constexpr Rotation kRotations[4] = {
    {1, 0, 0, 1},    // +x
    {0, 1, -1, 0},   // +y
    {-1, 0, 0, -1},  // -x
    {0, -1, 1, 0},   // -y
};

void SetInt(int location, int value) {
    if (location >= 0) rlSetUniform(location, &value, RL_SHADER_UNIFORM_INT, 1);
}

void SetFloat(int location, float value) {
    if (location >= 0) rlSetUniform(location, &value, RL_SHADER_UNIFORM_FLOAT, 1);
}

void SetIVec2(int location, int x, int y) {
    const int value[2] = {x, y};
    if (location >= 0) rlSetUniform(location, value, RL_SHADER_UNIFORM_IVEC2, 1);
}

void SetVec3(int location, Radiance value) {
    const float raw[3] = {value.r, value.g, value.b};
    if (location >= 0) rlSetUniform(location, raw, RL_SHADER_UNIFORM_VEC3, 1);
}

// Whole workgroups, rounded up. A kernel that guards its own bounds can be handed a
// count that overshoots; every one here does.
unsigned int Groups(int count, int local) {
    return static_cast<unsigned int>((std::max(count, 1) + local - 1) / local);
}

bool PowerOfTwo(int value) {
    return value > 0 && (value & (value - 1)) == 0;
}

unsigned int Compile(const std::string &code, const char *name) {
    const unsigned int shader = rlLoadShader(code.c_str(), RL_COMPUTE_SHADER);
    if (shader == 0) {
        TraceLog(LOG_ERROR, "RADIANCE: compute shader '%s' did not compile", name);
        return 0;
    }

    const unsigned int program = rlLoadShaderProgramCompute(shader);
    rlUnloadShader(shader);

    if (program == 0) {
        TraceLog(LOG_ERROR, "RADIANCE: compute program '%s' did not link", name);
        return 0;
    }

    return program;
}

// Everything written by a compute pass, made visible to the next one. Coarser than
// it needs to be on purpose: the passes here run a hundred times a frame and the
// difference between one barrier bit and three is not measurable, while getting the
// bit wrong is a race that only shows on somebody else's machine.
void Barrier() {
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                    GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT |
                    GL_BUFFER_UPDATE_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT);
}

} // namespace

void Medium::Resize(int newCols, int newRows) {
    cols = newCols;
    rows = newRows;

    const std::size_t count = static_cast<std::size_t>(cols) * rows;

    sigma.assign(count, 0.0f);
    albedo.assign(count, Radiance{});
    emission.assign(count, Radiance{});
}

void Medium::Clear() {
    std::fill(sigma.begin(), sigma.end(), 0.0f);
    std::fill(albedo.begin(), albedo.end(), Radiance{});
    std::fill(emission.begin(), emission.end(), Radiance{});
}

Rectangle Medium::Bounds() const {
    return {origin.x, origin.y, cols * spacing, rows * spacing};
}

void Field::Program::Locate() {
    if (id == 0) return;

    gx   = rlGetLocationUniform(id, "uGX");
    gy   = rlGetLocationUniform(id, "uGY");
    dirs = rlGetLocationUniform(id, "uDirs");
    step = rlGetLocationUniform(id, "uStep");

    here  = rlGetLocationUniform(id, "uHere");
    below = rlGetLocationUniform(id, "uBelow");
    above = rlGetLocationUniform(id, "uAbove");

    parity       = rlGetLocationUniform(id, "uParity");
    parityStride = rlGetLocationUniform(id, "uPyramidStride");

    sceneW = rlGetLocationUniform(id, "uSceneW");
    sceneH = rlGetLocationUniform(id, "uSceneH");

    xDir   = rlGetLocationUniform(id, "uXDir");
    yDir   = rlGetLocationUniform(id, "uYDir");
    anchor = rlGetLocationUniform(id, "uAnchor");
    spanX  = rlGetLocationUniform(id, "uSpanX");
    spanY  = rlGetLocationUniform(id, "uSpanY");

    skyRadiance = rlGetLocationUniform(id, "uSkyRadiance");
    skyHorizon  = rlGetLocationUniform(id, "uSkyHorizon");
    skyZenith   = rlGetLocationUniform(id, "uSkyZenith");
    skyCover    = rlGetLocationUniform(id, "uSkyCover");

    opacityGuard = rlGetLocationUniform(id, "uOpacityGuard");
}

Field::~Field() {
    // Nothing is released here. A GL object outlives its context only as a number,
    // and by the time a global's destructor runs the window may be gone; deleting
    // then is undefined at best and a crash on exit at worst. Unload() is the
    // caller's job, while there is still a context to unload into.
}

bool Field::Build() {
    if (built_) return ready_;

    built_ = true;

    const std::string prelude  = kPrelude;
    const std::string tracer   = kTracer;
    const std::string pyramid  = kPyramid;
    const std::string radiance = kRadiance;

    source_.id    = Compile(prelude + kSource, "source");
    trace_.id     = Compile(prelude + tracer + pyramid + kTrace, "trace");
    mergeUp_.id   = Compile(prelude + pyramid + kMergeUp, "merge up");
    boundary_.id  = Compile(prelude + pyramid + radiance + kBoundary, "boundary");
    mergeDown_.id = Compile(prelude + pyramid + radiance + kMergeDown, "merge down");
    finish_.id    = Compile(prelude + tracer + radiance + kFinish, "finish");
    blur_.id      = Compile(prelude + kBlur, "blur");

    ready_ = source_.id && trace_.id && mergeUp_.id && boundary_.id && mergeDown_.id &&
             finish_.id && blur_.id;

    if (!ready_) {
        TraceLog(LOG_ERROR,
                 "RADIANCE: the light will not be solved. This needs an OpenGL 4.3 context; "
                 "check that raylib was built with OPENGL_VERSION 4.3.");
        return false;
    }

    source_.Locate();
    trace_.Locate();
    mergeUp_.Locate();
    boundary_.Locate();
    mergeDown_.Locate();
    finish_.Locate();
    blur_.Locate();

    return true;
}

bool Field::Allocate(int cols, int rows) {
    if (cols == cols_ && rows == rows_) return true;

    // The probe grid is half the medium each way, and the cascades halve its columns
    // once per level -- so both halves have to be powers of two, since the two
    // quarter-turn quadrants swap which one is being halved.
    const int probeCols = cols / 2;
    const int probeRows = rows / 2;

    if (cols % 2 || rows % 2 || !PowerOfTwo(probeCols) || !PowerOfTwo(probeRows)) {
        TraceLog(LOG_ERROR,
                 "RADIANCE: region %dx%d is not usable; both halves must be powers of two",
                 cols, rows);
        return false;
    }

    Unload();

    cols_      = cols;
    rows_      = rows;
    probeCols_ = probeCols;
    probeRows_ = probeRows;

    // The pyramid's shape per quadrant. Levels 0 and 2 run the region the long way
    // and 1 and 3 across it, so the two have different level counts; the buffer is
    // sized for whichever needs more.
    int widest = 0;

    for (int rotation = 0; rotation < 4; rotation++) {
        const bool turned = (rotation & 1) != 0;

        const int gx = turned ? probeRows_ : probeCols_;
        const int gy = turned ? probeCols_ : probeRows_;

        int levels = 0;
        while ((gx >> levels) > 1) levels++;

        std::vector<Level> &stack = levels_[rotation];

        stack.clear();
        stack.reserve(levels + 1);

        int offset = 0;

        for (int n = 0; n <= levels; n++) {
            Level level;

            level.gx     = gx >> n;
            level.gy     = gy;
            level.dirs   = 2 << n;
            level.step   = 1 << n;
            level.offset = offset;
            level.count  = level.gx * (level.dirs + 1) * level.gy;

            offset += level.count;

            stack.push_back(level);
        }

        widest = std::max(widest, offset);
    }

    parityStride_ = widest;

    // Every cascade of R holds the same count by construction: halving the columns
    // and doubling the directions cancel.
    const int radianceStride = 2 * probeCols_ * probeRows_;

    const std::size_t cells = static_cast<std::size_t>(cols_) * rows_;

    const auto bytes = [](std::size_t count) {
        return static_cast<unsigned int>(count * 4 * sizeof(float));
    };

    scene_     = rlLoadShaderBuffer(static_cast<unsigned int>(cells * sizeof(Cell)), nullptr, RL_DYNAMIC_COPY);
    sourceBuf_ = rlLoadShaderBuffer(bytes(cells), nullptr, RL_DYNAMIC_COPY);
    accum_     = rlLoadShaderBuffer(bytes(cells), nullptr, RL_DYNAMIC_COPY);
    final_     = rlLoadShaderBuffer(bytes(cells), nullptr, RL_DYNAMIC_COPY);
    previous_  = final_;   // last frame's answer is this frame's scattering source

    pyramid_   = rlLoadShaderBuffer(bytes(static_cast<std::size_t>(parityStride_) * 2), nullptr, RL_DYNAMIC_COPY);
    radianceA_ = rlLoadShaderBuffer(bytes(static_cast<std::size_t>(radianceStride) * 2), nullptr, RL_DYNAMIC_COPY);
    radianceB_ = rlLoadShaderBuffer(bytes(static_cast<std::size_t>(radianceStride) * 2), nullptr, RL_DYNAMIC_COPY);

    if (!scene_ || !sourceBuf_ || !accum_ || !final_ || !pyramid_ || !radianceA_ || !radianceB_) {
        TraceLog(LOG_ERROR, "RADIANCE: could not allocate the cascade buffers");
        Unload();
        return false;
    }

    screen_.id      = rlLoadTexture(nullptr, cols_, rows_, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    screen_.width   = cols_;
    screen_.height  = rows_;
    screen_.mipmaps = 1;
    screen_.format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    // Clamped, so the last row of texels is not wrapped round to the first along the
    // edge of the region; filtered, because the field is smooth and the screen is
    // finer than the lattice.
    SetTextureFilter(screen_, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(screen_, TEXTURE_WRAP_CLAMP);

    staging_.assign(cells, Cell{});
    field_.assign(cells * 4, 0.0f);

    primed_ = false;

    TraceLog(LOG_INFO,
             "RADIANCE: %dx%d cells, %dx%d probes, %d cascades, %.1f MB",
             cols_, rows_, probeCols_, probeRows_,
             static_cast<int>(levels_[0].size()),
             (parityStride_ * 2.0 + radianceStride * 4.0 + cells * 10.0) * 16.0 / (1024.0 * 1024.0));

    return true;
}

void Field::Unload() {
    if (scene_) rlUnloadShaderBuffer(scene_);
    if (sourceBuf_) rlUnloadShaderBuffer(sourceBuf_);
    if (accum_) rlUnloadShaderBuffer(accum_);
    if (final_) rlUnloadShaderBuffer(final_);
    if (pyramid_) rlUnloadShaderBuffer(pyramid_);
    if (radianceA_) rlUnloadShaderBuffer(radianceA_);
    if (radianceB_) rlUnloadShaderBuffer(radianceB_);

    if (screen_.id != 0) rlUnloadTexture(screen_.id);

    scene_ = sourceBuf_ = accum_ = final_ = previous_ = 0;
    pyramid_ = radianceA_ = radianceB_ = 0;
    screen_ = Texture2D{};

    cols_ = rows_ = probeCols_ = probeRows_ = 0;
    primed_ = false;
}

void Field::Upload(const Medium &medium) {
    const std::size_t cells = static_cast<std::size_t>(cols_) * rows_;

    for (std::size_t cell = 0; cell < cells; cell++) {
        Cell &out = staging_[cell];

        out.albedoR = medium.albedo[cell].r;
        out.albedoG = medium.albedo[cell].g;
        out.albedoB = medium.albedo[cell].b;
        out.sigma   = medium.sigma[cell];

        out.emitR = medium.emission[cell].r;
        out.emitG = medium.emission[cell].g;
        out.emitB = medium.emission[cell].b;
        out.pad   = 0.0f;
    }

    rlUpdateShaderBuffer(scene_, staging_.data(),
                         static_cast<unsigned int>(cells * sizeof(Cell)), 0);
}

void Field::Readback() {
    // Read before anything is dispatched, so this takes the previous solve -- which
    // the GPU finished long ago -- rather than stalling on the one about to run.
    // glGetBufferSubData is synchronous, and asking it for a buffer the GPU is still
    // writing would put the whole frame behind the light.
    if (!primed_) return;

    rlReadShaderBuffer(final_, field_.data(),
                       static_cast<unsigned int>(field_.size() * sizeof(float)), 0);
}

void Field::SetGrid(const Program &program, const Level &level) const {
    SetInt(program.gx, level.gx);
    SetInt(program.gy, level.gy);
    SetInt(program.dirs, level.dirs);
    SetInt(program.step, level.step);
}

void Field::SetScene(const Program &program) const {
    SetInt(program.sceneW, cols_);
    SetInt(program.sceneH, rows_);
    SetInt(program.parityStride, parityStride_);
    SetInt(rlGetLocationUniform(program.id, "uRadianceStride"), 2 * probeCols_ * probeRows_);
    SetFloat(rlGetLocationUniform(program.id, "uBounce"), settings_.bounce ? 1.0f : 0.0f);
}

void Field::SetRotation(const Program &program, int rotation) const {
    const Rotation &turn = kRotations[rotation];

    // The corner the grid starts from is whichever one keeps both axes inside.
    const int anchorX = (turn.xDirX < 0 || turn.yDirX < 0) ? cols_ - 1 : 0;
    const int anchorY = (turn.xDirY < 0 || turn.yDirY < 0) ? rows_ - 1 : 0;

    const bool turned = (rotation & 1) != 0;

    SetIVec2(program.xDir, turn.xDirX, turn.xDirY);
    SetIVec2(program.yDir, turn.yDirX, turn.yDirY);
    SetIVec2(program.anchor, anchorX, anchorY);
    SetInt(program.spanX, turned ? rows_ : cols_);
    SetInt(program.spanY, turned ? cols_ : rows_);
}

void Field::SetSky(const Program &program) const {
    SetVec3(program.skyRadiance, settings_.sky.radiance);
    SetFloat(program.skyHorizon, settings_.sky.horizon);
    SetFloat(program.skyZenith, settings_.sky.zenith);
    SetFloat(program.skyCover, std::clamp(settings_.sky.cover, 0.0f, 1.0f));
}

void Field::Dispatch(int x, int y, int z, int localX, int localY, int localZ) const {
    rlComputeShaderDispatch(Groups(x, localX), Groups(y, localY), Groups(z, localZ));
    Barrier();
}

void Field::SolveRotation(int rotation) {
    const std::vector<Level> &stack = levels_[rotation];
    const int top = static_cast<int>(stack.size()) - 1;

    // How many levels of T are traced for real rather than joined. The paper's
    // reason for three: the error in joining two rays at an angle between them is
    // worst when the rays are shortest, and these are the shortest there are.
    const int traced = std::min(3, top + 1);

    for (int parity = 0; parity < 2; parity++) {
        // --- T_0 .. T_2, real rays ------------------------------------------
        rlEnableShader(trace_.id);
        SetScene(trace_);
        SetRotation(trace_, rotation);
        SetInt(trace_.parity, parity);

        rlBindShaderBuffer(sourceBuf_, 1);
        rlBindShaderBuffer(pyramid_, 3);

        for (int n = 0; n < traced; n++) {
            const Level &level = stack[n];

            SetGrid(trace_, level);
            SetInt(trace_.here, level.offset);

            Dispatch(level.gy, level.dirs + 1, level.gx, 32, 2, 2);

            rays_ += static_cast<long>(level.gx) * level.gy * (level.dirs + 1);
        }

        // --- T_3 .. T_N, joined ---------------------------------------------
        rlEnableShader(mergeUp_.id);
        SetScene(mergeUp_);
        SetRotation(mergeUp_, rotation);
        SetInt(mergeUp_.parity, parity);

        rlBindShaderBuffer(pyramid_, 3);

        for (int n = traced; n <= top; n++) {
            const Level &level = stack[n];

            SetGrid(mergeUp_, level);
            SetInt(mergeUp_.here, level.offset);
            SetInt(mergeUp_.below, stack[n - 1].offset);

            Dispatch(level.gy, level.dirs + 1, level.gx, 32, 2, 2);
        }

        // --- R_N, the boundary and the sky ----------------------------------
        rlEnableShader(boundary_.id);
        SetScene(boundary_);
        SetRotation(boundary_, rotation);
        SetSky(boundary_);
        SetInt(boundary_.parity, parity);

        rlBindShaderBuffer(pyramid_, 3);
        rlBindShaderBuffer(radianceA_, 4);

        {
            const Level &level = stack[top];

            SetGrid(boundary_, level);
            SetInt(boundary_.here, level.offset);

            Dispatch(level.gy, level.dirs, 1, 64, 1, 1);
        }
    }

    // --- R_N-1 .. R_0 ---------------------------------------------------------
    //
    // Ping-ponged, because a level reads the one above it while writing its own and
    // the two cannot be the same buffer. Both parities march down together so the
    // pair ends up in the same buffer for the finish, which reads both.
    unsigned int writing = radianceB_;
    unsigned int reading = radianceA_;

    for (int n = top - 1; n >= 0; n--) {
        const Level &level = stack[n];
        const Level &upper = stack[n + 1];

        rlEnableShader(mergeDown_.id);
        SetScene(mergeDown_);
        SetRotation(mergeDown_, rotation);
        SetSky(mergeDown_);
        SetGrid(mergeDown_, level);
        SetInt(mergeDown_.here, level.offset);
        SetInt(mergeDown_.above, upper.offset);

        rlBindShaderBuffer(pyramid_, 3);
        rlBindShaderBuffer(writing, 4);
        rlBindShaderBuffer(reading, 5);

        for (int parity = 0; parity < 2; parity++) {
            SetInt(mergeDown_.parity, parity);

            Dispatch(level.gy, level.dirs, level.gx, 32, 2, 2);
        }

        std::swap(writing, reading);
    }

    // R_0 is in whichever buffer was last written, which the swap has just made
    // `reading`.

    // --- back to the medium's own resolution ----------------------------------
    rlEnableShader(finish_.id);
    SetScene(finish_);
    SetRotation(finish_, rotation);
    SetSky(finish_);
    SetGrid(finish_, stack[0]);

    rlBindShaderBuffer(sourceBuf_, 1);
    rlBindShaderBuffer(reading, 5);
    rlBindShaderBuffer(accum_, 6);

    const bool turned = (rotation & 1) != 0;

    Dispatch(turned ? rows_ : cols_, turned ? cols_ : rows_, 1, 16, 16, 1);

    rays_ += static_cast<long>(cols_) * rows_ * 2;
}

void Field::Solve(const Medium &medium, const Settings &settings) {
    settings_ = settings;
    rays_     = 0;

    if (!Build()) return;

    if (medium.cols <= 0 || medium.rows <= 0) return;
    if (!Allocate(medium.cols, medium.rows)) return;

    spacing_ = medium.spacing;
    origin_  = medium.origin;

    Readback();
    Upload(medium);

    // --- the source term, and the accumulator cleared -------------------------
    rlEnableShader(source_.id);
    SetScene(source_);

    rlBindShaderBuffer(scene_, 0);
    rlBindShaderBuffer(sourceBuf_, 1);
    rlBindShaderBuffer(previous_, 2);
    rlBindShaderBuffer(accum_, 6);

    Dispatch(cols_, rows_, 1, 16, 16, 1);

    // --- the four quadrants ---------------------------------------------------
    for (int rotation = 0; rotation < 4; rotation++) {
        SolveRotation(rotation);
    }

    // --- the cross blur, the exposure, and the picture ------------------------
    rlEnableShader(blur_.id);
    SetScene(blur_);
    SetFloat(blur_.opacityGuard, settings_.opacityGuard);
    SetFloat(rlGetLocationUniform(blur_.id, "uExposure"), settings_.exposure);
    SetFloat(rlGetLocationUniform(blur_.id, "uBlur"), settings_.crossBlur ? 1.0f : 0.0f);

    rlBindShaderBuffer(scene_, 0);
    rlBindShaderBuffer(accum_, 6);
    rlBindShaderBuffer(final_, 7);
    rlBindImageTexture(screen_.id, 0, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, false);

    Dispatch(cols_, rows_, 1, 16, 16, 1);

    rlDisableShader();

    // The first solve reads itself back before returning, and every one after it
    // takes the previous frame's.
    //
    // The delay is deliberate and is what keeps the readback free: asking for a
    // buffer the GPU is still writing stalls the frame behind the light. But a
    // caller that solves once and then asks -- every one of the report modes does
    // exactly that -- would read a buffer that has never been filled and get a world
    // uniformly black, which looks like a broken solver rather than like a missing
    // frame. One stall, once.
    const bool first = !primed_;

    primed_ = true;

    if (first) Readback();
}

Radiance Field::At(Vector2 world) const {
    if (cols_ <= 0 || rows_ <= 0 || !primed_) return {};

    // Bilinear between the four nearest cells, since a cell's value belongs at its
    // centre and a caller asking about a point between two of them wants the point.
    const float u = (world.x - origin_.x) / spacing_ - 0.5f;
    const float v = (world.y - origin_.y) / spacing_ - 0.5f;

    const int i0 = static_cast<int>(std::floor(u));
    const int j0 = static_cast<int>(std::floor(v));

    const float fx = u - static_cast<float>(i0);
    const float fy = v - static_cast<float>(j0);

    const auto read = [&](int i, int j) -> Radiance {
        i = std::clamp(i, 0, cols_ - 1);
        j = std::clamp(j, 0, rows_ - 1);

        const std::size_t at = (static_cast<std::size_t>(j) * cols_ + i) * 4;

        return {field_[at], field_[at + 1], field_[at + 2]};
    };

    return read(i0, j0) * ((1.0f - fx) * (1.0f - fy)) + read(i0 + 1, j0) * (fx * (1.0f - fy)) +
           read(i0, j0 + 1) * ((1.0f - fx) * fy) + read(i0 + 1, j0 + 1) * (fx * fy);
}

float Field::LevelAt(Vector2 world) const {
    return Expose(Luminance(At(world)), settings_.exposure);
}

} // namespace light
