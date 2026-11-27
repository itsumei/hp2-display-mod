#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

namespace {

// Direct3D 8 ABI, used by both native D3D8 and the bundled D3D8-to-D3D9 wrapper.
struct DisplayMode {
    UINT width, height, refreshHz;
    DWORD format;
};

struct PresentParameters {
    UINT width, height;
    DWORD format;
    UINT bufferCount;
    DWORD multisample, swapEffect;
    HWND window;
    BOOL windowed, autoDepthStencil;
    DWORD depthStencilFormat, flags;
    UINT refreshHz, interval;
};

struct DeviceCreation {
    UINT adapter;
    DWORD deviceType;
    HWND focusWindow;
    DWORD flags;
};

static_assert(sizeof(PresentParameters) == 52 && sizeof(void*) == 4);

constexpr DWORD CopyVsync = 4;
constexpr DWORD DiscardSwap = 1;
constexpr DWORD LockableBackbuffer = 1;
constexpr UINT IntervalOne = 1;
constexpr UINT IntervalImmediate = 0x80000000;
constexpr UINT AutoRefresh = (std::numeric_limits<UINT>::max)();
constexpr HRESULT DeviceLost = static_cast<HRESULT>(0x88760868);

template<class Method>
Method ComMethod(void* object, unsigned index) {
    return reinterpret_cast<Method>((*static_cast<void***>(object))[index]);
}

using CreateDevice = HRESULT (WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PresentParameters*, void**);
using ResetDevice = HRESULT (WINAPI*)(void*, PresentParameters*);
using PresentFrame = HRESULT (WINAPI*)(void*, const RECT*, const RECT*, HWND, const RGNDATA*);

HMODULE pluginModule;
BYTE* gameImage;
const DWORD* gameTick;
const float* frontEndAspect;
wchar_t logPath[MAX_PATH];
INIT_ONCE settingsOnce = INIT_ONCE_STATIC_INIT;
UINT maxRefreshHz = AutoRefresh;
int vsync = 1;
UINT sampleSeconds = 5;
UINT frameTraceSeconds;
UINT msaaSamples;
bool showFps;
LARGE_INTEGER counterFrequency;
bool raceInterpolation;
bool InstallRaceHooks(BYTE* image);

constexpr double SimulationHz = 60.0;

struct Vector3 {
    float x, y, z;
};

struct CameraPose {
    Vector3 eye, target, up;
};

struct CarRotation {
    Vector3 axis[3];
};

struct CarPose {
    Vector3 position;
    CarRotation rotation;
};

static_assert(sizeof(CameraPose) == 36 && sizeof(CarPose) == 48);

template<class Pose>
struct PoseHistory {
    DWORD tick;
    Pose previous, current;
    bool tracked;
    bool hasPrevious;
};

struct RenderClock {
    DWORD tick;
    double tickStart;
    LONGLONG lastFrame;
    bool tracked;
};

struct RenderFrame {
    DWORD tick;
    float alpha;
    bool reset;
};

struct CarHistory {
    const void* renderer;
    const void* car;
    PoseHistory<CarPose> pose;
};

RenderClock raceClock;
PoseHistory<CameraPose> cameraHistory;
CarHistory cars[128];
const BYTE* cameraOwner;
DWORD cameraController;
const BYTE* lastDirector;
const BYTE* raceDirector;
const BYTE* playerCar;
RenderFrame raceFrame;
bool cameraBlendedThisFrame;
bool playerBlendedThisFrame;
UINT carsBlendedThisFrame;

void ClearRaceHistory() {
    raceClock = {};
    cameraHistory = {};
    lastDirector = nullptr;
    memset(cars, 0, sizeof(cars));
}

RenderFrame SampleRenderClock(RenderClock& clock, DWORD tick, LONGLONG now, LONGLONG frequency) {
    const double period = frequency / SimulationHz;
    // Drop stale poses after a pause, a skipped tick, or a clock discontinuity.
    if (!clock.tracked || tick < clock.tick || tick - clock.tick > 1 ||
        now < clock.lastFrame || now - clock.lastFrame > frequency / 12 ||
        now - clock.tickStart > frequency / 12) {
        clock = {tick, static_cast<double>(now), now, true};
        return {tick, 1.0f, true};
    }
    if (clock.tick != tick) {
        // The latest simulation step occurred between the two render samples.
        // Keep the nominal cadence unless it falls outside that observed interval.
        double next = clock.tickStart + period;
        clock.tickStart = next < clock.lastFrame ? static_cast<double>(clock.lastFrame) :
                          next > now ? static_cast<double>(now) : next;
        clock.tick = tick;
    }
    clock.lastFrame = now;
    double alpha = (now - clock.tickStart) / period;
    return {tick, alpha < 1.0 ? static_cast<float>(alpha) : 1.0f, false};
}

Vector3 Difference(Vector3 a, Vector3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

float Dot(Vector3 a, Vector3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3 Blend(Vector3 a, Vector3 b, float fraction) {
    return {a.x + (b.x - a.x) * fraction, a.y + (b.y - a.y) * fraction,
            a.z + (b.z - a.z) * fraction};
}

Vector3 Scale(Vector3 v, float factor) {
    return {v.x * factor, v.y * factor, v.z * factor};
}

Vector3 Cross(Vector3 a, Vector3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

bool IsContinuous(const CameraPose& previous, const CameraPose& current) {
    Vector3 jump = Difference(current.eye, previous.eye);
    Vector3 before = Difference(previous.target, previous.eye);
    Vector3 after = Difference(current.target, current.eye);
    float angleScale = Dot(before, before) * Dot(after, after);
    // Keep teleports, camera cuts, and degenerate look-at inputs on the game's path.
    float upAlignment = Dot(previous.up, current.up);
    return Dot(jump, jump) < 16.0f * 16.0f && angleScale > 0.000001f &&
           std::isfinite(angleScale) && Dot(before, after) > 0.5f * std::sqrt(angleScale) &&
           std::isfinite(upAlignment) && upAlignment > 0.0f;
}

bool IsContinuous(const CarPose& previous, const CarPose& current) {
    Vector3 jump = Difference(current.position, previous.position);
    if (!(Dot(jump, jump) < 16.0f * 16.0f)) return false;
    for (unsigned axis = 0; axis < 3; ++axis) {
        Vector3 before = previous.rotation.axis[axis];
        Vector3 after = current.rotation.axis[axis];
        float beforeLength = Dot(before, before);
        float afterLength = Dot(after, after);
        if (!(beforeLength > 0.8f && beforeLength < 1.2f && afterLength > 0.8f && afterLength < 1.2f &&
              Dot(before, after) > 0.5f)) return false;
    }
    return true;
}

template<class Pose>
bool AdvancePose(PoseHistory<Pose>& history, const RenderFrame& frame, const Pose& stock) {
    if (frame.reset || !history.tracked || frame.tick < history.tick || frame.tick - history.tick > 1) {
        history = {frame.tick, {}, stock, true, false};
    } else if (frame.tick != history.tick) {
        history.hasPrevious = IsContinuous(history.current, stock);
        history.previous = history.current;
        history.current = stock;
        history.tick = frame.tick;
    } else if (memcmp(&history.current, &stock, sizeof(stock)) != 0) {
        // Some stock camera modes already move on every frame.
        history.current = stock;
        history.hasPrevious = false;
    }
    return frame.alpha < 1.0f && history.hasPrevious &&
           memcmp(&history.previous, &history.current, sizeof(stock)) != 0;
}

bool BlendCar(const CarPose& previous, const CarPose& current, float alpha, CarPose& out) {
    Vector3 x = Blend(previous.rotation.axis[0], current.rotation.axis[0], alpha);
    float length = Dot(x, x);
    if (!(length > 0.25f)) return false;
    x = Scale(x, 1.0f / std::sqrt(length));
    Vector3 y = Blend(previous.rotation.axis[1], current.rotation.axis[1], alpha);
    y = Difference(y, Scale(x, Dot(x, y)));
    length = Dot(y, y);
    if (!(length > 0.25f)) return false;
    y = Scale(y, 1.0f / std::sqrt(length));
    Vector3 z = Cross(x, y);
    if (Dot(z, current.rotation.axis[2]) < 0.0f) z = Scale(z, -1.0f);
    out = {Blend(previous.position, current.position, alpha), {{x, y, z}}};
    return true;
}

void Log(const char* format, ...) {
    HANDLE file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME time;
    GetLocalTime(&time);
    char message[512];
    int prefix = sprintf_s(message, "[%02u:%02u:%02u] ", time.wHour, time.wMinute, time.wSecond);
    va_list args;
    va_start(args, format);
    int count = vsnprintf_s(message + prefix, sizeof(message) - prefix - 2, _TRUNCATE, format, args);
    va_end(args);
    size_t length = (count < 0) ? strlen(message) : static_cast<size_t>(prefix + count);
    message[length++] = '\r';
    message[length++] = '\n';
    DWORD written;
    WriteFile(file, message, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

struct PresentTraceRow {
    LONGLONG returned, interval, present, previousLog;
    DWORD tick, mode, result;
    UINT cars;
    bool camera, player, race;
};

struct PresentTrace {
    LONGLONG start, previousReturn, previousLog;
    DWORD previousTick, mode;
    bool haveTick, finished;
    SYSTEMTIME localStart;
    size_t size;
    // Buffer samples so the render thread does not write CSV rows during capture.
    PresentTraceRow rows[65536];
};

PresentTrace frameTrace;
bool raceViewThisFrame;

unsigned __stdcall SavePresentTrace(void*) {
    wchar_t path[MAX_PATH];
    const wchar_t* leaf = wcsrchr(logPath, L'\\');
    if (!leaf) return 1;
    size_t prefix = static_cast<size_t>(leaf + 1 - logPath);
    if (prefix + 64 > std::size(path)) {
        Log("Frame trace: output path is too long");
        return 1;
    }
    const auto& time = frameTrace.localStart;
    int length = swprintf_s(path, L"%.*lsHP2FrameTrace-%04u%02u%02u-%02u%02u%02u-%lu.csv",
                            static_cast<int>(prefix), logPath, time.wYear, time.wMonth, time.wDay,
                            time.wHour, time.wMinute, time.wSecond, GetCurrentProcessId());
    if (length < 0) {
        Log("Frame trace: output path is too long");
        return 1;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") || !file) {
        Log("Frame trace: could not open %ls", path);
        return 1;
    }
    fputs("elapsed_ms,frame_ms,present_ms,previous_log_ms,tick,mode,camera_blended,player_blended,cars_blended,race_view,present_hresult\n", file);
    double toMilliseconds = 1000.0 / static_cast<double>(counterFrequency.QuadPart);
    for (size_t i = 0; i < frameTrace.size; ++i) {
        const auto& row = frameTrace.rows[i];
        fprintf(file, "%.3f,%.3f,%.3f,%.3f,%lu,%lu,%d,%d,%u,%d,0x%08lX\n",
                (row.returned - frameTrace.start) * toMilliseconds, row.interval * toMilliseconds,
                row.present * toMilliseconds, row.previousLog * toMilliseconds,
                row.tick, row.mode, row.camera, row.player, row.cars, row.race, row.result);
    }
    bool written = !ferror(file);
    if (fclose(file) != 0) written = false;
    if (!written) {
        Log("Frame trace: incomplete write to %ls", path);
        return 1;
    }
    Log("Frame trace: saved %zu frames over %.3f s, start=%02u:%02u:%02u.%03u, file=%ls",
        frameTrace.size, (frameTrace.previousReturn - frameTrace.start) * toMilliseconds / 1000.0,
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, wcsrchr(path, L'\\') + 1);
    return 0;
}

void RecordPresent(LONGLONG entered, LONGLONG returned, DWORD tick, bool race, bool camera, bool player,
                   UINT carCount, HRESULT result) {
    auto& trace = frameTrace;
    if (!trace.size) {
        // Start on the first successful frame with active simulation, after menus and loading.
        bool moving = trace.haveTick && tick > trace.previousTick &&
                      (!raceInterpolation || race) && result == S_OK;
        trace.haveTick = true;
        trace.previousTick = tick;
        if (!moving) return;
        trace.start = returned;
        trace.previousReturn = returned;
        trace.previousLog = 0;
        GetLocalTime(&trace.localStart);
    }

    trace.rows[trace.size++] = {returned, returned - trace.previousReturn, returned - entered, trace.previousLog,
                                tick, trace.mode, static_cast<DWORD>(result), carCount, camera, player, race};
    trace.previousReturn = returned;
    trace.previousLog = 0;
    if (returned - trace.start >= counterFrequency.QuadPart * frameTraceSeconds ||
        trace.size == std::size(trace.rows)) {
        trace.finished = true;
        auto thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, SavePresentTrace, nullptr, 0, nullptr));
        if (thread) CloseHandle(thread);
        else SavePresentTrace(nullptr);
    }
}

UINT ParseRefreshLimit(const wchar_t* setting) {
    UINT value = 0;
    if (!*setting) return AutoRefresh;
    for (; *setting; ++setting) {
        if (*setting < L'0' || *setting > L'9') return AutoRefresh;
        UINT digit = static_cast<UINT>(*setting - L'0');
        if (value > (AutoRefresh - digit) / 10) return AutoRefresh;
        value = value * 10 + digit;
    }
    return value;
}

constexpr UINT DefaultMsaaSamples = 8;

BOOL CALLBACK LoadSettings(PINIT_ONCE, PVOID, PVOID*) {
    // Keep the config and diagnostics beside the ASI, regardless of the working directory.
    wchar_t path[MAX_PATH];
    DWORD count = GetModuleFileNameW(pluginModule, path, MAX_PATH);
    wchar_t* leaf = (count != 0 && count < MAX_PATH) ? wcsrchr(path, L'\\') : nullptr;
    if (!leaf) return TRUE;
    size_t directoryLength = leaf + 1 - path;
    if (directoryLength + std::size(L"HP2Display.ini") > std::size(path)) return TRUE;

    wcscpy_s(leaf + 1, std::size(path) - directoryLength, L"HP2Display.ini");
    wcscpy_s(logPath, path);
    wcscpy_s(logPath + directoryLength, std::size(logPath) - directoryLength, L"HP2Display.log");
    wchar_t refresh[32];
    GetPrivateProfileStringW(L"Display", L"MaxRefreshHz", L"Auto", refresh, static_cast<DWORD>(std::size(refresh)), path);
    maxRefreshHz = ParseRefreshLimit(refresh);
    int sync = static_cast<int>(GetPrivateProfileIntW(L"Display", L"VSync", 1, path));
    vsync = (sync >= -1 && sync <= 1) ? sync : 1;
    UINT aa = GetPrivateProfileIntW(L"Graphics", L"MSAA", DefaultMsaaSamples, path);
    msaaSamples = (aa == 2 || aa == 4 || aa == 8) ? aa : 0;
    UINT seconds = GetPrivateProfileIntW(L"Diagnostics", L"SampleSeconds", 5, path);
    sampleSeconds = seconds <= 60 ? seconds : 5;
    frameTraceSeconds = GetPrivateProfileIntW(L"Diagnostics", L"FrameTraceSeconds", 0, path);
    if (frameTraceSeconds > 180) frameTraceSeconds = 180;
    showFps = GetPrivateProfileIntW(L"Overlay", L"ShowFPS", 0, path) == 1;
    QueryPerformanceFrequency(&counterFrequency);
    bool requested = GetPrivateProfileIntW(L"Interpolation", L"Camera", 0, path) == 1;
    raceInterpolation = requested && counterFrequency.QuadPart > 0 && InstallRaceHooks(gameImage);
    char refreshPolicy[20] = "Auto";
    if (maxRefreshHz != AutoRefresh) sprintf_s(refreshPolicy, "%u", maxRefreshHz);
    Log("HP2Display started: max_refresh=%s vsync=%d msaa=%u sample_seconds=%u trace_seconds=%u race_interpolation=%s fps_overlay=%s",
        refreshPolicy, vsync, msaaSamples, sampleSeconds, frameTraceSeconds,
        raceInterpolation ? "camera+cars" : requested ? "unavailable" : "disabled", showFps ? "enabled" : "disabled");
    return TRUE;
}

std::vector<UINT> EnumerateRefreshRates(void* d3d, UINT adapter, DWORD deviceType,
                                       const PresentParameters& parameters) {
    std::vector<UINT> rates;
    if (!d3d || parameters.windowed) return rates;

    auto count = ComMethod<UINT (WINAPI*)(void*, UINT)>(d3d, 6);
    auto enumerate = ComMethod<HRESULT (WINAPI*)(void*, UINT, UINT, DisplayMode*)>(d3d, 7);
    auto checkType = ComMethod<HRESULT (WINAPI*)(void*, UINT, DWORD, DWORD, DWORD, BOOL)>(d3d, 9);
    UINT available = count(d3d, adapter);
    for (UINT index = 0; index < available; ++index) {
        DisplayMode mode{};
        if (SUCCEEDED(enumerate(d3d, adapter, index, &mode)) &&
            mode.width == parameters.width && mode.height == parameters.height &&
            mode.refreshHz &&
            (mode.format == parameters.format ||
             SUCCEEDED(checkType(d3d, adapter, deviceType, mode.format, parameters.format, FALSE)))) {
            rates.push_back(mode.refreshHz);
        }
    }
    std::sort(rates.rbegin(), rates.rend());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

template<class ApplyMode>
HRESULT SetDisplayMode(const char* stage, void* d3d, UINT adapter, DWORD deviceType,
                       PresentParameters* parameters, ApplyMode apply) {
    // The desktop controls the refresh rate of windowed devices.
    if (parameters->windowed) return apply(parameters);

    auto rates = EnumerateRefreshRates(d3d, adapter, deviceType, *parameters);
    char list[220] = "";
    size_t length = 0;
    for (UINT rate : rates) {
        int written = _snprintf_s(list + length, sizeof(list) - length, _TRUNCATE, "%s%u", length ? "/" : "", rate);
        if (written < 0) break;
        length += static_cast<size_t>(written);
    }
    Log("%s: adapter=%u %ux%u format=%lu supported refresh Hz=%s",
        stage, adapter, parameters->width, parameters->height, parameters->format, length ? list : "none reported");

    rates.erase(std::remove_if(rates.begin(), rates.end(), [](UINT rate) { return rate > maxRefreshHz; }),
                rates.end());
    if (rates.empty()) rates.push_back(parameters->refreshHz);
    UINT interval = parameters->interval;
    if (vsync >= 0 && parameters->swapEffect != CopyVsync) interval = vsync ? IntervalOne : IntervalImmediate;
    HRESULT lastAttempt = E_FAIL;
    bool triedOriginal = false;
    for (UINT rate : rates) {
        triedOriginal = triedOriginal || (rate == parameters->refreshHz && interval == parameters->interval);
        // A failed driver call can mutate its input. Retry from the game's original parameters.
        PresentParameters proposed = *parameters;
        proposed.refreshHz = rate;
        proposed.interval = interval;
        lastAttempt = apply(&proposed);
        if (SUCCEEDED(lastAttempt)) {
            *parameters = proposed;
            return lastAttempt;
        }
        Log("%s: %u Hz interval=0x%08X rejected (0x%08lX)", stage, rate, interval, static_cast<DWORD>(lastAttempt));
        if (lastAttempt == DeviceLost) return lastAttempt;
    }
    if (triedOriginal) return lastAttempt;
    Log("%s: retrying game settings", stage);
    return apply(parameters);
}

struct MultisampleModes {
    DWORD samples[3];
    size_t count;
};

MultisampleModes CheckMultisampling(void* d3d, UINT adapter, DWORD type, const PresentParameters& parameters) {
    MultisampleModes result{};
    if (!msaaSamples || !d3d) return result;
    if (parameters.flags & LockableBackbuffer) return result;

    DWORD colorFormat = parameters.format;
    if (!colorFormat && parameters.windowed) {
        DisplayMode desktop{};
        if (FAILED(ComMethod<HRESULT (WINAPI*)(void*, UINT, DisplayMode*)>(d3d, 8)(d3d, adapter, &desktop))) return result;
        colorFormat = desktop.format;
    }

    auto check = ComMethod<HRESULT (WINAPI*)(void*, UINT, DWORD, DWORD, BOOL, DWORD)>(d3d, 11);
    // A usable sample count must be supported by both the color and automatic depth buffers.
    for (DWORD samples : {8ul, 4ul, 2ul}) {
        if (samples > msaaSamples || FAILED(check(d3d, adapter, type, colorFormat, parameters.windowed, samples))) continue;
        if (parameters.autoDepthStencil &&
            FAILED(check(d3d, adapter, type, parameters.depthStencilFormat, parameters.windowed, samples))) continue;
        result.samples[result.count++] = samples;
    }
    return result;
}

template<class ApplyMode>
HRESULT SetGraphicsMode(const char* stage, void* d3d, UINT adapter, DWORD type, PresentParameters* parameters,
                        ApplyMode apply) {
    auto modes = CheckMultisampling(d3d, adapter, type, *parameters);
    if (msaaSamples && !modes.count) {
        Log("%s: no eligible MSAA for color=%lu depth=%lu flags=0x%08lX", stage, parameters->format,
            parameters->autoDepthStencil ? parameters->depthStencilFormat : 0, parameters->flags);
    }
    return SetDisplayMode(stage, d3d, adapter, type, parameters, [&](PresentParameters* mode) {
        for (size_t i = 0; i < modes.count; ++i) {
            // The game's Present calls use a full buffer. Direct3D requires DISCARD for MSAA.
            PresentParameters candidate = *mode;
            candidate.multisample = modes.samples[i];
            candidate.swapEffect = DiscardSwap;
            if (mode->swapEffect == CopyVsync && !mode->windowed) {
                candidate.interval = vsync == 0 ? IntervalImmediate : IntervalOne;
            }
            HRESULT result = apply(&candidate);
            if (SUCCEEDED(result)) {
                // Reset may reuse the same game-owned structure. Keep its original AA and swap settings.
                candidate.multisample = mode->multisample;
                candidate.swapEffect = mode->swapEffect;
                candidate.interval = mode->interval;
                *mode = candidate;
                return result;
            }
            if (result == DeviceLost) return result;
            Log("%s: MSAA %lux @ %u Hz rejected (0x%08lX)", stage, modes.samples[i], mode->refreshHz,
                static_cast<DWORD>(result));
        }
        return apply(mode);
    });
}

struct FrameSample {
    void* device;
    LONGLONG start, previous, slowest;
    DWORD firstTick, previousTick;
    UINT presents, sameTick, cameraBlended, playerBlended, carsBlended, maxCars;
};
FrameSample sample;

struct BackbufferDescription {
    DWORD format, resourceType, usage, pool, size, multisample;
    UINT width, height;
};

struct Viewport {
    UINT x, y, width, height;
    float minZ, maxZ;
};

struct FpsCounter {
    LONGLONG start, previous;
    UINT frames, value;
};

void AddFpsFrame(FpsCounter& fps, LONGLONG now, LONGLONG frequency) {
    if (!fps.previous || now <= fps.previous || now - fps.previous > frequency) {
        fps = {now, now, 0, 0};
        return;
    }
    fps.previous = now;
    ++fps.frames;
    LONGLONG elapsed = now - fps.start;
    if (elapsed >= frequency / 2) {
        auto rounded = static_cast<UINT>(fps.frames * static_cast<double>(frequency) / elapsed + 0.5);
        fps.value = (std::min)(rounded, 9999u);
        fps.start = now;
        fps.frames = 0;
    }
}

struct FpsPaint {
    RECT background;
    RECT ink[8 * 7 * 3];
    DWORD inkCount;
};

constexpr BYTE FpsGlyphs[][7] = {
    {14, 17, 19, 21, 25, 17, 14}, // 0
    {4, 12, 4, 4, 4, 4, 14},     // 1
    {14, 17, 1, 2, 4, 8, 31},    // 2
    {30, 1, 1, 14, 1, 1, 30},    // 3
    {2, 6, 10, 18, 31, 2, 2},    // 4
    {31, 16, 16, 30, 1, 1, 30},  // 5
    {14, 16, 16, 30, 17, 17, 14},// 6
    {31, 1, 2, 4, 8, 8, 8},      // 7
    {14, 17, 17, 14, 17, 17, 14},// 8
    {14, 17, 17, 15, 1, 1, 14},  // 9
    {31, 16, 16, 30, 16, 16, 16},// F
    {30, 17, 17, 30, 16, 16, 16},// P
    {15, 16, 16, 14, 1, 1, 30},  // S
    {0, 0, 0, 14, 0, 0, 0},       // -
    {}                            // space
};

void LayoutFps(FpsPaint& paint, UINT value, UINT width, UINT height) {
    constexpr char glyphNames[] = "0123456789FPS- ";
    char text[9];
    if (value) sprintf_s(text, "%3u FPS", value);
    else strcpy_s(text, " -- FPS");
    float scale = (std::max)(1.5f, height / 540.0f);
    LONG left = std::lround(7 * scale);
    // Scores and position occupy the stock HUD's first 13 percent of the view.
    LONG top = std::lround(height * 0.143f);
    auto x = [&](int offset) { return left + std::lround(offset * scale); };
    auto y = [&](int offset) { return top + std::lround(offset * scale); };
    paint.inkCount = 0;
    int length = static_cast<int>(strlen(text));
    paint.background = {left, top, x(5 + 6 * length), y(13)};
    if (paint.background.right > static_cast<LONG>(width) || paint.background.bottom > static_cast<LONG>(height)) return;
    for (int index = 0; index < length; ++index) {
        const auto& glyph = FpsGlyphs[strchr(glyphNames, text[index]) - glyphNames];
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5;) {
                int first = col;
                while (col < 5 && (glyph[row] & (16 >> col))) ++col;
                if (first == col) {
                    ++col;
                } else {
                    int offset = 3 + 6 * index;
                    paint.ink[paint.inkCount++] = {x(offset + first), y(3 + row), x(offset + col), y(4 + row)};
                }
            }
        }
    }
}

struct FpsOverlay {
    void* device;
    Viewport surface;
    FpsCounter counter;
    FpsPaint paint;
    UINT paintedValue;
    bool ready;
    bool logged;
};

FpsOverlay fpsOverlay;
constexpr DWORD OverlayBackground = 0xFF181C22;
constexpr DWORD OverlayText = 0xFFF3F6FA;
using GetBackbuffer = HRESULT (WINAPI*)(void*, UINT, DWORD, void**);
using ClearSurface = HRESULT (WINAPI*)(void*, DWORD, const RECT*, DWORD, DWORD, float, DWORD);

bool ReadBackbufferDescription(void* device, BackbufferDescription& desc) {
    void* surface = nullptr;
    if (FAILED(ComMethod<GetBackbuffer>(device, 16)(device, 0, 0, &surface)) || !surface) return false;
    HRESULT result = ComMethod<HRESULT (WINAPI*)(void*, BackbufferDescription*)>(surface, 8)(surface, &desc);
    ComMethod<ULONG (WINAPI*)(void*)>(surface, 2)(surface);
    return SUCCEEDED(result);
}

void InitializeFpsOverlay(void* device, const BackbufferDescription& desc) {
    fpsOverlay = {};
    if (!showFps || !desc.width || !desc.height) return;
    fpsOverlay.device = device;
    fpsOverlay.surface = {0, 0, desc.width, desc.height, 0.0f, 1.0f};
}

bool DrawFpsOverlay(void* device) {
    auto& overlay = fpsOverlay;
    if (device != overlay.device) return false;
    if (!overlay.ready || overlay.paintedValue != overlay.counter.value) {
        LayoutFps(overlay.paint, overlay.counter.value, overlay.surface.width, overlay.surface.height);
        overlay.paintedValue = overlay.counter.value;
        overlay.ready = true;
    }
    if (!overlay.paint.inkCount) return false;

    void* backbuffer = nullptr;
    void* target = nullptr;
    if (FAILED(ComMethod<GetBackbuffer>(device, 16)(device, 0, 0, &backbuffer)) || !backbuffer) return false;
    HRESULT targetResult = ComMethod<HRESULT (WINAPI*)(void*, void**)>(device, 32)(device, &target);
    bool onBackbuffer = SUCCEEDED(targetResult) && target == backbuffer;
    if (target) ComMethod<ULONG (WINAPI*)(void*)>(target, 2)(target);
    ComMethod<ULONG (WINAPI*)(void*)>(backbuffer, 2)(backbuffer);
    if (!onBackbuffer) return false;

    Viewport viewport{};
    if (FAILED(ComMethod<HRESULT (WINAPI*)(void*, Viewport*)>(device, 41)(device, &viewport))) return false;
    auto setViewport = ComMethod<HRESULT (WINAPI*)(void*, const Viewport*)>(device, 40);
    bool change = viewport.x || viewport.y || viewport.width != overlay.surface.width ||
                  viewport.height != overlay.surface.height;
    if (change && FAILED(setViewport(device, &overlay.surface))) return false;
    auto clear = ComMethod<ClearSurface>(device, 36);
    // Rectangular clears touch only the color pixels used by the badge.
    HRESULT result = clear(device, 1, &overlay.paint.background, 1, OverlayBackground, 1.0f, 0);
    if (SUCCEEDED(result)) {
        result = clear(device, overlay.paint.inkCount, overlay.paint.ink, 1, OverlayText, 1.0f, 0);
    }
    if (change) setViewport(device, &viewport);
    if (SUCCEEDED(result) && !overlay.logged) {
        Log("FPS overlay: drawn on %ux%u backbuffer", overlay.surface.width, overlay.surface.height);
        overlay.logged = true;
    }
    return SUCCEEDED(result);
}

void StartSample(void* device, LONGLONG now, DWORD tick) {
    sample = {device, now, now, 0, tick, tick, 0, 0, 0, 0, 0, 0};
}

void LogMode(const char* stage, void* device, UINT adapter, const PresentParameters& parameters) {
    sample = {};
    if (frameTraceSeconds && !frameTrace.finished) {
        ++frameTrace.mode;
        frameTrace.haveTick = false;
    }
    raceViewThisFrame = false;
    ClearRaceHistory();
    cameraBlendedThisFrame = false;
    playerBlendedThisFrame = false;
    carsBlendedThisFrame = 0;
    BackbufferDescription desc{};
    if ((showFps || msaaSamples) && ReadBackbufferDescription(device, desc) && msaaSamples) {
        DWORD enabled = 0;
        auto getState = ComMethod<HRESULT (WINAPI*)(void*, DWORD, DWORD*)>(device, 51);
        if (SUCCEEDED(getState(device, 161, &enabled))) {
            Log("%s: MSAA requested=%u actual=%lu enabled=%lu format=%lu", stage, msaaSamples, desc.multisample,
                enabled, desc.format);
        } else {
            Log("%s: MSAA requested=%u actual=%lu format=%lu", stage, msaaSamples, desc.multisample, desc.format);
        }
    }
    InitializeFpsOverlay(device, desc);
    DisplayMode actual{};
    HRESULT result = ComMethod<HRESULT (WINAPI*)(void*, DisplayMode*)>(device, 8)(device, &actual);
    if (SUCCEEDED(result)) {
        Log("%s: adapter=%u %s backbuffer=%ux%u requested=%u Hz interval=0x%08X actual=%ux%u @ %u Hz FE_aspect=%.4f",
            stage, adapter, parameters.windowed ? "windowed" : "fullscreen",
            parameters.width, parameters.height, parameters.refreshHz, parameters.interval,
            actual.width, actual.height, actual.refreshHz, frontEndAspect ? *frontEndAspect : 0.0f);
    } else {
        Log("%s: GetDisplayMode returned 0x%08lX, requested=%u Hz",
            stage, static_cast<DWORD>(result), parameters.refreshHz);
    }
}

HRESULT WINAPI OnCreateDevice(void* d3d, UINT adapter, DWORD type, HWND window, DWORD flags,
                              PresentParameters* parameters, void** device) {
    InitOnceExecuteOnce(&settingsOnce, LoadSettings, nullptr, nullptr);
    auto create = ComMethod<CreateDevice>(d3d, 15);
    HRESULT result = SetGraphicsMode("CreateDevice", d3d, adapter, type, parameters, [&](PresentParameters* mode) {
        return create(d3d, adapter, type, window, flags, mode, device);
    });
    if (SUCCEEDED(result) && *device) LogMode("CreateDevice", *device, adapter, *parameters);
    return result;
}

HRESULT WINAPI OnReset(void* device, PresentParameters* parameters) {
    InitOnceExecuteOnce(&settingsOnce, LoadSettings, nullptr, nullptr);
    auto reset = ComMethod<ResetDevice>(device, 14);
    if (fpsOverlay.device == device) fpsOverlay = {};
    DeviceCreation creation{};
    void* d3d = nullptr;
    HRESULT result;
    if ((!parameters->windowed || msaaSamples) &&
        SUCCEEDED(ComMethod<HRESULT (WINAPI*)(void*, DeviceCreation*)>(device, 9)(device, &creation)) &&
        SUCCEEDED(ComMethod<HRESULT (WINAPI*)(void*, void**)>(device, 6)(device, &d3d)) && d3d) {
        result = SetGraphicsMode("Reset", d3d, creation.adapter, creation.deviceType, parameters,
                                 [&](PresentParameters* mode) { return reset(device, mode); });
        ComMethod<ULONG (WINAPI*)(void*)>(d3d, 2)(d3d);
    } else {
        result = reset(device, parameters);
    }
    if (SUCCEEDED(result)) LogMode("Reset", device, creation.adapter, *parameters);
    return result;
}

HRESULT WINAPI OnPresent(void* device, const RECT* source, const RECT* destination,
                         HWND window, const RGNDATA* dirty) {
    // Consume render diagnostics once, even if Present fails or the next frame is a menu.
    bool cameraBlended = cameraBlendedThisFrame;
    bool playerBlended = playerBlendedThisFrame;
    UINT carsBlended = carsBlendedThisFrame;
    bool raceView = raceViewThisFrame;
    cameraBlendedThisFrame = false;
    playerBlendedThisFrame = false;
    carsBlendedThisFrame = 0;
    raceViewThisFrame = false;
    if (showFps) DrawFpsOverlay(device);
    auto present = ComMethod<PresentFrame>(device, 15);
    LARGE_INTEGER entered{};
    bool tracing = frameTraceSeconds && !frameTrace.finished && counterFrequency.QuadPart &&
                   QueryPerformanceCounter(&entered);
    DWORD traceTick = tracing && gameTick ? *gameTick : 0;
    HRESULT result = present(device, source, destination, window, dirty);
    LARGE_INTEGER now{};
    if ((tracing || (SUCCEEDED(result) && (sampleSeconds || showFps))) && counterFrequency.QuadPart) {
        QueryPerformanceCounter(&now);
    }
    if (tracing && now.QuadPart) {
        RecordPresent(entered.QuadPart, now.QuadPart, traceTick, raceView, cameraBlended, playerBlended, carsBlended, result);
    }
    if (FAILED(result)) {
        sample = {};
        fpsOverlay.counter = {};
        return result;
    }
    if (!now.QuadPart) return result;
    if (showFps && fpsOverlay.device == device && result == S_OK) {
        AddFpsFrame(fpsOverlay.counter, now.QuadPart, counterFrequency.QuadPart);
    }
    if (!sampleSeconds) return result;
    DWORD tick = gameTick ? *gameTick : 0;
    if (sample.device != device || now.QuadPart - sample.previous > counterFrequency.QuadPart) {
        StartSample(device, now.QuadPart, tick);
        return result;
    }
    LONGLONG interval = now.QuadPart - sample.previous;
    if (interval > sample.slowest) sample.slowest = interval;
    sample.previous = now.QuadPart;
    ++sample.presents;
    if (cameraBlended) ++sample.cameraBlended;
    if (playerBlended) ++sample.playerBlended;
    if (carsBlended) ++sample.carsBlended;
    if (carsBlended > sample.maxCars) sample.maxCars = carsBlended;
    if (tick == sample.previousTick) ++sample.sameTick;
    sample.previousTick = tick;

    double seconds = static_cast<double>(now.QuadPart - sample.start) / counterFrequency.QuadPart;
    if (seconds >= sampleSeconds) {
        double tickRate = tick >= sample.firstTick ? (tick - sample.firstTick) / seconds : -1.0;
        double maxMs = 1000.0 * sample.slowest / counterFrequency.QuadPart;
        Log("Present: %.1f/s, mean %.2f ms, slowest %.2f ms, simulation %.1f ticks/s, same tick %u/%u, "
            "camera blended %u/%u, car frames %u/%u, player car frames %u/%u, max cars/frame %u",
            sample.presents / seconds, 1000.0 * seconds / sample.presents, maxMs,
            tickRate, sample.sameTick, sample.presents, sample.cameraBlended, sample.presents,
            sample.carsBlended, sample.presents, sample.playerBlended, sample.presents, sample.maxCars);
        if (tracing && frameTrace.size && !frameTrace.finished) {
            LARGE_INTEGER logged;
            if (QueryPerformanceCounter(&logged)) frameTrace.previousLog = logged.QuadPart - now.QuadPart;
        }
        StartSample(device, now.QuadPart, tick);
    }
    return result;
}

struct MovieFrameHeader {
    DWORD flags;
    short width, height;
};

struct MovieFrame {
    DWORD reserved;
    const MovieFrameHeader* header;
};

// The movie renderer takes scale multipliers and pixel coordinates separately.
void WINAPI SetMovieLayout(void* renderer, const MovieFrame* frame, float viewWidth, float viewHeight) {
    auto position = ComMethod<void (__thiscall*)(void*, float, float, float)>(renderer, 1);
    if (!frame || !frame->header || frame->header->width <= 0 || frame->header->height <= 0 ||
        !(viewWidth > 0 && viewHeight > 0)) {
        position(renderer, 0.0f, 0.0f, 0.0f);
        return;
    }

    float frameWidth = frame->header->width;
    float frameHeight = frame->header->height;
    float scaleX = viewWidth / frameWidth;
    float scaleY = viewHeight / frameHeight;
    float scale = scaleX < scaleY ? scaleX : scaleY;
    float left = (viewWidth - frameWidth * scale) * 0.5f;
    float top = (viewHeight - frameHeight * scale) * 0.5f;
    ComMethod<void (__thiscall*)(void*, float, float)>(renderer, 2)(renderer, scale, scale);
    position(renderer, left, top, 0.0f);

    InitOnceExecuteOnce(&settingsOnce, LoadSettings, nullptr, nullptr);
    Log("Movie: %.0fx%.0f within %.0fx%.0f, rect=(%.1f, %.1f, %.1f, %.1f)",
        frameWidth, frameHeight, viewWidth, viewHeight, left, top, frameWidth * scale, frameHeight * scale);
}

using RaceView = void (__cdecl*)(BYTE*);
using PrepareCar = void (__thiscall*)(void*, const BYTE*, const Vector3*, const CarRotation*);
RaceView prepareRaceView;
RaceView drawRaceView;
PrepareCar prepareCar;
const BYTE* simulationPaused;
const DWORD* simulationBlocked;

CameraPose ReadCamera(const BYTE* camera) {
    CameraPose pose;
    memcpy(&pose.eye, camera + 0x04, sizeof(Vector3));
    memcpy(&pose.target, camera + 0x10, sizeof(Vector3));
    memcpy(&pose.up, camera + 0x3C, sizeof(Vector3));
    return pose;
}

void WriteCamera(BYTE* camera, const CameraPose& pose) {
    memcpy(camera + 0x04, &pose.eye, sizeof(Vector3));
    memcpy(camera + 0x10, &pose.target, sizeof(Vector3));
    memcpy(camera + 0x3C, &pose.up, sizeof(Vector3));
}

void __fastcall RenderCar(BYTE* renderer, void*, const BYTE* director,
                          const Vector3* position, const CarRotation* rotation) {
    if (raceDirector && director == raceDirector) {
        // Set by the stock renderer just before preparing each car in the race.
        const BYTE* car;
        memcpy(&car, renderer + 0xF88, sizeof(car));
        CarHistory* reusable = nullptr;
        // A renderer slot can be assigned to another car. Key history by both pointers.
        for (auto& candidate : cars) {
            if (candidate.renderer == renderer && candidate.car == car) {
                reusable = &candidate;
                break;
            }
            if (!reusable && (!candidate.renderer || raceFrame.tick < candidate.pose.tick ||
                              raceFrame.tick - candidate.pose.tick > 2)) reusable = &candidate;
        }
        if (reusable && car) {
            if (reusable->renderer != renderer || reusable->car != car) *reusable = {renderer, car, {}};
            auto& history = reusable->pose;
            CarPose blended;
            if (AdvancePose(history, raceFrame, CarPose{*position, *rotation}) &&
                BlendCar(history.previous, history.current, raceFrame.alpha, blended)) {
                ++carsBlendedThisFrame;
                if (car == playerCar) playerBlendedThisFrame = true;
                // The stock preparation copies both arguments into its render instance.
                prepareCar(renderer, director, &blended.position, &blended.rotation);
                return;
            }
        }
    }
    prepareCar(renderer, director, position, rotation);
}

void WINAPI RenderRaceView(BYTE* director, int viewCount) {
    prepareRaceView(director);
    LARGE_INTEGER now;
    if (!raceInterpolation || !director || viewCount != 1 || *simulationPaused || *simulationBlocked ||
        !QueryPerformanceCounter(&now)) {
        if (raceClock.tracked) ClearRaceHistory();
        drawRaceView(director);
        return;
    }

    if (lastDirector != director) ClearRaceHistory();
    if (frameTraceSeconds && !frameTrace.finished) raceViewThisFrame = true;
    lastDirector = director;
    // All cars and the camera use the same tick and blend fraction for this view.
    raceFrame = SampleRenderClock(raceClock, *gameTick, now.QuadPart, counterFrequency.QuadPart);
    if (raceFrame.reset) memset(cars, 0, sizeof(cars));
    raceDirector = director;
    memcpy(&playerCar, director + 0x12C, sizeof(playerCar));

    BYTE* camera = ComMethod<BYTE* (__thiscall*)(void*)>(director, 2)(director);
    DWORD controllerIndex;
    memcpy(&controllerIndex, director + 0x164, sizeof(controllerIndex));
    if (cameraOwner != camera || cameraController != controllerIndex) cameraHistory = {};
    cameraOwner = camera;
    cameraController = controllerIndex;

    CameraPose stock;
    BYTE stockTransform[0x60];
    bool useBlendedCamera = false;
    if (camera) {
        stock = ReadCamera(camera);
        useBlendedCamera = AdvancePose(cameraHistory, raceFrame, stock);
        if (useBlendedCamera) {
            // Both original calls consume the controller's look-at inputs.
            // Restore the controller and its derived transform after drawing the race view.
            memcpy(stockTransform, director + 0x14, sizeof(stockTransform));
            float alpha = raceFrame.alpha;
            const auto& previous = cameraHistory.previous;
            WriteCamera(camera, {Blend(previous.eye, stock.eye, alpha), Blend(previous.target, stock.target, alpha),
                                 Blend(previous.up, stock.up, alpha)});
            prepareRaceView(director);
        }
    }

    drawRaceView(director);
    raceDirector = nullptr;
    if (useBlendedCamera) {
        WriteCamera(camera, stock);
        memcpy(director + 0x14, stockTransform, sizeof(stockTransform));
        cameraBlendedThisFrame = true;
    }
}

// Reconstruct the pushes replaced by our call and tail-call the stdcall hook.
__declspec(naked) void CreateDeviceThunk() {
    __asm {
        pop ecx
        push edx
        push eax
        push ecx
        jmp OnCreateDevice
    }
}

__declspec(naked) void ResetThunk() {
    __asm {
        pop edx
        push eax
        push edx
        jmp OnReset
    }
}

__declspec(naked) void PresentThunk() {
    __asm {
        pop edx
        push 0
        push eax
        push edx
        jmp OnPresent
    }
}

__declspec(naked) void MovieLayoutThunk() {
    __asm {
        push dword ptr [ebp-18h]
        push dword ptr [ebp-20h]
        push ebx
        push esi
        call SetMovieLayout
        ret
    }
}

__declspec(naked) void RaceViewThunk() {
    __asm {
        push ebx
        push edi
        call RenderRaceView
        ret
    }
}

__declspec(naked) void CarViewThunk() {
    // Thiscall and fastcall use ECX for the instance and pop all three stack arguments.
    __asm jmp RenderCar
}

struct CallSite {
    DWORD rva;
    BYTE size;
    BYTE expected[15];
    void (*thunk)();
};

// The supplied viewport dimensions are the current surface size. HP2WSFix's
// patched constants used by the preceding scale call have their axes reversed.
const CallSite movieLayoutSite = {
    0x0D7BC8, 13, {0x8B, 0x16, 0x6A, 0, 0x6A, 0, 0x6A, 0, 0x8B, 0xCE, 0xFF, 0x52, 0x04}, MovieLayoutThunk
};

// This covers the two calls and their stack cleanup in the race view loop.
const CallSite raceViewSite = {
    0x0D7EC7, 15, {0x57, 0xE8, 0x73, 0xFE, 0xFF, 0xFF, 0x57, 0xE8, 0x3D, 0xF8, 0xFF, 0xFF, 0x83, 0xC4, 0x08},
    RaceViewThunk
};

const CallSite carViewSite = {0x0CA978, 5, {0xE8, 0xC3, 0x0F, 0xF6, 0xFF}, CarViewThunk};

const CallSite sites[] = {
    {0x156E48, 6, {0x6A, 0, 0x50, 0xFF, 0x51, 0x3C}, PresentThunk},
    {0x1573B2, 6, {0x6A, 0, 0x50, 0xFF, 0x52, 0x3C}, PresentThunk},
    {0x1594DD, 6, {0x6A, 0, 0x50, 0xFF, 0x51, 0x3C}, PresentThunk},
    {0x15960E, 6, {0x50, 0x8B, 0x10, 0xFF, 0x52, 0x38}, ResetThunk},
    {0x1596FB, 6, {0x50, 0x8B, 0x10, 0xFF, 0x52, 0x38}, ResetThunk},
    {0x159953, 5, {0x52, 0x50, 0xFF, 0x51, 0x3C}, CreateDeviceThunk},
    {0x1599DB, 5, {0x52, 0x50, 0xFF, 0x51, 0x3C}, CreateDeviceThunk},
};

void WriteCall(BYTE* image, const CallSite& site) {
    BYTE* address = image + site.rva;
    DWORD relative = reinterpret_cast<DWORD>(site.thunk) - reinterpret_cast<DWORD>(address + 5);
    address[0] = 0xE8;
    memcpy(address + 1, &relative, sizeof(relative));
    memset(address + 5, 0x90, site.size - 5);
}

bool InstallRaceHooks(BYTE* image) {
    BYTE* race = image + raceViewSite.rva;
    BYTE* car = image + carViewSite.rva;
    if (memcmp(race, raceViewSite.expected, raceViewSite.size) != 0 ||
        memcmp(car, carViewSite.expected, carViewSite.size) != 0) return false;
    DWORD raceProtection, carProtection;
    if (!VirtualProtect(race, raceViewSite.size, PAGE_EXECUTE_READWRITE, &raceProtection)) return false;
    if (!VirtualProtect(car, carViewSite.size, PAGE_EXECUTE_READWRITE, &carProtection)) {
        VirtualProtect(race, raceViewSite.size, raceProtection, &raceProtection);
        return false;
    }

    prepareRaceView = reinterpret_cast<RaceView>(image + 0x0D7D40);
    drawRaceView = reinterpret_cast<RaceView>(image + 0x0D7710);
    prepareCar = reinterpret_cast<PrepareCar>(image + 0x02B940);
    simulationPaused = image + 0x3221EC;
    simulationBlocked = reinterpret_cast<const DWORD*>(image + 0x3221D0);
    WriteCall(image, raceViewSite);
    WriteCall(image, carViewSite);
    FlushInstructionCache(GetCurrentProcess(), race, raceViewSite.size);
    FlushInstructionCache(GetCurrentProcess(), car, carViewSite.size);
    VirtualProtect(race, raceViewSite.size, raceProtection, &raceProtection);
    VirtualProtect(car, carViewSite.size, carProtection, &carProtection);
    return true;
}

bool InstallHooks(BYTE* image) {
    // Check the executable and every required call site before writing any hooks.
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.TimeDateStamp != 0x3DDD5A2C || nt->OptionalHeader.SizeOfImage != 0x460000) return false;

    BYTE* movieLayout = image + movieLayoutSite.rva;
    if (memcmp(movieLayout, movieLayoutSite.expected, movieLayoutSite.size) != 0) return false;
    for (const auto& site : sites) {
        if (memcmp(image + site.rva, site.expected, site.size) != 0) return false;
    }
    BYTE* start = image + sites[0].rva;
    const CallSite& last = sites[std::size(sites) - 1];
    SIZE_T length = image + last.rva + last.size - start;
    DWORD protection;
    if (!VirtualProtect(start, length, PAGE_EXECUTE_READWRITE, &protection)) return false;
    DWORD movieProtection;
    if (!VirtualProtect(movieLayout, movieLayoutSite.size, PAGE_EXECUTE_READWRITE, &movieProtection)) {
        VirtualProtect(start, length, protection, &protection);
        return false;
    }

    gameImage = image;
    gameTick = reinterpret_cast<const DWORD*>(image + 0x3221DC);
    frontEndAspect = reinterpret_cast<const float*>(image + 0x25D900);
    for (const auto& site : sites) WriteCall(image, site);
    WriteCall(image, movieLayoutSite);
    FlushInstructionCache(GetCurrentProcess(), movieLayout, movieLayoutSite.size);
    FlushInstructionCache(GetCurrentProcess(), start, length);
    VirtualProtect(movieLayout, movieLayoutSite.size, movieProtection, &movieProtection);
    VirtualProtect(start, length, protection, &protection);
    return true;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        pluginModule = module;
        // The loader holds the ASI for the lifetime of the game.
        if (!InstallHooks(reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr)))) {
            OutputDebugStringA("HP2Display: unsupported game executable or modified renderer call sites.\n");
            return FALSE;
        }
    }
    return TRUE;
}
