#include "AsioCapture.h"
#include "Pcm16VideoEncoder.h"
#include "HamPcmV2Encoder.h"
#include "RealtimeThreadPolicy.h"

#include <asiodrivers.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

// Supplied by Steinberg's host/asiodrivers.cpp.
extern bool loadAsioDriver(char* name);
extern AsioDrivers* asioDrivers;

std::atomic<AsioCapture*> AsioCapture::active_{nullptr};

namespace
{
double medianOfSmallWindow(const std::deque<double>& values, std::size_t count)
{
    if (values.empty() || count == 0)
        return 0.0;

    count = std::min(count, values.size());
    std::vector<double> tmp;
    tmp.reserve(count);

    const auto begin = values.end() - static_cast<std::ptrdiff_t>(count);
    for (auto it = begin; it != values.end(); ++it)
        tmp.push_back(*it);

    const auto mid = tmp.begin() + static_cast<std::ptrdiff_t>(tmp.size() / 2);
    std::nth_element(tmp.begin(), mid, tmp.end());
    return *mid;
}

std::string wideToAnsi(const std::wstring& text)
{
    if (text.empty())
        return {};

    const int count = WideCharToMultiByte(
        CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1)
        return {};

    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_ACP, 0, text.c_str(), -1, out.data(), count, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(count - 1));
    return out;
}

std::wstring ansiToWide(const char* text)
{
    if (text == nullptr || *text == '\0')
        return {};

    const int count = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (count <= 1)
        return {};

    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), count);
    out.resize(static_cast<std::size_t>(count - 1));
    return out;
}

std::wstring asioErrorText(const wchar_t* where, ASIOError error)
{
    return std::wstring(where) + L" failed, ASIO error " +
        std::to_wstring(static_cast<long>(error));
}

std::int32_t readInt24Lsb(const std::uint8_t* p)
{
    std::int32_t v = static_cast<std::int32_t>(p[0]) |
        (static_cast<std::int32_t>(p[1]) << 8) |
        (static_cast<std::int32_t>(p[2]) << 16);
    if ((v & 0x00800000) != 0)
        v |= static_cast<std::int32_t>(0xff000000);
    return v;
}

std::int32_t readInt24Msb(const std::uint8_t* p)
{
    std::int32_t v = (static_cast<std::int32_t>(p[0]) << 16) |
        (static_cast<std::int32_t>(p[1]) << 8) |
        static_cast<std::int32_t>(p[2]);
    if ((v & 0x00800000) != 0)
        v |= static_cast<std::int32_t>(0xff000000);
    return v;
}

std::uint32_t readU32Msb(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
        (static_cast<std::uint32_t>(p[1]) << 16) |
        (static_cast<std::uint32_t>(p[2]) << 8) |
        static_cast<std::uint32_t>(p[3]);
}

std::uint64_t readU64Msb(const std::uint8_t* p)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}

HWND createHiddenAsioHostWindow()
{
    static constexpr wchar_t kClassName[] = L"OpenScopePCMEncoderAsioHostWindow";

    HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;

    const ATOM atom = ::RegisterClassW(&wc);
    if (atom == 0)
    {
        const DWORD error = ::GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
    }

    HWND window = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName,
        L"OpenScope ASIO Host",
        WS_POPUP,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        instance,
        nullptr);

    return window;
}
}

AsioCapture::AsioCapture() = default;

AsioCapture::~AsioCapture()
{
    stop();
}

std::vector<AudioInputDevice> AsioCapture::enumerate()
{
    std::vector<AudioInputDevice> result;

    AsioDrivers drivers;
    constexpr long MaxDrivers = 64;
    char names[MaxDrivers][64]{};
    char* namePtrs[MaxDrivers]{};
    for (long i = 0; i < MaxDrivers; ++i)
        namePtrs[i] = names[i];

    const long count = drivers.getDriverNames(namePtrs, MaxDrivers);
    result.reserve(static_cast<std::size_t>(std::max(0L, count)));

    for (long i = 0; i < count; ++i)
    {
        const std::wstring name = ansiToWide(names[i]);
        if (!name.empty())
            result.push_back({name, name});
    }

    return result;
}

