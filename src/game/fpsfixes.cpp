// Effects the game steps once a frame, held to the console's 30 Hz.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "xdk/hle.h"

using namespace kernel;

namespace {

// Vision modes reroll their grain every frame, so it races at high fps. Each roll is held for 1/30 s.
using Clock = std::chrono::steady_clock;
constexpr auto kStep = std::chrono::microseconds(33333);

struct Roll {
    Clock::time_point next;
    bool fresh;
    double v[3];
};
Roll g_rolls[4];

// return address of a grain roll as roll * 4 + value, else -1
int Site(uint32_t ret) {
    switch (ret) {
    case 0x2F02E8: return 0;  // noise overlay: angle, v, u
    case 0x2F0343: return 1;
    case 0x2F034E: return 2;
    case 0x2F2119: return 4;
    case 0x2F213E: return 5;
    case 0x2F2149: return 6;
    case 0x2F2A6A: return 8;
    case 0x2F2A9D: return 9;
    case 0x2F2AB9: return 10;
    case 0x2F308D:
    case 0x2F309C: return 12;  // thermal distortion angle
    }
    return -1;
}

template <class T>
void Hold(int site, T& value) {
    if (site < 0) return;
    Roll& r = g_rolls[site / 4];
    if (site % 4 == 0) {
        auto now = Clock::now();
        if ((r.fresh = now >= r.next)) r.next = std::max(r.next + kStep, now);
    }
    if (r.fresh) r.v[site % 4] = double(value);
    else value = T(r.v[site % 4]);
}

// Cloth steps every frame and never by less than 0.015 s, so it speeds up past 66 fps. It steps at 30 Hz and
// the frames between blend the last two steps.
constexpr uint32_t kOwner = 0x5C, kVerts = 0x94, kCount = 0x98, kSmoothedStep = 0x17C;  // UESoftBody
constexpr uint32_t kVertSize = 0x48, kVertPos = 4;
constexpr uint32_t kLevel = 0xF4, kDeltaSeconds = 0x3F4;  // owner's LevelInfo
constexpr uint32_t kAttachments = 0xC8, kBounds = 0xCC;   // vtable
constexpr float kFixedStep = 1.0f / 30, kMaxDelta = 0.05f;

struct Cloth {
    std::vector<float> back, front, shown, now;  // last two steps, the blend drawn, the vertices as found
    float time = 0;
};
std::unordered_map<uint32_t, Cloth> g_cloths;

void Read(uint32_t body, std::vector<float>& v) {
    uint32_t pos = R32(body + kVerts) + kVertPos;
    for (size_t i = 0; i < v.size(); i += 3, pos += kVertSize) std::memcpy(&v[i], HostPtr(pos), 12);
}

void Write(uint32_t body, const std::vector<float>& v) {
    uint32_t pos = R32(body + kVerts) + kVertPos;
    for (size_t i = 0; i < v.size(); i += 3, pos += kVertSize) std::memcpy(HostPtr(pos), &v[i], 12);
}

// A texture with no MaxFrameRate moves on a frame of its animation every tick.
constexpr uint32_t kMaxFrameRate = 0xAC, kAccumulator = 0xB0;  // UTexture

// The night vision interference of the JBA missions scrolls by a fixed amount every frame.
constexpr uint32_t kScroll = 0x6A94EC, kViewport = 0x1D0, kViewActor = 0x34;
constexpr float kScrollStep = 0.005f;

// Dynamic water steps its ripples by a fixed 0.033 s every tick, so they race past 30 fps.
std::unordered_map<uint32_t, float> g_water_time;

}  // namespace

// UEDynamicWater update, thiscall, run by its water actor's tick
X86_EXTERN_FUNC(__imp__sub_00178798);
X86_FUNC(sub_00178798) {
    uint32_t water = ctx.ecx;
    float& time = g_water_time[water];
    time += std::clamp(GF(R32(R32(water + kOwner) + kLevel) + kDeltaSeconds), 0.0f, kMaxDelta);
    if (time < kFixedStep) {
        ctx.esp += 4;  // ret, skipped this frame
        return;
    }
    time = std::min(time - kFixedStep, kFixedStep);
    __imp__sub_00178798(ctx, base);
}

