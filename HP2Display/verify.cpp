#include "HP2Display.cpp"

#include <cassert>
#include <cmath>

namespace {

// Enter the patched game calls with the same registers and stack as the renderer.
HRESULT RunCreate(BYTE* site, void* d3d, HWND window, PresentParameters* parameters, void** device) {
    HRESULT result;
    __asm {
        push offset resumeCreate
        push device
        push parameters
        push 20h
        push window
        push 1
        xor edx, edx
        mov eax, d3d
        jmp site
    resumeCreate:
        mov result, eax
    }
    return result;
}

HRESULT RunReset(BYTE* site, void* device, PresentParameters* parameters) {
    HRESULT result;
    __asm {
        push offset resumeReset
        push parameters
        mov eax, device
        jmp site
    resumeReset:
        mov result, eax
    }
    return result;
}

HRESULT RunPresent(BYTE* site, void* device) {
    HRESULT result;
    __asm {
        push offset resumePresent
        push 0
        push 0
        push 0
        mov eax, device
        jmp site
    resumePresent:
        mov result, eax
    }
    return result;
}

__declspec(naked) void CallMovieSite(BYTE*, void*, const MovieFrame*, const float*) {
    __asm {
        push ebp
        push ebx
        push esi
        mov eax, [esp+10h]
        mov esi, [esp+14h]
        mov ebx, [esp+18h]
        mov ebp, [esp+1Ch]
        call eax
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

void RunMovieLayout(BYTE* site, void* renderer, const MovieFrame* frame, float width, float height) {
    float movieLocals[9]{};
    movieLocals[0] = width;   // EBP-0x20 in the stock movie player.
    movieLocals[2] = height;  // EBP-0x18.
    CallMovieSite(site, renderer, frame, movieLocals + 8);
}

struct MovieLayout {
    void** methods;
    float scaleX, scaleY, left, top;
};

void __fastcall MockMoviePosition(MovieLayout* renderer, void*, float left, float top, float depth) {
    assert(depth == 0.0f);
    renderer->left = left;
    renderer->top = top;
}

void __fastcall MockMovieScale(MovieLayout* renderer, void*, float scaleX, float scaleY) {
    renderer->scaleX = scaleX;
    renderer->scaleY = scaleY;
}

void VerifyMovieLayout(BYTE* site) {
    void* methods[] = {nullptr, reinterpret_cast<void*>(MockMoviePosition), reinterpret_cast<void*>(MockMovieScale)};
    MovieLayout layout{methods, 0, 0, 0, 0};
    MovieFrameHeader header{0, 720, 496};
    MovieFrame frame{0, &header};
    RunMovieLayout(site, &layout, &frame, 1920, 1080);
    assert(std::fabs(layout.left - 176.129f) < 0.01f && std::fabs(layout.top) < 0.01f);
    assert(layout.scaleX == layout.scaleY);
    assert(std::fabs(layout.scaleY * header.height - 1080.0f) < 0.01f);

    header = {0, 640, 448};
    RunMovieLayout(site, &layout, &frame, 1920, 1080);
    assert(std::fabs(layout.left - 188.571f) < 0.01f && std::fabs(layout.top) < 0.01f);
    RunMovieLayout(site, &layout, &frame, 800, 600);
    assert(std::fabs(layout.left) < 0.01f && std::fabs(layout.top - 20.0f) < 0.01f);
    RunMovieLayout(site, &layout, &frame, 3440, 1440);
    assert(std::fabs(layout.left - 691.429f) < 0.01f && std::fabs(layout.top) < 0.01f);
    puts("Startup logo and intro are scaled proportionally and centered");
}

BYTE cameraData[0x48];
CameraPose poseAtDraw;
unsigned raceDraws;
BYTE renderCars[2][0xF8C];
BYTE* carSite;
CarPose stockCars[2];
CarPose carsAtDraw[2];

__declspec(naked) void RunCarView(BYTE*, BYTE*, const BYTE*, const Vector3*, const CarRotation*) {
    __asm {
        mov eax, [esp+4]
        mov ecx, [esp+8]
        push [esp+14h]
        push [esp+14h]
        push [esp+14h]
        jmp eax
    }
}

void WINAPI SaveMockCar(BYTE* renderer, const Vector3* position, const CarRotation* rotation) {
    unsigned index = renderer == renderCars[0] ? 0 : 1;
    carsAtDraw[index] = {*position, *rotation};
}

__declspec(naked) void MockPrepareCar() {
    __asm {
        push [esp+0Ch]
        push [esp+0Ch]
        push ecx
        call SaveMockCar
        ret 0Ch
    }
}

BYTE* __fastcall MockActiveCamera(void*, void*) { return cameraData; }

void MockPrepareRaceView(BYTE* director) {
    CameraPose pose = ReadCamera(cameraData);
    memcpy(director + 0x14, &pose.eye, sizeof(pose.eye));
}

void MockDrawRaceView(BYTE* director) {
    ++raceDraws;
    poseAtDraw = ReadCamera(cameraData);
    assert(memcmp(director + 0x14, &poseAtDraw.eye, sizeof(Vector3)) == 0);
    for (unsigned i = 0; i < 2; ++i) {
        RunCarView(carSite, renderCars[i], director, &stockCars[i].position, &stockCars[i].rotation);
    }
}

void RunRaceView(BYTE* site, BYTE* director, int views) {
    __asm {
        mov eax, site
        mov edi, director
        mov ebx, views
        call eax
    }
}

void VerifyRaceInterpolation(BYTE* image) {
    CameraPose first{{0, 0, 1}, {0, 10, 1}, {0, 0, 1}};
    CameraPose second{{1, 0, 1}, {2, 10, 1}, {0, 0, 1}};
    RenderClock clock{};
    PoseHistory<CameraPose> history{};
    assert(!AdvancePose(history, SampleRenderClock(clock, 10, 1000, 60000), first));
    assert(AdvancePose(history, SampleRenderClock(clock, 11, 2000, 60000), second));
    auto quarter = SampleRenderClock(clock, 11, 2250, 60000);
    assert(quarter.alpha == 0.25f && AdvancePose(history, quarter, second));
    assert(!AdvancePose(history, SampleRenderClock(clock, 11, 3150, 60000), second));
    assert(!AdvancePose(history, SampleRenderClock(clock, 13, 4000, 60000), second));
    CameraPose cut = first;
    cut.eye.x = 1000;
    assert(!AdvancePose(history, SampleRenderClock(clock, 14, 5000, 60000), cut));

    constexpr CarRotation straight{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    constexpr CarRotation turned{{{0.8f, 0.6f, 0}, {-0.6f, 0.8f, 0}, {0, 0, 1}}};
    CarPose carBefore{{0, 1, 2}, straight};
    CarPose carAfter{{4, 1, 2}, turned};
    CarPose blended{};
    assert(IsContinuous(carBefore, carAfter) && BlendCar(carBefore, carAfter, quarter.alpha, blended));
    assert(blended.position.x == 1.0f);
    assert(std::fabs(Dot(blended.rotation.axis[0], blended.rotation.axis[1])) < 0.00001f);
    assert(std::fabs(Dot(blended.rotation.axis[0], blended.rotation.axis[0]) - 1.0f) < 0.00001f);

    BYTE director[0x168]{};
    void* methods[] = {nullptr, nullptr, reinterpret_cast<void*>(MockActiveCamera)};
    void** vtable = methods;
    memcpy(director, &vtable, sizeof(vtable));
    prepareRaceView = MockPrepareRaceView;
    drawRaceView = MockDrawRaceView;
    prepareCar = reinterpret_cast<PrepareCar>(MockPrepareCar);
    raceInterpolation = true;
    QueryPerformanceFrequency(&counterFrequency);
    BYTE* site = image + raceViewSite.rva;
    carSite = image + carViewSite.rva;
    const BYTE* player = renderCars[0];
    memcpy(director + 0x12C, &player, sizeof(player));
    for (unsigned i = 0; i < 2; ++i) {
        const BYTE* car = renderCars[i];
        memcpy(renderCars[i] + 0xF88, &car, sizeof(car));
        stockCars[i] = {{10.0f * (i + 1), 0, 1}, straight};
    }
    DWORD* tick = reinterpret_cast<DWORD*>(image + 0x3221DC);
    *tick = 10;
    WriteCamera(cameraData, first);
    RunRaceView(site, director, 1);
    assert(raceDraws == 1 && !cameraBlendedThisFrame && poseAtDraw.eye.x == first.eye.x && !carsBlendedThisFrame);
    ++*tick;
    stockCars[0].position.x = 11;
    stockCars[1].position.x = 21;
    WriteCamera(cameraData, second);
    RunRaceView(site, director, 1);
    assert(raceDraws == 2 && cameraBlendedThisFrame && carsBlendedThisFrame == 2 && playerBlendedThisFrame);
    assert(std::fabs(carsAtDraw[0].position.x - poseAtDraw.eye.x - 10.0f) < 0.001f);
    assert(std::fabs(carsAtDraw[1].position.x - poseAtDraw.eye.x - 20.0f) < 0.001f);
    assert(stockCars[0].position.x == 11 && stockCars[1].position.x == 21);
    assert(ReadCamera(cameraData).eye.x == second.eye.x);
    assert(memcmp(director + 0x14, &second.eye, sizeof(second.eye)) == 0);
    cameraBlendedThisFrame = false;
    RunRaceView(site, director, 2);
    assert(raceDraws == 3 && !cameraBlendedThisFrame && poseAtDraw.eye.x == second.eye.x);
    image[0x3221EC] = 1;
    RunRaceView(site, director, 1);
    assert(raceDraws == 4 && !cameraBlendedThisFrame && poseAtDraw.eye.x == second.eye.x);
    image[0x3221EC] = 0;
    puts("Race interpolation: shared subframes for camera/player/opponent, rigid rotation, cuts, and x86 render hooks passed");
}

struct MockObject { void** table; };
void* adapterMethods[16]{};
void* deviceMethods[16]{};
MockObject mockD3d{adapterMethods};
MockObject mockDevice{deviceMethods};
UINT attempts;
UINT adapterUsed;
bool rejectAll;

UINT WINAPI ModeCount(void*, UINT adapter) {
    adapterUsed = adapter;
    return 9;
}

HRESULT WINAPI EnumModes(void*, UINT, UINT index, DisplayMode* mode) {
    constexpr DisplayMode modes[] = {
        {1920, 1080, 60, 22}, {1920, 1080, 240, 22}, {1920, 1080, 165, 22},
        {1920, 1080, 360, 22}, {1920, 1080, 239, 23}, {1280, 720, 75, 22},
        {1920, 1080, 144, 22}, {1920, 1080, 120, 22}, {1920, 1080, 60, 22}
    };
    *mode = modes[index];
    return S_OK;
}

HRESULT WINAPI CheckType(void*, UINT, DWORD type, DWORD displayFormat, DWORD backBufferFormat, BOOL windowed) {
    return type == 1 && displayFormat == 22 && backBufferFormat == 21 && !windowed ? S_OK : E_FAIL;
}

HRESULT WINAPI MockCheckMultisample(void*, UINT adapter, DWORD type, DWORD format, BOOL, DWORD samples) {
    assert(adapter == 4 && type == 1);
    return (format == 22 && samples <= 8) || (format == 75 && samples <= 4) ? S_OK : E_FAIL;
}

void VerifyMultisampleSelection() {
    // The mock rejects 8x for depth and 4x on device creation, so 2x must succeed.
    msaaSamples = 8;
    PresentParameters mode{1920, 1080, 22, 1, 0, CopyVsync, nullptr, FALSE, TRUE, 75, 0, 60, 0};
    auto options = CheckMultisampling(&mockD3d, 4, 1, mode);
    assert(options.count == 2 && options.samples[0] == 4 && options.samples[1] == 2);
    mode.flags = LockableBackbuffer;
    assert(!CheckMultisampling(&mockD3d, 4, 1, mode).count);
    mode.flags = 0;
    UINT oldLimit = maxRefreshHz;
    maxRefreshHz = 60;
    UINT attempt = 0;
    assert(SetGraphicsMode("Verify MSAA fallback", &mockD3d, 4, 1, &mode, [&](PresentParameters* candidate) {
        assert(candidate->multisample == (attempt == 0 ? 4ul : 2ul));
        assert(candidate->swapEffect == DiscardSwap && candidate->refreshHz == 60 && candidate->interval == IntervalOne);
        return attempt++ == 0 ? E_FAIL : S_OK;
    }) == S_OK);
    assert(attempt == 2 && mode.multisample == 0 && mode.swapEffect == CopyVsync);
    maxRefreshHz = oldLimit;
    msaaSamples = 0;
}

HRESULT MockApplyMode(PresentParameters* parameters) {
    constexpr UINT rates[] = {240, 165, 144, 120, 60};
    UINT index = attempts++;
    assert(parameters->width == 1920);
    if (index < std::size(rates)) {
        assert(parameters->refreshHz == rates[index] && parameters->interval == IntervalOne);
        if (index == 1 && !rejectAll) return S_OK;
        parameters->width = 0;
        return E_FAIL;
    }
    assert(parameters->refreshHz == 60 && !parameters->interval);
    return S_OK;
}

HRESULT WINAPI MockCreate(void*, UINT, DWORD, HWND, DWORD, PresentParameters* parameters, void** device) {
    HRESULT result = MockApplyMode(parameters);
    if (SUCCEEDED(result)) *device = &mockDevice;
    return result;
}

HRESULT WINAPI MockReset(void*, PresentParameters* parameters) {
    assert(adapterUsed == 4);
    return MockApplyMode(parameters);
}

HRESULT WINAPI MockDisplayMode(void*, DisplayMode* mode) {
    *mode = {1920, 1080, 60, 22};
    return S_OK;
}

HRESULT WINAPI MockCreation(void*, DeviceCreation* creation) {
    creation->adapter = 4;
    creation->deviceType = 1;
    return S_OK;
}

HRESULT WINAPI MockOwner(void*, void** owner) {
    *owner = &mockD3d;
    return S_OK;
}

ULONG WINAPI MockRelease(void*) { return 1; }

void VerifyFpsLayout() {
    FpsCounter counter{};
    for (LONGLONG time = 100; time <= 600; time += 5) AddFpsFrame(counter, time, 1000);
    assert(counter.value == 200);
    AddFpsFrame(counter, 2000, 1000);
    assert(counter.value == 0);

    LONG previousHeight = 0;
    constexpr DisplayMode modes[] = {{640, 480, 0, 0}, {1280, 720, 0, 0}, {1920, 1080, 0, 0},
                                     {3440, 1440, 0, 0}, {3840, 2160, 0, 0}};
    for (const auto& mode : modes) {
        FpsPaint paint{};
        LayoutFps(paint, 240, mode.width, mode.height);
        const auto& box = paint.background;
        assert(paint.inkCount && box.left > 0 && box.top > 0 &&
               box.right <= static_cast<LONG>(mode.width) && box.bottom < static_cast<LONG>(mode.height / 4));
        assert(box.bottom - box.top >= previousHeight);
        previousHeight = box.bottom - box.top;
    }
}

void VerifyOverlayPixels(void* device) {
    struct LockedRect { int pitch; void* pixels; };
    constexpr DWORD untouched = 0xFF245068;
    auto clear = ComMethod<ClearSurface>(device, 36);
    assert(clear(device, 0, nullptr, 1, untouched, 1.0f, 0) == S_OK);
    auto setViewport = ComMethod<HRESULT (WINAPI*)(void*, const Viewport*)>(device, 40);
    auto width = fpsOverlay.surface.width;
    auto height = fpsOverlay.surface.height;
    Viewport partial{width / 8, height / 8, width / 2, height / 2, 0.0f, 1.0f};
    assert(setViewport(device, &partial) == S_OK);
    fpsOverlay.counter.value = 240;
    assert(DrawFpsOverlay(device));
    Viewport restored{};
    assert(ComMethod<HRESULT (WINAPI*)(void*, Viewport*)>(device, 41)(device, &restored) == S_OK);
    assert(memcmp(&partial, &restored, sizeof(partial)) == 0);
    assert(setViewport(device, &fpsOverlay.surface) == S_OK);

    void* surface = nullptr;
    assert(ComMethod<GetBackbuffer>(device, 16)(device, 0, 0, &surface) == S_OK && surface);
    LockedRect locked{};
    assert(ComMethod<HRESULT (WINAPI*)(void*, LockedRect*, const RECT*, DWORD)>(surface, 9)
           (surface, &locked, nullptr, 0x10) == S_OK);
    auto pixel = [&](LONG x, LONG y) {
        auto row = static_cast<const BYTE*>(locked.pixels) + y * locked.pitch;
        return reinterpret_cast<const DWORD*>(row)[x] & 0x00FFFFFF;
    };
    const auto& box = fpsOverlay.paint.background;
    const auto& ink = fpsOverlay.paint.ink[0];
    assert(pixel(box.left, box.top) == (OverlayBackground & 0x00FFFFFF));
    assert(pixel(ink.left, ink.top) == (OverlayText & 0x00FFFFFF));
    assert(pixel(box.left - 1, box.top) == (untouched & 0x00FFFFFF));
    assert(ComMethod<HRESULT (WINAPI*)(void*)>(surface, 10)(surface) == S_OK);
    ComMethod<ULONG (WINAPI*)(void*)>(surface, 2)(surface);
    printf("FPS overlay rendered on %ux%u with color and viewport restoration verified\n",
           fpsOverlay.surface.width, fpsOverlay.surface.height);
}

void VerifyPresentTrace() {
    frameTraceSeconds = 1;
    LONGLONG second = counterFrequency.QuadPart;
    frameTrace.mode = 2;
    RecordPresent(second, second + 100, 0, false, false, false, 0, S_OK);
    assert(frameTrace.size == 0);
    RecordPresent(second + 500, second + 600, 1, true, true, true, 6, S_OK);
    assert(frameTrace.size == 1 && frameTrace.rows[0].interval == 0 && frameTrace.rows[0].present == 100);
    frameTrace.previousLog = 200;
    RecordPresent(second + 3500, second + 3600, 1, true, true, true, 5, S_OK);
    const auto& row = frameTrace.rows[1];
    assert(row.interval == 3000 && row.previousLog == 200 && row.tick == 1 && row.mode == 2 && row.cars == 5);
    RecordPresent(second * 2 + 600, second * 2 + 700, 60, true, false, false, 0, S_OK);
    assert(frameTrace.finished && frameTrace.size == 3);

    WIN32_FIND_DATAW traceFile{};
    HANDLE finder = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 100 && finder == INVALID_HANDLE_VALUE; ++attempt) {
        Sleep(10);
        finder = FindFirstFileW(L"HP2FrameTrace-*.csv", &traceFile);
        if (finder != INVALID_HANDLE_VALUE && traceFile.nFileSizeLow == 0) {
            FindClose(finder);
            finder = INVALID_HANDLE_VALUE;
        }
    }
    assert(finder != INVALID_HANDLE_VALUE);
    FindClose(finder);
    FILE* csv = nullptr;
    assert(_wfopen_s(&csv, traceFile.cFileName, L"rb") == 0 && csv);
    char line[200];
    assert(fgets(line, sizeof(line), csv) && strstr(line, "previous_log_ms,tick"));
    assert(fgets(line, sizeof(line), csv) && strstr(line, ",1,2,1,1,6,1,0x00000000"));
    assert(fgets(line, sizeof(line), csv) && strstr(line, ",1,2,1,1,5,1,0x00000000"));
    assert(fgets(line, sizeof(line), csv) && strstr(line, ",60,2,0,0,0,1,0x00000000"));
    fclose(csv);
    puts("Per-frame trace: timing, physics ticks and buffered CSV export passed");
}

void VerifyAntialiasedPixels(void* device) {
    struct Vertex { float x, y, z, rhw; DWORD color; };
    struct LockedRect { int pitch; void* pixels; };
    constexpr Vertex triangle[] = {{40, 40, 0, 1, 0xFFFFFFFF}, {200, 40, 0, 1, 0xFFFFFFFF},
                                   {40, 180, 0, 1, 0xFFFFFFFF}};
    auto clear = ComMethod<ClearSurface>(device, 36);
    assert(clear(device, 0, nullptr, 1, 0xFF000000, 1.0f, 0) == S_OK);
    auto setState = ComMethod<HRESULT (WINAPI*)(void*, DWORD, DWORD)>(device, 50);
    assert(setState(device, 7, FALSE) == S_OK && setState(device, 22, 1) == S_OK);
    auto setTextureState = ComMethod<HRESULT (WINAPI*)(void*, DWORD, DWORD, DWORD)>(device, 63);
    assert(setTextureState(device, 0, 1, 2) == S_OK && setTextureState(device, 0, 2, 0) == S_OK);
    assert(ComMethod<HRESULT (WINAPI*)(void*, DWORD)>(device, 76)(device, 0x44) == S_OK);
    assert(ComMethod<HRESULT (WINAPI*)(void*)>(device, 34)(device) == S_OK);
    assert(ComMethod<HRESULT (WINAPI*)(void*, DWORD, UINT, const void*, UINT)>(device, 72)
           (device, 4, 1, triangle, sizeof(Vertex)) == S_OK);
    assert(ComMethod<HRESULT (WINAPI*)(void*)>(device, 35)(device) == S_OK);

    BackbufferDescription desc{};
    assert(ReadBackbufferDescription(device, desc) && desc.multisample >= 2);
    void* source = nullptr;
    void* resolved = nullptr;
    assert(ComMethod<GetBackbuffer>(device, 16)(device, 0, 0, &source) == S_OK);
    assert(ComMethod<HRESULT (WINAPI*)(void*, UINT, UINT, DWORD, DWORD, BOOL, void**)>(device, 25)
           (device, desc.width, desc.height, desc.format, 0, TRUE, &resolved) == S_OK);
    assert(ComMethod<HRESULT (WINAPI*)(void*, void*, const RECT*, UINT, void*, const POINT*)>(device, 28)
           (device, source, nullptr, 0, resolved, nullptr) == S_OK);
    LockedRect locked{};
    assert(ComMethod<HRESULT (WINAPI*)(void*, LockedRect*, const RECT*, DWORD)>(resolved, 9)
           (resolved, &locked, nullptr, 0x10) == S_OK);
    unsigned edgePixels = 0;
    for (int y = 41; y < 181; ++y) {
        const auto* pixels = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(locked.pixels) + y * locked.pitch);
        for (int x = 41; x < 201; ++x) {
            DWORD rgb = pixels[x] & 0xFFFFFF;
            if (rgb && rgb < 0xFFFFFF) ++edgePixels;
        }
    }
    assert(edgePixels > 100);
    assert(ComMethod<HRESULT (WINAPI*)(void*)>(resolved, 10)(resolved) == S_OK);
    auto release = ComMethod<ULONG (WINAPI*)(void*)>(resolved, 2);
    release(resolved);
    ComMethod<ULONG (WINAPI*)(void*)>(source, 2)(source);
    printf("Real x86 wrapper: MSAA %lux resolved a diagonal into %u partially covered edge pixels\n",
           desc.multisample, edgePixels);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    assert(argc == 3);
    BYTE* image = static_cast<BYTE*>(VirtualAlloc(nullptr, 0x460000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    assert(image);
    HANDLE exe = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD size;
    assert(exe != INVALID_HANDLE_VALUE && ReadFile(exe, image, 0x460000, &size, nullptr) && size == 2965504);
    CloseHandle(exe);

    const auto last = sites[std::size(sites) - 1];
    image[last.rva] ^= 1;
    assert(!InstallHooks(image) && image[sites[0].rva] == sites[0].expected[0]);
    image[last.rva] ^= 1;
    assert(InstallHooks(image));
    BYTE* race = image + raceViewSite.rva;
    assert(memcmp(race, raceViewSite.expected, raceViewSite.size) == 0);
    assert(LoadSettings(nullptr, nullptr, nullptr) && !raceInterpolation && msaaSamples == DefaultMsaaSamples);
    assert(memcmp(race, raceViewSite.expected, raceViewSite.size) == 0);
    BYTE* car = image + carViewSite.rva;
    car[1] ^= 1;
    assert(!InstallRaceHooks(image) && memcmp(race, raceViewSite.expected, raceViewSite.size) == 0);
    car[1] ^= 1;
    wchar_t ini[MAX_PATH];
    assert(GetFullPathNameW(L"HP2Display.ini", MAX_PATH, ini, nullptr) < MAX_PATH);
    assert(WritePrivateProfileStringW(L"Interpolation", L"Camera", L"1", ini));
    assert(InitOnceExecuteOnce(&settingsOnce, LoadSettings, nullptr, nullptr) && raceInterpolation);
    assert(race[0] == 0xE8 && memcmp(car, carViewSite.expected, carViewSite.size) != 0);
    for (const auto& site : sites) image[site.rva + site.size] = 0xC3;
    image[movieLayoutSite.rva + movieLayoutSite.size] = 0xC3;
    image[raceViewSite.rva + raceViewSite.size] = 0xC3;
    image[carViewSite.rva + carViewSite.size] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), image, 0x460000);
    VerifyMovieLayout(image + movieLayoutSite.rva);
    VerifyRaceInterpolation(image);
    VerifyFpsLayout();

    adapterMethods[2] = reinterpret_cast<void*>(MockRelease);
    adapterMethods[6] = reinterpret_cast<void*>(ModeCount);
    adapterMethods[7] = reinterpret_cast<void*>(EnumModes);
    adapterMethods[9] = reinterpret_cast<void*>(CheckType);
    adapterMethods[11] = reinterpret_cast<void*>(MockCheckMultisample);
    adapterMethods[15] = reinterpret_cast<void*>(MockCreate);
    deviceMethods[6] = reinterpret_cast<void*>(MockOwner);
    deviceMethods[8] = reinterpret_cast<void*>(MockDisplayMode);
    deviceMethods[9] = reinterpret_cast<void*>(MockCreation);
    deviceMethods[14] = reinterpret_cast<void*>(MockReset);

    VerifyMultisampleSelection();
    PresentParameters input{1920, 1080, 22, 1, 0, 1, nullptr, FALSE, FALSE, 0, 0, 60, 0};
    void* device = nullptr;
    assert(maxRefreshHz == AutoRefresh && ParseRefreshLimit(L"Auto") == AutoRefresh &&
           ParseRefreshLimit(L"360") == 360 && ParseRefreshLimit(L"0") == 0);
    auto rates = EnumerateRefreshRates(&mockD3d, 0, 1, input);
    assert((rates == std::vector<UINT>{360, 240, 165, 144, 120, 60}));
    auto alpha = input;
    alpha.format = 21;
    assert(EnumerateRefreshRates(&mockD3d, 0, 1, alpha) == rates);
    auto smaller = input;
    smaller.width = 1280;
    smaller.height = 720;
    assert(EnumerateRefreshRates(&mockD3d, 0, 1, smaller) == std::vector<UINT>{75});
    struct ExpectedRate { UINT limit, selected; };
    constexpr ExpectedRate checks[] = {{AutoRefresh, 360}, {240, 240}, {165, 165}, {144, 144},
                                       {120, 120}, {60, 60}, {0, 60}};
    for (const auto& check : checks) {
        maxRefreshHz = check.limit;
        auto trial = input;
        assert(SetDisplayMode("Verify refresh ceiling", &mockD3d, 0, 1, &trial,
                              [&](PresentParameters* mode) {
                                  assert(mode->refreshHz == check.selected);
                                  return S_OK;
                              }) == S_OK && trial.refreshHz == check.selected);
    }
    maxRefreshHz = 240;
    auto stock = input;
    assert(RunCreate(image + sites[5].rva, &mockD3d, nullptr, &input, &device) == S_OK &&
           attempts == 2 && input.refreshHz == 165);
    input = stock;
    attempts = 0;
    rejectAll = true;
    assert(RunReset(image + sites[3].rva, device, &input) == S_OK && attempts == 6 && input.refreshHz == 60);
    auto windowed = input;
    windowed.windowed = TRUE;
    assert(SetDisplayMode("Windowed", &mockD3d, 0, 1, &windowed,
                          [&](PresentParameters* mode) {
                              assert(mode == &windowed && mode->refreshHz == input.refreshHz);
                              return S_OK;
                          }) == S_OK);
    puts("Executable guard, x86 thunks, mode selection, fallback and windowed mode passed");

    HMODULE wrapper = LoadLibraryW(argv[2]);
    assert(wrapper);
    auto createD3d = reinterpret_cast<void* (WINAPI*)(UINT)>(GetProcAddress(wrapper, "Direct3DCreate8"));
    assert(createD3d);
    void* d3d = createD3d(220);
    assert(d3d);
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    assert(window);
    input.window = window;
    auto realRates = EnumerateRefreshRates(d3d, 0, 1, input);
    assert(!realRates.empty());
    maxRefreshHz = AutoRefresh;
    showFps = true;
    input.flags = 1; // Use a lockable backbuffer for pixel verification.
    device = nullptr;
    assert(RunCreate(image + sites[6].rva, d3d, window, &input, &device) == S_OK &&
           input.refreshHz == realRates.front());
    VerifyOverlayPixels(device);
    input.refreshHz = 60;
    assert(RunReset(image + sites[4].rva, device, &input) == S_OK && input.refreshHz == realRates.front());
    VerifyOverlayPixels(device);
    for (unsigned i = 0; i < 3; ++i) assert(RunPresent(image + sites[i].rva, device) == S_OK);
    DisplayMode mode{};
    assert(ComMethod<HRESULT (WINAPI*)(void*, DisplayMode*)>(device, 8)(device, &mode) == S_OK);
    assert(mode.width == 1920 && mode.height == 1080 && mode.refreshHz == realRates.front());
    printf("Real x86 wrapper: %ux%u at %u Hz after CreateDevice / Reset / Present\n",
           mode.width, mode.height, mode.refreshHz);
    // Request the release default. Pixel readback below checks the mode actually created.
    msaaSamples = DefaultMsaaSamples;
    input.flags = 0;
    input.autoDepthStencil = TRUE;
    input.depthStencilFormat = 75;
    auto msaaModes = CheckMultisampling(d3d, 0, 1, input);
    if (msaaModes.count) {
        assert(RunReset(image + sites[4].rva, device, &input) == S_OK);
        VerifyAntialiasedPixels(device);
        assert(DrawFpsOverlay(device) && RunPresent(image + sites[0].rva, device) == S_OK);
    } else {
        puts("Real x86 wrapper: no compatible multisampling mode reported");
    }
    ComMethod<ULONG (WINAPI*)(void*)>(device, 2)(device);
    ComMethod<ULONG (WINAPI*)(void*)>(d3d, 2)(d3d);
    DestroyWindow(window);
    FreeLibrary(wrapper);
    VirtualFree(image, 0, MEM_RELEASE);
    VerifyPresentTrace();
}