bool AsioCapture::start(const std::wstring& id)
{
    stop();
    asioCallbackCount_ = 0;
    asioSampleFrameCount_ = 0;
    asioTimingSequence_ = 0;
    asioLastCallbackHostNs_ = 0;
    asioTimingHistoryWrite_ = 0;
    asioLatestTimingFrames_ = 0;
    asioLatestTimingQpcTicks_ = 0;
    for (auto& slot : asioTimingHistory_)
        slot.serial.store(0, std::memory_order_relaxed);
    LARGE_INTEGER qpcFrequency{};
    qpcFrequency_ = QueryPerformanceFrequency(&qpcFrequency)
        ? qpcFrequency.QuadPart
        : 0;
    restartRequested_ = false;

    addDebugLogLine("OpenScope PCM Encoder 0.5.02b coherent parser/build fix");
    addDebugLogLine("ASIO START request driver=\"" + wideToAnsi(id) + "\"");

    {
        std::lock_guard lock(mutex_);
        error_.clear();
        bufferNudge_.reset();
        pendingQpcMargin_ = {};
        txCurrentMarginMs_ = 0.0;
        txCurrentAverageMarginMs_ = 0.0;
        txContinuousBufferMs_ = 0.0;
        txContinuousAverageBufferMs_ = 0.0;
    txSlowAverageMargin3sMs_ = 0.0;
        txSlowAverageMargin3sMs_ = 0.0;
        asrc_.reset(48000.0);
        hamAsrc_.reset(48000.0);
        pcmQueuedFramesPublished_.store(asrc_.queuedOutputFrames(), std::memory_order_release);
        hamQueuedFramesPublished_.store(hamAsrc_.queuedOutputFrames(), std::memory_order_release);
        asrc_.setClockCorrectionPpm(0.0);
        hamAsrc_.setClockCorrectionPpm(0.0);
    }

    inputRate_ = 0.0;
    asioBufferFrames_ = 0;
    asioBufferMinFrames_ = 0;
    asioBufferMaxFrames_ = 0;
    asioBufferPreferredFrames_ = 0;
    asioBufferGranularity_ = 0;
    txMarginMs_ = 0.0;
    txAverageMarginMs_ = 0.0;
    txMarginSequence_ = 0;
    asrcAppliedPpm_ = 0.0;
    asrcNudgePpm_ = 0.0;
    asrcActualStep_ = 0.0;
    txContinuousBufferMs_ = 0.0;
    nudgeActive_ = false;
    peakL_ = 0.0f;
    peakR_ = 0.0f;

    std::string driverName = wideToAnsi(id);
    if (driverName.empty() || !loadAsioDriver(driverName.data()))
    {
        setError(L"Could not load ASIO driver: " + id);
        stop();
        return false;
    }
    driverLoaded_ = true;
    addDebugLogLine("ASIO driver loaded");

    // Only the sysRef HWND is moved off the Qt thread. ASIO driver loading,
    // ASIOInit/CreateBuffers/Start and Stop/Exit stay on this caller thread,
    // identical to the known-working 0.4.15 lifecycle.
    if (!startAsioHostWindowThread())
    {
        setError(L"Could not create dedicated hidden ASIO host window.");
        stop();
        return false;
    }

    ASIODriverInfo info{};
    info.asioVersion = 2;
    info.sysRef = asioHostWindow_;

    ASIOError err = ASIOInit(&info);
    if (err != ASE_OK && err != ASE_SUCCESS)
    {
        setError(asioErrorText(L"ASIOInit", err));
        stop();
        return false;
    }
    asioInitialized_ = true;
    {
        std::ostringstream os;
        os << "ASIO INIT ok"
           << " name=\"" << info.name << "\""
           << " version=" << info.driverVersion;
        if (info.errorMessage[0] != '\0')
            os << " info=\"" << info.errorMessage << "\"";
        addDebugLogLine(os.str());
    }

    long inputs = 0;
    long outputs = 0;
    err = ASIOGetChannels(&inputs, &outputs);
    if ((err != ASE_OK && err != ASE_SUCCESS) || inputs < 1)
    {
        setError(L"ASIO driver has no usable input channels.");
        stop();
        return false;
    }
    activeInputChannels_ = std::min(2L, inputs);
    {
        std::ostringstream os;
        os << "ASIO CHANNELS inputs=" << inputs
           << " outputs=" << outputs
           << " usingInputs=" << activeInputChannels_;
        addDebugLogLine(os.str());
    }

    // 0.4.04 is deliberately fixed: 48 kHz ASIO input, 44.1 kHz ASRC output.
    ASIOSampleRate sampleRate = 0.0;
    err = ASIOGetSampleRate(&sampleRate);
    if ((err != ASE_OK && err != ASE_SUCCESS) ||
        !std::isfinite(sampleRate) ||
        std::abs(sampleRate - 48000.0) > 0.5)
    {
        if (ASIOCanSampleRate(48000.0) == ASE_OK &&
            ASIOSetSampleRate(48000.0) == ASE_OK)
        {
            sampleRate = 48000.0;
        }
        else
        {
            setError(L"0.4.04 requires a 48 kHz ASIO input rate.");
            stop();
            return false;
        }
    }
    else
    {
        sampleRate = 48000.0;
    }

    inputRate_ = sampleRate;
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(3)
           << "ASIO SAMPLE RATE " << sampleRate << " Hz";
        addDebugLogLine(os.str());
    }
    {
        std::lock_guard lock(mutex_);
        asrc_.reset(sampleRate);
        hamAsrc_.reset(sampleRate);
        pcmQueuedFramesPublished_.store(asrc_.queuedOutputFrames(), std::memory_order_release);
        hamQueuedFramesPublished_.store(hamAsrc_.queuedOutputFrames(), std::memory_order_release);
        asrc_.setClockCorrectionPpm(0.0);
        hamAsrc_.setClockCorrectionPpm(0.0);
    }

    long minSize = 0;
    long maxSize = 0;
    long preferredSize = 0;
    long granularity = 0;
    err = ASIOGetBufferSize(&minSize, &maxSize, &preferredSize, &granularity);
    if ((err != ASE_OK && err != ASE_SUCCESS) || preferredSize <= 0)
    {
        setError(asioErrorText(L"ASIOGetBufferSize", err));
        stop();
        return false;
    }
    asioBufferMinFrames_ = minSize;
    asioBufferMaxFrames_ = maxSize;
    asioBufferPreferredFrames_ = preferredSize;
    asioBufferGranularity_ = granularity;
    {
        std::ostringstream os;
        os << "ASIO BUFFER CAPS min=" << minSize
           << " max=" << maxSize
           << " preferred=" << preferredSize
           << " granularity=" << granularity;
        addDebugLogLine(os.str());
    }

    // Timing experiment: prefer a 16-sample ASIO quantum when the driver
    // explicitly allows it. Fall back to the driver's preferred size when
    // 16 samples is not a legal buffer size.
    constexpr long RequestedBufferSize = 16;

    auto isLegalAsioBufferSize = [&](long frames)
    {
        if (frames < minSize || frames > maxSize)
            return false;

        if (granularity == 0)
            return frames == preferredSize;

        if (granularity == -1)
            return frames > 0 && (frames & (frames - 1)) == 0;

        if (granularity > 0)
            return ((frames - minSize) % granularity) == 0;

        return false;
    };

    const long bufferSize =
        isLegalAsioBufferSize(RequestedBufferSize)
            ? RequestedBufferSize
            : preferredSize;

    asioBufferFrames_ = bufferSize;
    {
        std::ostringstream os;
        const double bufferPeriodMs =
            1000.0 * static_cast<double>(bufferSize) / sampleRate;
        os << "ASIO BUFFER CHOSEN requested=" << RequestedBufferSize
           << " actual=" << bufferSize
           << " periodMs="
           << std::fixed << std::setprecision(3) << bufferPeriodMs
           << " nudgeTriggerMs=" << (1.5 * bufferPeriodMs)
           << " defaultTargetMs=" << std::lround(4.0 * bufferPeriodMs)
           << " expectedCallbacksPerSec="
           << std::setprecision(2)
           << (sampleRate / static_cast<double>(bufferSize));
        addDebugLogLine(os.str());
    }
    stereoScratch_.assign(static_cast<std::size_t>(bufferSize) * 2, 0.0f);

    for (long c = 0; c < activeInputChannels_; ++c)
    {
        bufferInfos_[static_cast<std::size_t>(c)] = {};
        bufferInfos_[static_cast<std::size_t>(c)].isInput = ASIOTrue;
        bufferInfos_[static_cast<std::size_t>(c)].channelNum = c;

        channelInfos_[static_cast<std::size_t>(c)] = {};
        channelInfos_[static_cast<std::size_t>(c)].channel = c;
        channelInfos_[static_cast<std::size_t>(c)].isInput = ASIOTrue;

        err = ASIOGetChannelInfo(&channelInfos_[static_cast<std::size_t>(c)]);
        if (err != ASE_OK && err != ASE_SUCCESS)
        {
            setError(asioErrorText(L"ASIOGetChannelInfo", err));
            stop();
            return false;
        }

        {
            const auto& ci = channelInfos_[static_cast<std::size_t>(c)];
            std::ostringstream os;
            os << "ASIO INPUT ch=" << c
               << " type=" << static_cast<long>(ci.type)
               << " name=\"" << ci.name << "\"";
            addDebugLogLine(os.str());
        }
    }

    callbacks_ = {};
    callbacks_.bufferSwitch = &AsioCapture::bufferSwitchStatic;
    callbacks_.sampleRateDidChange = &AsioCapture::sampleRateDidChangeStatic;
    callbacks_.asioMessage = &AsioCapture::asioMessageStatic;
    callbacks_.bufferSwitchTimeInfo = &AsioCapture::bufferSwitchTimeInfoStatic;

    active_.store(this, std::memory_order_release);
    err = ASIOCreateBuffers(
        bufferInfos_.data(),
        activeInputChannels_,
        bufferSize,
        &callbacks_);
    if (err != ASE_OK && err != ASE_SUCCESS)
    {
        setError(asioErrorText(L"ASIOCreateBuffers", err));
        active_.store(nullptr, std::memory_order_release);
        stop();
        return false;
    }
    buffersCreated_ = true;
    addDebugLogLine("ASIO CREATE BUFFERS ok");

    {
        long inputLatency = 0;
        long outputLatency = 0;
        const ASIOError latencyErr = ASIOGetLatencies(&inputLatency, &outputLatency);
        if (latencyErr == ASE_OK || latencyErr == ASE_SUCCESS)
        {
            std::ostringstream os;
            os << "ASIO LATENCIES input=" << inputLatency
               << " output=" << outputLatency << " frames";
            addDebugLogLine(os.str());
        }
        else
        {
            std::ostringstream os;
            os << "ASIO LATENCIES query failed error="
               << static_cast<long>(latencyErr);
            addDebugLogLine(os.str());
        }
    }

    run_ = true;
    err = ASIOStart();
    if (err != ASE_OK && err != ASE_SUCCESS)
    {
        setError(asioErrorText(L"ASIOStart", err));
        run_ = false;
        stop();
        return false;
    }
    asioStarted_ = true;
    addDebugLogLine("ASIO START ok");
    return true;
}