// night vision pass, thiscall
X86_EXTERN_FUNC(__imp__sub_002F157B);
X86_FUNC(sub_002F157B) {
    float delta = GF(R32(R32(R32(ctx.ecx + kViewport) + kViewActor) + kLevel) + kDeltaSeconds);
    float& scroll = G<float>(kScroll);
    float before = scroll;
    float ahead = scroll += kScrollStep * (1 - std::clamp(delta, 0.0f, kMaxDelta) / kFixedStep);
    __imp__sub_002F157B(ctx, base);
    if (scroll == ahead) scroll = before;  // interference off, nothing scrolled
}

// JBA night vision glitch, thiscall(LevelInfo): rolled once a frame, then 8 frames long
X86_EXTERN_FUNC(__imp__sub_001F8F9C);
X86_FUNC(sub_001F8F9C) {
    static float time;
    time += std::clamp(GF(R32(ctx.esp + 4) + kDeltaSeconds), 0.0f, kMaxDelta);
    if (time < kFixedStep) {
        ctx.esp += 8;  // ret 4, skipped this frame
        return;
    }
    time -= kFixedStep;
    __imp__sub_001F8F9C(ctx, base);
}

// UTexture::Tick, thiscall(DeltaSeconds)
X86_EXTERN_FUNC(__imp__sub_001DDA92);
X86_FUNC(sub_001DDA92) {
    float delta = GF(ctx.esp + 4);
    float& time = G<float>(ctx.ecx + kAccumulator);
    if (G<float>(ctx.ecx + kMaxFrameRate) == 0 && delta > 0) {
        time += delta;
        if (time < kFixedStep) {
            ctx.esp += 8;
            return;
        }
        time = std::min(time - kFixedStep, kFixedStep);
    }
    __imp__sub_001DDA92(ctx, base);
}

// UESoftBody::Update, thiscall
X86_EXTERN_FUNC(__imp__sub_0025E82D);
X86_FUNC(sub_0025E82D) {
    uint32_t body = ctx.ecx, ret = R32(ctx.esp);
    float& delta = G<float>(R32(R32(body + kOwner) + kLevel) + kDeltaSeconds);
    size_t n = size_t(R32(body + kCount)) * 3;
    Cloth& c = g_cloths[body];
    c.now.resize(n);
    Read(body, c.now);
    if (c.front.size() != n) c.back = c.front = c.shown = c.now;
    // whatever moved the vertices since the last blend moves both steps
    for (size_t i = 0; i < n; i++) {
        float moved = c.now[i] - c.shown[i];
        c.back[i] += moved;
        c.front[i] += moved;
    }
    Write(body, c.front);
    ctx.esp += 4;  // the return address comes off: each step calls the original with it pushed again
    if (delta > 0) c.time += std::min(delta, kMaxDelta);
    for (; c.time >= kFixedStep; c.time -= kFixedStep) {
        float frame = delta;
        delta = G<float>(body + kSmoothedStep) = kFixedStep;
        ctx.ecx = body;
        ctx.esp -= 4;
        W32(ctx.esp, ret);
        __imp__sub_0025E82D(ctx, base);
        delta = frame;
        c.back.swap(c.front);
        Read(body, c.front);
    }
    float blend = c.time / kFixedStep;
    for (size_t i = 0; i < n; i++) c.shown[i] = c.back[i] + blend * (c.front[i] - c.back[i]);
    Write(body, c.shown);
    ctx.ecx = body;
    CallGuest(ctx, R32(R32(body) + kAttachments), {});
    CallGuest(ctx, R32(R32(body) + kBounds), {});
}

// appFrand
X86_EXTERN_FUNC(__imp__sub_00067760);
X86_FUNC(sub_00067760) {
    int site = Site(R32(ctx.esp));
    __imp__sub_00067760(ctx, base);
    Hold(site, X87_ST(0));
}

// rand
X86_EXTERN_FUNC(__imp__sub_00067732);
X86_FUNC(sub_00067732) {
    int site = Site(R32(ctx.esp));
    __imp__sub_00067732(ctx, base);
    Hold(site, ctx.eax);
}