void AsioCapture::stop()
{
    const bool hadAsio = asioStarted_ || buffersCreated_ || asioInitialized_ || driverLoaded_;
    if (hadAsio)
        addDebugLogLine("ASIO STOP begin");

    run_ = false;
    wakeAudioWaiters();

    if (asioStarted_)
    {
        ASIOStop();
        asioStarted_ = false;
    }

    active_.store(nullptr, std::memory_order_release);

    if (buffersCreated_)
    {
        ASIODisposeBuffers();
        buffersCreated_ = false;
    }

    if (asioInitialized_)
    {
        // On Windows ASIOExit() also removes the current driver from the
        // Steinberg AsioDrivers helper.
        ASIOExit();
        asioInitialized_ = false;
        driverLoaded_ = false;
    }
    else if (driverLoaded_)
    {
        // ASIOInit may have failed after loadAsioDriver succeeded.
        if (asioDrivers != nullptr)
            asioDrivers->removeCurrentDriver();
        driverLoaded_ = false;
    }

    // Keep sysRef valid until ASIOStop/ASIOExit have completed, then let the
    // window-owning thread destroy it and exit.
    stopAsioHostWindowThread();

    activeInputChannels_ = 0;
    asioBufferFrames_ = 0;
    stereoScratch_.clear();

    if (hadAsio)
        addDebugLogLine("ASIO STOP complete");
}


bool AsioCapture::startAsioHostWindowThread()
{
    {
        std::lock_guard lock(asioHostWindowMutex_);
        asioHostWindowReady_ = false;
        asioHostWindowOk_ = false;
    }

    asioHostWindowThread_ =
        std::thread(&AsioCapture::asioHostWindowThreadMain, this);

    std::unique_lock lock(asioHostWindowMutex_);
    asioHostWindowCv_.wait(
        lock,
        [&] { return asioHostWindowReady_; });

    return asioHostWindowOk_;
}

void AsioCapture::stopAsioHostWindowThread()
{
    const DWORD threadId =
        asioHostWindowThreadId_.load(std::memory_order_acquire);

    if (threadId != 0)
        PostThreadMessageW(threadId, WM_APP + 29, 0, 0);

    if (asioHostWindowThread_.joinable())
        asioHostWindowThread_.join();

    asioHostWindowThreadId_ = 0;
    asioHostWindow_ = nullptr;
}

void AsioCapture::asioHostWindowThreadMain()
{
    asioHostWindowThreadId_ = GetCurrentThreadId();

    // Force creation of this thread's Win32 message queue before publishing
    // the thread id to stop().
    MSG bootstrap{};
    PeekMessageW(&bootstrap, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    HWND window = createHiddenAsioHostWindow();

    {
        std::lock_guard lock(asioHostWindowMutex_);
        asioHostWindow_ = window;
        asioHostWindowOk_ = window != nullptr;
        asioHostWindowReady_ = true;
    }
    asioHostWindowCv_.notify_all();

    if (window == nullptr)
    {
        asioHostWindowThreadId_ = 0;
        return;
    }

    MSG message{};
    while (true)
    {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0)
            break;

        if (message.message == WM_APP + 29)
            break;

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DestroyWindow(window);
    asioHostWindowThreadId_ = 0;
}

std::size_t AsioCapture::queuedFrames() const
{
    std::lock_guard lock(mutex_);
    return hamMode_.load() ? hamAsrc_.queuedOutputFrames() : asrc_.queuedOutputFrames();
}


double AsioCapture::bmdBufferFillMsSnapshot() const
{
    const bool ham = hamMode_.load(std::memory_order_relaxed);
    const std::size_t frames = ham
        ? hamQueuedFramesPublished_.load(std::memory_order_acquire)
        : pcmQueuedFramesPublished_.load(std::memory_order_acquire);
    const double rate = ham ? HamAudioAsrc::OutputRate : AudioAsrc::OutputRate;
    return 1000.0 * static_cast<double>(frames) / rate;
}

void AsioCapture::setBufferMs(int bufferMs)
{
    bufferMs_.store(std::clamp(bufferMs, 4, 100), std::memory_order_relaxed);
}

int AsioCapture::bufferMs() const
{
    return bufferMs_.load(std::memory_order_relaxed);
}

double AsioCapture::outputSampleRate() const
{
    std::lock_guard lock(mutex_);
    return hamMode_.load() ? hamAsrc_.outputRate() : asrc_.outputRate();
}

bool AsioCapture::readyForPalFrame() const
{
    if (inputRate_.load() <= 1.0)
        return false;

    // Start only when the active elastic FIFO has reached the one shared
    // transport reserve target. The required frame count is derived here from
    // the selected output rate; the ASRC classes do not own buffer policy.
    std::lock_guard lock(mutex_);
    const bool ham = hamMode_.load();
    const double rate = ham ? hamAsrc_.outputRate() : asrc_.outputRate();
    const std::size_t targetFrames = static_cast<std::size_t>(std::max<long long>(
        0LL, std::llround(rate * static_cast<double>(bufferMs()) / 1000.0)));
    const std::size_t queuedFrames =
        ham ? hamAsrc_.queuedOutputFrames() : asrc_.queuedOutputFrames();
    return queuedFrames >= targetFrames;
}

void AsioCapture::beginRebuffer()
{
    std::lock_guard lock(mutex_);

    // Buffer timing/control is global to the ASIO -> DeckLink path. Rebuffering
    // therefore resets one shared controller, independent of PCM encoding mode.
    bufferNudge_.reset();
    txContinuousBufferMs_ = 0.0;
    txContinuousAverageBufferMs_ = 0.0;
    txSlowAverageMargin3sMs_ = 0.0;
    asrc_.setClockCorrectionPpm(0.0);
    hamAsrc_.setClockCorrectionPpm(0.0);
    txAverageMarginMs_ = 0.0;
    asrcAppliedPpm_ = 0.0;
    asrcNudgePpm_ = 0.0;
    nudgeActive_ = false;
    maintenanceHoldActive_ = false;
    maintenanceTriggerTiming_ = false;

    // This path is a PRE-TRANSMIT refill/wait. No audio has been lost yet,
    // so it must not increment the real-starvation counter.
    if (hamMode_.load())
        hamAsrc_.beginRebuffer(false);
    else
        asrc_.beginRebuffer(false);
}

bool AsioCapture::hasCompletePalFrame() const
{
    if (inputRate_.load() <= 1.0)
        return false;

    std::lock_guard lock(mutex_);
    if (hamMode_.load())
        return hamAsrc_.queuedOutputFrames() >=
            static_cast<std::size_t>(HamPcmV2Encoder::SamplesPerFrame);

    return asrc_.queuedOutputFrames() >=
        static_cast<std::size_t>(Pcm16VideoEncoder::SamplesPerFrame);
}

std::uint64_t AsioCapture::underruns() const
{
    std::lock_guard lock(mutex_);
    return hamMode_.load() ? hamAsrc_.underruns() : asrc_.underruns();
}

bool AsioCapture::rebuffering() const
{
    std::lock_guard lock(mutex_);
    return hamMode_.load() ? hamAsrc_.rebuffering() : asrc_.rebuffering();
}

std::wstring AsioCapture::lastError() const
{
    std::lock_guard lock(mutex_);
    return error_;
}

std::size_t AsioCapture::drainStereoFrames(
    std::vector<std::int16_t>& destination,
    std::size_t maxFrames)
{
    std::lock_guard lock(mutex_);
    const std::size_t drained = asrc_.drainAvailableInt16(destination, maxFrames);
    pcmQueuedFramesPublished_.store(asrc_.queuedOutputFrames(), std::memory_order_release);
    return drained;
}

std::size_t AsioCapture::drainHamStereoFrames(
    std::vector<std::int16_t>& destination,
    std::size_t maxFrames)
{
    std::lock_guard lock(mutex_);
    const std::size_t drained = hamAsrc_.drainAvailableInt16(destination, maxFrames);
    hamQueuedFramesPublished_.store(hamAsrc_.queuedOutputFrames(), std::memory_order_release);
    return drained;
}


void AsioCapture::waitForAudioData(std::uint64_t afterSequence)
{
    std::unique_lock lock(audioDataWaitMutex_);
    audioDataCondition_.wait(
        lock,
        [&]
        {
            return !run_.load(std::memory_order_acquire) ||
                   audioDataSequence_.load(std::memory_order_acquire) != afterSequence;
        });
}

void AsioCapture::wakeAudioWaiters()
{
    audioDataSequence_.fetch_add(1, std::memory_order_release);
    audioDataCondition_.notify_all();
}


void AsioCapture::addDebugLogLine(const std::string& text)
{
    std::lock_guard lock(mutex_);
    appendDebugLogLocked(text);
}

bool AsioCapture::consumeRestartRequested()
{
    return restartRequested_.exchange(false, std::memory_order_acq_rel);
}

std::vector<std::string> AsioCapture::takeDebugLogLines()
{
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.reserve(debugLogLines_.size());
    while (!debugLogLines_.empty())
    {
        out.push_back(std::move(debugLogLines_.front()));
        debugLogLines_.pop_front();
    }
    return out;
}

void AsioCapture::appendDebugLogLocked(const std::string& text)
{
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto epochMs = duration_cast<milliseconds>(now.time_since_epoch()).count();
    const auto msPart = static_cast<int>((epochMs % 1000 + 1000) % 1000);
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &tt);

    std::ostringstream line;
    line << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
         << '.' << std::setw(3) << std::setfill('0') << msPart
         << "  " << text;

    constexpr std::size_t MaxDebugLines = 4000;
    if (debugLogLines_.size() >= MaxDebugLines)
        debugLogLines_.pop_front();
    debugLogLines_.push_back(line.str());
}


void AsioCapture::updateBufferNudge(
    double bmdBufferFillMs,
    double completionMarginMs,
    double sampleDtSec,
    double bmdCallbackElapsedMs,
    double bmdIdealElapsedMs,
    std::int64_t frameReadyQpcTicks,
    std::int64_t bmdCallbackQpcTicks)
{
    (void)bmdBufferFillMs;
    (void)bmdCallbackElapsedMs;
    (void)bmdIdealElapsedMs;

    const double asioRate = inputRate_.load(std::memory_order_relaxed);
    const long asioFrames = asioBufferFrames_.load(std::memory_order_relaxed);

    if (!std::isfinite(asioRate) || asioRate <= 0.0 || asioFrames <= 0)
        return;

    const double asioBufferPeriodMs =
        1000.0 * static_cast<double>(asioFrames) / asioRate;

    std::lock_guard lock(mutex_);

    // 0.4.96: back to the cheap/proven measurement.
    // Measure READY -> Blackmagic ScheduledFrameCompleted and use only that
    // POSITION signal. No ASRC FIFO depth, no slope, no PID, no clock estimate.
    double rawQpcMarginMs = completionMarginMs;
    const std::int64_t qpcHz = qpcFrequency_.load(std::memory_order_relaxed);
    if (qpcHz > 0 && frameReadyQpcTicks > 0 && bmdCallbackQpcTicks > 0)
    {
        rawQpcMarginMs = std::max(
            0.0,
            1000.0 * static_cast<double>(
                bmdCallbackQpcTicks - frameReadyQpcTicks) /
                static_cast<double>(qpcHz));
    }

    // Keep the one-frame delayed ASIO/QPC interpolation from the old cheap
    // version when it resolves cleanly; otherwise use the direct QPC margin.
    double marginForControlMs = rawQpcMarginMs;
    if (pendingQpcMargin_.valid)
    {
        marginForControlMs = pendingQpcMargin_.rawQpcMarginMs;

        double readySamplePosition = 0.0;
        double bmdSamplePosition = 0.0;
        if (interpolateAsioPositionAtQpc(
                pendingQpcMargin_.frameReadyQpcTicks,
                readySamplePosition) &&
            interpolateAsioPositionAtQpc(
                pendingQpcMargin_.bmdCallbackQpcTicks,
                bmdSamplePosition) &&
            bmdSamplePosition >= readySamplePosition)
        {
            marginForControlMs =
                1000.0 * (bmdSamplePosition - readySamplePosition) / asioRate;
        }
    }

    pendingQpcMargin_.valid =
        qpcHz > 0 && frameReadyQpcTicks > 0 && bmdCallbackQpcTicks > 0;
    pendingQpcMargin_.frameReadyQpcTicks = frameReadyQpcTicks;
    pendingQpcMargin_.bmdCallbackQpcTicks = bmdCallbackQpcTicks;
    pendingQpcMargin_.rawQpcMarginMs = rawQpcMarginMs;

    completionMarginMs = std::max(0.0, marginForControlMs);

    // 0.4.97: slow reported buffer depth = exact time-weighted mean of the
    // most recent 3 seconds of completion-margin samples.
    // This is DISPLAY/REPORTING ONLY. The position servo still uses its own
    // fast average and is otherwise unchanged.
    {
        constexpr double SlowAverageSec = 3.0;
        const double dt = std::clamp(sampleDtSec, 0.001, 0.250);

        slowMarginAverage3sWindow_.push_back({dt, completionMarginMs});
        slowMarginAverage3sWindowSec_ += dt;
        slowMarginAverage3sWeightedSum_ += dt * completionMarginMs;

        while (!slowMarginAverage3sWindow_.empty() &&
               slowMarginAverage3sWindowSec_ > SlowAverageSec)
        {
            double excess = slowMarginAverage3sWindowSec_ - SlowAverageSec;
            auto& front = slowMarginAverage3sWindow_.front();

            if (front.first <= excess + 1.0e-12)
            {
                slowMarginAverage3sWindowSec_ -= front.first;
                slowMarginAverage3sWeightedSum_ -= front.first * front.second;
                slowMarginAverage3sWindow_.pop_front();
            }
            else
            {
                front.first -= excess;
                slowMarginAverage3sWindowSec_ -= excess;
                slowMarginAverage3sWeightedSum_ -= excess * front.second;
                break;
            }
        }

        if (slowMarginAverage3sWindowSec_ > 1.0e-9)
        {
            slowAverageMargin3sMs_ =
                slowMarginAverage3sWeightedSum_ / slowMarginAverage3sWindowSec_;
            slowAverageMargin3sValid_ = true;
            txSlowAverageMargin3sMs_ = slowAverageMargin3sMs_;
        }
    }

    const double resetMarginThresholdMs = 0.5 * asioBufferPeriodMs;
    if (completionMarginMs <= resetMarginThresholdMs)
    {
        appendDebugLogLocked(
            "BUFFER RESET EVENT margin=" + std::to_string(completionMarginMs) +
            " ms threshold=" + std::to_string(resetMarginThresholdMs) + " ms");

        bufferNudge_.reset();
        pendingQpcMargin_ = {};

        slowMarginAverage3sWindow_.clear();
        slowMarginAverage3sWindowSec_ = 0.0;
        slowMarginAverage3sWeightedSum_ = 0.0;
        slowAverageMargin3sMs_ = completionMarginMs;
        slowAverageMargin3sValid_ = true;
        txSlowAverageMargin3sMs_ = completionMarginMs;
        asrc_.setClockCorrectionPpm(0.0);
        hamAsrc_.setClockCorrectionPpm(0.0);
        asrcAppliedPpm_ = 0.0;
        asrcNudgePpm_ = 0.0;
        nudgeActive_ = false;
        maintenanceHoldActive_ = false;
        maintenanceTriggerTiming_ = false;
        txMarginMs_ = completionMarginMs;
        txCurrentMarginMs_ = completionMarginMs;
        txAverageMarginMs_ = completionMarginMs;
        ++txMarginSequence_;
        return;
    }

    const double targetMarginMs = static_cast<double>(bufferMs());

    // AsioQuantumLeap: if a settled system drifts more than 3 ASIO quanta
    // away from target, treat it
    // exactly like a fresh acquisition. No slope/clock estimate: just re-arm
    // the existing position ladder and keep bumping until it is back.
    //
    // Use either the fast control average or the reported 3 s depth. The fast
    // path catches a genuinely bad clock promptly; the slow path catches a
    // sustained displacement even if the short average happens to wander back.
    if (!bufferNudge_.acquisitionActive())
    {
        const double fastErrorMs =
            bufferNudge_.marginValid()
                ? bufferNudge_.averageMarginMs() - targetMarginMs
                : completionMarginMs - targetMarginMs;

        const double average3sErrorMs =
            slowAverageMargin3sValid_
                ? slowAverageMargin3sMs_ - targetMarginMs
                : fastErrorMs;

        const double reacquireThresholdMs = 3.0 * asioBufferPeriodMs;
        if (std::abs(fastErrorMs) > reacquireThresholdMs ||
            std::abs(average3sErrorMs) > reacquireThresholdMs)
        {
            appendDebugLogLocked(
                "REACQUIRE position error fast=" + std::to_string(fastErrorMs) +
                " ms slow3=" + std::to_string(average3sErrorMs) + " ms");

            bufferNudge_.reset();
        }
    }

    // Two-regime, position-only servo:
    // fast acquisition after reset/target change, calm maintenance afterwards.
    bufferNudge_.update(
        completionMarginMs,
        targetMarginMs,
        sampleDtSec,
        asioBufferPeriodMs);

    const double positionNudgePpm = bufferNudge_.appliedPpm();

    // No automatic steady ppm trim. Manual ppm remains independent.
    asrc_.setClockCorrectionPpm(positionNudgePpm);
    hamAsrc_.setClockCorrectionPpm(positionNudgePpm);

    const double averageMarginMs = bufferNudge_.averageMarginMs();

    const double reportedAverage3sMarginMs =
        slowAverageMargin3sValid_ ? slowAverageMargin3sMs_ : averageMarginMs;

    txMarginMs_ = completionMarginMs;
    txCurrentMarginMs_ = completionMarginMs;
    txCurrentAverageMarginMs_ = averageMarginMs;
    // What the UI calls/reports as actual buffer depth is deliberately slow:
    // the time-weighted average over the last 3 seconds.
    txAverageMarginMs_ = reportedAverage3sMarginMs;
    txSlowAverageMargin3sMs_ = reportedAverage3sMarginMs;

    // Re-use these mirrors for the graph, but they now contain completion
    // margin rather than FIFO depth. The old PID path is completely inactive.
    txContinuousBufferMs_ = completionMarginMs;
    txContinuousAverageBufferMs_ = averageMarginMs;

    asrcAppliedPpm_ = positionNudgePpm;
    asrcNudgePpm_ = positionNudgePpm;
    nudgeActive_ = bufferNudge_.nudgeActive();
    maintenanceHoldActive_ = bufferNudge_.maintenanceHoldActive();
    maintenanceTriggerTiming_ = bufferNudge_.maintenanceTriggerTiming();

    // Simple once-per-second logging; no FIFO/median/slope machinery required.
    using namespace std::chrono;
    const std::int64_t nowMs = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    if (debugLastSnapshotMs_ == 0 || nowMs - debugLastSnapshotMs_ >= 1000)
    {
        auto fmt = [](double v, int n = 3)
        {
            std::ostringstream os;
            os.setf(std::ios::fixed);
            os << std::setprecision(n) << v;
            return os.str();
        };

        appendDebugLogLocked(
            "STATE margin=" + fmt(completionMarginMs, 3) + " ms" +
            " fastAvg=" + fmt(averageMarginMs, 3) + " ms" +
            " avg3s=" + fmt(reportedAverage3sMarginMs, 3) + " ms" +
            " target=" + fmt(targetMarginMs, 2) + " ms" +
            " error=" + fmt(averageMarginMs - targetMarginMs, 3) + " ms" +
            " servo=" + fmt(positionNudgePpm, 0) + " ppm" +
            " mode=" + std::string(bufferNudge_.acquisitionActive()
                ? "acquire"
                : (bufferNudge_.maintenanceHoldActive()
                    ? "settle"
                    : (bufferNudge_.maintenanceTriggerTiming()
                        ? "watch"
                        : "quiet"))));

        debugLastSnapshotMs_ = nowMs;
    }

    ++txMarginSequence_;
}

std::vector<std::int16_t> AsioCapture::takeStereoFrames(std::size_t frames)
{
    std::lock_guard lock(mutex_);
    auto result = asrc_.takeInt16(frames);
    pcmQueuedFramesPublished_.store(asrc_.queuedOutputFrames(), std::memory_order_release);
    return result;
}

std::vector<std::int16_t> AsioCapture::takeHamStereoFrames(std::size_t frames)
{
    std::lock_guard lock(mutex_);
    auto result = hamAsrc_.takeInt16(frames);
    hamQueuedFramesPublished_.store(hamAsrc_.queuedOutputFrames(), std::memory_order_release);
    return result;
}

void AsioCapture::publishAsioTimingPoint(
    std::uint64_t frames,
    std::int64_t qpcTicks) noexcept
{
    if (qpcTicks <= 0)
        return;

    // Single ASIO callback writer. Publish each slot last and then advance the
    // global serial so readers never need a mutex in the real-time callback.
    const std::uint64_t serial =
        asioTimingHistoryWrite_.load(std::memory_order_relaxed) + 1;
    auto& slot = asioTimingHistory_[(serial - 1) % AsioTimingHistorySize];

    slot.serial.store(0, std::memory_order_release);
    slot.frames.store(frames, std::memory_order_relaxed);
    slot.qpcTicks.store(qpcTicks, std::memory_order_relaxed);
    slot.serial.store(serial, std::memory_order_release);
    asioTimingHistoryWrite_.store(serial, std::memory_order_release);
    asioLatestTimingFrames_.store(frames, std::memory_order_release);
    asioLatestTimingQpcTicks_.store(qpcTicks, std::memory_order_release);
}

bool AsioCapture::interpolateAsioPositionAtQpc(
    std::int64_t eventQpcTicks,
    double& samplePosition) const noexcept
{
    if (eventQpcTicks <= 0)
        return false;

    const std::uint64_t newest =
        asioTimingHistoryWrite_.load(std::memory_order_acquire);
    if (newest < 2)
        return false;

    const std::uint64_t oldest = newest > AsioTimingHistorySize
        ? newest - AsioTimingHistorySize + 1
        : 1;

    bool haveBefore = false;
    bool haveAfter = false;
    std::int64_t beforeQpc = 0;
    std::int64_t afterQpc = 0;
    std::uint64_t beforeFrames = 0;
    std::uint64_t afterFrames = 0;

    for (std::uint64_t serial = oldest; serial <= newest; ++serial)
    {
        const auto& slot =
            asioTimingHistory_[(serial - 1) % AsioTimingHistorySize];

        const std::uint64_t serialBefore =
            slot.serial.load(std::memory_order_acquire);
        if (serialBefore != serial)
            continue;

        const std::uint64_t frames =
            slot.frames.load(std::memory_order_relaxed);
        const std::int64_t qpc =
            slot.qpcTicks.load(std::memory_order_relaxed);

        const std::uint64_t serialAfter =
            slot.serial.load(std::memory_order_acquire);
        if (serialAfter != serialBefore || qpc <= 0)
            continue;

        if (qpc <= eventQpcTicks &&
            (!haveBefore || qpc > beforeQpc))
        {
            haveBefore = true;
            beforeQpc = qpc;
            beforeFrames = frames;
        }

        if (qpc >= eventQpcTicks &&
            (!haveAfter || qpc < afterQpc))
        {
            haveAfter = true;
            afterQpc = qpc;
            afterFrames = frames;
        }
    }

    if (!haveBefore || !haveAfter)
        return false;

    if (afterQpc == beforeQpc)
    {
        samplePosition = static_cast<double>(beforeFrames);
        return true;
    }

    if (afterQpc < beforeQpc || afterFrames < beforeFrames)
        return false;

    const double fraction = std::clamp(
        static_cast<double>(eventQpcTicks - beforeQpc) /
            static_cast<double>(afterQpc - beforeQpc),
        0.0,
        1.0);

    samplePosition =
        static_cast<double>(beforeFrames) +
        fraction * static_cast<double>(afterFrames - beforeFrames);
    return true;
}

double AsioCapture::asioBufferMs() const
{
    const double rate = inputRate_.load();
    const long frames = asioBufferFrames_.load();
    if (rate <= 1.0 || frames <= 0)
        return 0.0;
    return static_cast<double>(frames) * 1000.0 / rate;
}

void AsioCapture::setError(const std::wstring& error)
{
    std::lock_guard lock(mutex_);
    error_ = error;
    appendDebugLogLocked("ASIO ERROR " + wideToAnsi(error));
}

void AsioCapture::bufferSwitchStatic(long doubleBufferIndex, ASIOBool directProcess)
{
    (void)directProcess;

    thread_local RealtimeThreadPolicy realtimePolicy(L"Pro Audio");
    (void)realtimePolicy;

    if (auto* self = active_.load(std::memory_order_acquire))
    {
        ++self->asioCallbackCount_;
        const long frames = self->asioBufferFrames_.load(std::memory_order_relaxed);

        // Publish an exact ASIO callback endpoint: sample count and host time
        // belong to the same callback. The sequence makes the pair readable
        // without a mutex from the DeckLink thread.
        self->asioTimingSequence_.fetch_add(1, std::memory_order_acq_rel); // odd
        std::uint64_t framesAfter =
            self->asioSampleFrameCount_.load(std::memory_order_relaxed);
        if (frames > 0)
        {
            framesAfter = self->asioSampleFrameCount_.fetch_add(
                static_cast<std::uint64_t>(frames),
                std::memory_order_relaxed) + static_cast<std::uint64_t>(frames);
        }
        const auto callbackNow = std::chrono::steady_clock::now();
        const auto callbackNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            callbackNow.time_since_epoch()).count();
        self->asioLastCallbackHostNs_.store(callbackNs, std::memory_order_relaxed);

        LARGE_INTEGER callbackQpc{};
        if (QueryPerformanceCounter(&callbackQpc))
            self->publishAsioTimingPoint(framesAfter, callbackQpc.QuadPart);

        self->asioTimingSequence_.fetch_add(1, std::memory_order_release); // even

        self->processBuffer(doubleBufferIndex);
    }
}

ASIOTime* AsioCapture::bufferSwitchTimeInfoStatic(
    ASIOTime* params,
    long doubleBufferIndex,
    ASIOBool directProcess)
{
    (void)directProcess;

    thread_local RealtimeThreadPolicy realtimePolicy(L"Pro Audio");
    (void)realtimePolicy;

    if (auto* self = active_.load(std::memory_order_acquire))
    {
        ++self->asioCallbackCount_;
        const long frames = self->asioBufferFrames_.load(std::memory_order_relaxed);

        // Publish an exact ASIO callback endpoint: sample count and host time
        // belong to the same callback. The sequence makes the pair readable
        // without a mutex from the DeckLink thread.
        self->asioTimingSequence_.fetch_add(1, std::memory_order_acq_rel); // odd
        std::uint64_t framesAfter =
            self->asioSampleFrameCount_.load(std::memory_order_relaxed);
        if (frames > 0)
        {
            framesAfter = self->asioSampleFrameCount_.fetch_add(
                static_cast<std::uint64_t>(frames),
                std::memory_order_relaxed) + static_cast<std::uint64_t>(frames);
        }
        const auto callbackNow = std::chrono::steady_clock::now();
        const auto callbackNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            callbackNow.time_since_epoch()).count();
        self->asioLastCallbackHostNs_.store(callbackNs, std::memory_order_relaxed);

        LARGE_INTEGER callbackQpc{};
        if (QueryPerformanceCounter(&callbackQpc))
            self->publishAsioTimingPoint(framesAfter, callbackQpc.QuadPart);

        self->asioTimingSequence_.fetch_add(1, std::memory_order_release); // even

        self->processBuffer(doubleBufferIndex);
    }

    return params;
}

void AsioCapture::sampleRateDidChangeStatic(ASIOSampleRate sampleRate)
{
    if (auto* self = active_.load(std::memory_order_acquire))
        self->handleSampleRateChange(sampleRate);
}

long AsioCapture::asioMessageStatic(long selector, long value, void* message, double* opt)
{
    (void)message;
    (void)opt;

    switch (selector)
    {
    case kAsioSelectorSupported:
        switch (value)
        {
        case kAsioEngineVersion:
        case kAsioSupportsTimeInfo:
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
            return 1;
        default:
            return 0;
        }
    case kAsioEngineVersion:
        return 2;
    case kAsioSupportsTimeInfo:
        return 1;
    case kAsioResetRequest:
        if (auto* self = active_.load(std::memory_order_acquire))
        {
            self->addDebugLogLine("ASIO MESSAGE reset request");
            self->setError(L"ASIO driver requested a reset.");
            self->restartRequested_.store(true, std::memory_order_release);
        }
        return 1;
    case kAsioBufferSizeChange:
        if (auto* self = active_.load(std::memory_order_acquire))
        {
            self->addDebugLogLine("ASIO MESSAGE buffer size change");
            self->setError(L"ASIO driver reported a buffer size change.");
            self->restartRequested_.store(true, std::memory_order_release);
        }
        return 1;
    case kAsioResyncRequest:
        if (auto* self = active_.load(std::memory_order_acquire))
            self->addDebugLogLine("ASIO MESSAGE resync request");
        return 1;
    case kAsioLatenciesChanged:
        if (auto* self = active_.load(std::memory_order_acquire))
            self->addDebugLogLine("ASIO MESSAGE latencies changed");
        return 1;
    default:
        return 0;
    }
}

void AsioCapture::handleSampleRateChange(double sampleRate)
{
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(3)
           << "ASIO SAMPLE RATE CHANGED " << sampleRate << " Hz";
        addDebugLogLine(os.str());
    }

    // 0.4.04 accepts only the fixed 48 kHz input basis.
    if (!std::isfinite(sampleRate) || std::abs(sampleRate - 48000.0) > 0.5)
    {
        setError(L"ASIO sample rate changed; 0.4.04 requires 48 kHz.");
        restartRequested_.store(true, std::memory_order_release);
        return;
    }

    inputRate_ = 48000.0;
    std::lock_guard lock(mutex_);
    asrc_.reset(48000.0);
    hamAsrc_.reset(48000.0);
    pcmQueuedFramesPublished_.store(asrc_.queuedOutputFrames(), std::memory_order_release);
    hamQueuedFramesPublished_.store(hamAsrc_.queuedOutputFrames(), std::memory_order_release);
}

float AsioCapture::sampleToFloat(const void* buffer, ASIOSampleType type, long frame)
{
    if (buffer == nullptr || frame < 0)
        return 0.0f;

    const auto clamp = [](double v)
    {
        return static_cast<float>(std::clamp(v, -1.0, 1.0));
    };

    const auto* p = static_cast<const std::uint8_t*>(buffer);

    switch (type)
    {
    case ASIOSTInt16LSB:
    {
        std::int16_t v = 0;
        std::memcpy(&v, p + static_cast<std::size_t>(frame) * 2, 2);
        return clamp(static_cast<double>(v) / 32768.0);
    }
    case ASIOSTInt24LSB:
        return clamp(static_cast<double>(readInt24Lsb(p + static_cast<std::size_t>(frame) * 3)) / 8388608.0);
    case ASIOSTInt32LSB:
    {
        std::int32_t v = 0;
        std::memcpy(&v, p + static_cast<std::size_t>(frame) * 4, 4);
        return clamp(static_cast<double>(v) / 2147483648.0);
    }
    case ASIOSTFloat32LSB:
    {
        float v = 0.0f;
        std::memcpy(&v, p + static_cast<std::size_t>(frame) * 4, 4);
        return clamp(v);
    }
    case ASIOSTFloat64LSB:
    {
        double v = 0.0;
        std::memcpy(&v, p + static_cast<std::size_t>(frame) * 8, 8);
        return clamp(v);
    }
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    {
        std::int32_t v = 0;
        std::memcpy(&v, p + static_cast<std::size_t>(frame) * 4, 4);
        const int bits = type == ASIOSTInt32LSB16 ? 16 :
            type == ASIOSTInt32LSB18 ? 18 :
            type == ASIOSTInt32LSB20 ? 20 : 24;
        return clamp(static_cast<double>(v) / static_cast<double>(std::uint64_t{1} << (bits - 1)));
    }
    case ASIOSTInt16MSB:
    {
        const auto* s = p + static_cast<std::size_t>(frame) * 2;
        const std::int16_t v = static_cast<std::int16_t>(
            (static_cast<std::uint16_t>(s[0]) << 8) | s[1]);
        return clamp(static_cast<double>(v) / 32768.0);
    }
    case ASIOSTInt24MSB:
        return clamp(static_cast<double>(readInt24Msb(p + static_cast<std::size_t>(frame) * 3)) / 8388608.0);
    case ASIOSTInt32MSB:
    {
        const std::uint32_t u = readU32Msb(p + static_cast<std::size_t>(frame) * 4);
        const std::int32_t v = static_cast<std::int32_t>(u);
        return clamp(static_cast<double>(v) / 2147483648.0);
    }
    case ASIOSTFloat32MSB:
    {
        const std::uint32_t u = readU32Msb(p + static_cast<std::size_t>(frame) * 4);
        float v = 0.0f;
        std::memcpy(&v, &u, 4);
        return clamp(v);
    }
    case ASIOSTFloat64MSB:
    {
        const std::uint64_t u = readU64Msb(p + static_cast<std::size_t>(frame) * 8);
        double v = 0.0;
        std::memcpy(&v, &u, 8);
        return clamp(v);
    }
    case ASIOSTInt32MSB16:
    case ASIOSTInt32MSB18:
    case ASIOSTInt32MSB20:
    case ASIOSTInt32MSB24:
    {
        const std::int32_t v = static_cast<std::int32_t>(
            readU32Msb(p + static_cast<std::size_t>(frame) * 4));
        const int bits = type == ASIOSTInt32MSB16 ? 16 :
            type == ASIOSTInt32MSB18 ? 18 :
            type == ASIOSTInt32MSB20 ? 20 : 24;
        return clamp(static_cast<double>(v) / static_cast<double>(std::uint64_t{1} << (bits - 1)));
    }
    default:
        return 0.0f;
    }
}

void AsioCapture::processBuffer(long doubleBufferIndex)
{
    if (!run_.load(std::memory_order_relaxed) ||
        (doubleBufferIndex != 0 && doubleBufferIndex != 1))
    {
        return;
    }

    const long frames = asioBufferFrames_.load(std::memory_order_relaxed);
    if (frames <= 0 || stereoScratch_.size() < static_cast<std::size_t>(frames) * 2)
        return;

    const void* leftBuffer = bufferInfos_[0].buffers[doubleBufferIndex];
    const void* rightBuffer = activeInputChannels_ >= 2
        ? bufferInfos_[1].buffers[doubleBufferIndex]
        : leftBuffer;

    const ASIOSampleType leftType = channelInfos_[0].type;
    const ASIOSampleType rightType = activeInputChannels_ >= 2
        ? channelInfos_[1].type
        : leftType;

    float peakL = 0.0f;
    float peakR = 0.0f;

    for (long i = 0; i < frames; ++i)
    {
        const float l = sampleToFloat(leftBuffer, leftType, i);
        const float r = sampleToFloat(rightBuffer, rightType, i);
        const std::size_t base = static_cast<std::size_t>(i) * 2;
        stereoScratch_[base] = l;
        stereoScratch_[base + 1] = r;
        peakL = std::max(peakL, std::abs(l));
        peakR = std::max(peakR, std::abs(r));
    }

    {
        std::lock_guard lock(mutex_);
        // Keep both format-specific ASRC paths warm so mode switching remains
        // possible at a PAL frame boundary without restarting the ASIO driver.
        asrc_.pushInterleavedFloat(stereoScratch_.data(), static_cast<std::size_t>(frames));
        hamAsrc_.pushInterleavedFloat(stereoScratch_.data(), static_cast<std::size_t>(frames));
        pcmQueuedFramesPublished_.store(asrc_.queuedOutputFrames(), std::memory_order_release);
        hamQueuedFramesPublished_.store(hamAsrc_.queuedOutputFrames(), std::memory_order_release);
    }

    // Publish after the samples are visible in the ASRC FIFOs. The DeckLink
    // worker waits on this instead of polling with sleep_for().
    audioDataSequence_.fetch_add(1, std::memory_order_release);
    audioDataCondition_.notify_all();

    float oldL = peakL_.load(std::memory_order_relaxed);
    while (peakL > oldL && !peakL_.compare_exchange_weak(oldL, peakL)) {}
    float oldR = peakR_.load(std::memory_order_relaxed);
    while (peakR > oldR && !peakR_.compare_exchange_weak(oldR, peakR)) {}
}
