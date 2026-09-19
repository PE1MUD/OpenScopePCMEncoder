#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <Unknwn.h>

#ifndef interface
#define interface struct
#endif

#include "AudioAsrc.h"
#include "HamAudioAsrc.h"
#include "BufferNudgeController.h"

#include <asio.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct AudioInputDevice
{
    std::wstring id;
    std::wstring name;
};

class AsioCapture
{
public:
    AsioCapture();
    ~AsioCapture();

    static std::vector<AudioInputDevice> enumerate();

    bool start(const std::wstring& deviceId);
    void stop();

    std::vector<std::int16_t> takeStereoFrames(std::size_t frames);
    std::vector<std::int16_t> takeHamStereoFrames(std::size_t frames);
    std::size_t drainStereoFrames(
        std::vector<std::int16_t>& destination,
        std::size_t maxFrames);
    std::size_t drainHamStereoFrames(
        std::vector<std::int16_t>& destination,
        std::size_t maxFrames);

    // Event-driven wake-up for the DeckLink PCM producer. The returned
    // sequence changes whenever a new ASIO block has been pushed through
    // the ASRCs. wakeAudioWaiters() is also used during DeckLink shutdown.
    std::uint64_t audioDataSequence() const
    {
        return audioDataSequence_.load(std::memory_order_acquire);
    }
    void waitForAudioData(std::uint64_t afterSequence);
    void wakeAudioWaiters();

    // 0.4.96: READY -> Blackmagic ScheduledFrameCompleted margin is again the control signal.
    // 0.4.49 also receives the exact QPC ticks for READY and the BMD callback.
    // Those events are projected onto the ASIO sample timeline using the ASIO
    // callbacks immediately before/after each event, removing whole-callback
    // quantisation from the buffer-margin measurement.
    void updateBufferNudge(
        double bmdBufferFillMs,
        double completionMarginMs,
        double sampleDtSec,
        double bmdCallbackElapsedMs,
        double bmdIdealElapsedMs,
        std::int64_t frameReadyQpcTicks,
        std::int64_t bmdCallbackQpcTicks);

    // 0.4.93: callback snapshot used while the ASRC consumer is intentionally
    // idle from frame-complete until ScheduledFrameCompleted().
    double bmdBufferFillMsSnapshot() const;

    void setHamMode(bool enabled) { hamMode_.store(enabled); }
    bool hamMode() const { return hamMode_.load(); }

    float consumePeakLeft() { return peakL_.exchange(0.0f); }
    float consumePeakRight() { return peakR_.exchange(0.0f); }

    std::size_t queuedFrames() const;
    void setBufferMs(int bufferMs);
    int bufferMs() const;
    double outputSampleRate() const;

    double txMarginMs() const { return txMarginMs_.load(); }
    double txAverageMarginMs() const { return txAverageMarginMs_.load(); }
    double txCurrentMarginMs() const { return txCurrentMarginMs_.load(); }
    double txCurrentAverageMarginMs() const { return txCurrentAverageMarginMs_.load(); }
    double txMicroAverageMarginMs() const { return txMicroAverageMarginMs_.load(); }
    double txContinuousBufferMs() const { return txContinuousBufferMs_.load(); }
    double txContinuousAverageBufferMs() const { return txContinuousAverageBufferMs_.load(); }
    double txSlowAverageMargin3sMs() const { return txSlowAverageMargin3sMs_.load(); }
    std::uint64_t txMarginSequence() const { return txMarginSequence_.load(); }
    std::uint64_t asioCallbackCount() const { return asioCallbackCount_.load(); }
    double asrcAppliedPpm() const { return asrcAppliedPpm_.load(); }
    double asrcNudgePpm() const { return asrcNudgePpm_.load(); }
    double asrcActualStep() const { return asrcActualStep_.load(); }
    bool nudgeActive() const { return nudgeActive_.load(); }
    bool maintenanceHoldActive() const { return maintenanceHoldActive_.load(); }
    bool maintenanceTriggerTiming() const { return maintenanceTriggerTiming_.load(); }

    bool readyForPalFrame() const;
    bool hasCompletePalFrame() const;
    void beginRebuffer();
    std::uint64_t underruns() const;
    bool rebuffering() const;

    long asioBufferFrames() const { return asioBufferFrames_.load(); }
    long asioBufferMinFrames() const { return asioBufferMinFrames_.load(); }
    long asioBufferMaxFrames() const { return asioBufferMaxFrames_.load(); }
    long asioBufferPreferredFrames() const { return asioBufferPreferredFrames_.load(); }
    long asioBufferGranularity() const { return asioBufferGranularity_.load(); }
    double asioBufferMs() const;

    std::vector<std::string> takeDebugLogLines();
    void addDebugLogLine(const std::string& text);
    bool consumeRestartRequested();
    bool running() const { return run_.load(std::memory_order_acquire); }
    std::wstring lastError() const;

private:
    static void bufferSwitchStatic(long doubleBufferIndex, ASIOBool directProcess);
    static ASIOTime* bufferSwitchTimeInfoStatic(
        ASIOTime* params,
        long doubleBufferIndex,
        ASIOBool directProcess);
    static void sampleRateDidChangeStatic(ASIOSampleRate sampleRate);
    static long asioMessageStatic(long selector, long value, void* message, double* opt);

    void processBuffer(long doubleBufferIndex);
    bool startAsioHostWindowThread();
    void stopAsioHostWindowThread();
    void asioHostWindowThreadMain();
    void handleSampleRateChange(double sampleRate);
    static float sampleToFloat(const void* buffer, ASIOSampleType type, long frame);
    void setError(const std::wstring& error);

    void appendDebugLogLocked(const std::string& text);
    static std::atomic<AsioCapture*> active_;

    mutable std::mutex mutex_;

    // Separate wait primitive: never hold the ASRC/data mutex while the
    // DeckLink worker sleeps waiting for more audio.
    mutable std::mutex audioDataWaitMutex_;
    std::condition_variable audioDataCondition_;
    std::atomic<std::uint64_t> audioDataSequence_{0};

    AudioAsrc asrc_;
    HamAudioAsrc hamAsrc_;
    BufferNudgeController bufferNudge_;
    std::wstring error_;

    std::array<ASIOBufferInfo, 2> bufferInfos_{};
    std::array<ASIOChannelInfo, 2> channelInfos_{};
    ASIOCallbacks callbacks_{};
    std::vector<float> stereoScratch_;
    long activeInputChannels_ = 0;
    bool driverLoaded_ = false;
    bool asioInitialized_ = false;
    bool buffersCreated_ = false;
    bool asioStarted_ = false;

    // Dedicated hidden ASIO host window. This deliberately has no lifetime
    // or visibility relationship with the Qt MainWindow, so minimizing the
    // UI cannot affect the ASIO driver through ASIODriverInfo::sysRef.
    HWND asioHostWindow_ = nullptr;

    std::atomic<bool> run_{false};
    std::atomic<bool> restartRequested_{false};
    std::atomic<float> peakL_{0.0f};
    std::atomic<float> peakR_{0.0f};
    std::atomic<double> inputRate_{48000.0};
    std::atomic<long> asioBufferFrames_{0};
    std::atomic<long> asioBufferMinFrames_{0};
    std::atomic<long> asioBufferMaxFrames_{0};
    std::atomic<long> asioBufferPreferredFrames_{0};
    std::atomic<long> asioBufferGranularity_{0};

    // 0.4.49: lock-free ASIO callback history used to project arbitrary QPC
    // event times onto a fractional ASIO sample position.  2048 entries is
    // intentionally generous so even pathological callback bursts retain
    // enough history to resolve the previous PAL frame without blocking the
    // DeckLink worker.
    static constexpr std::size_t AsioTimingHistorySize = 2048;
    struct AsioTimingHistorySlot
    {
        std::atomic<std::uint64_t> serial{0};
        std::atomic<std::uint64_t> frames{0};
        std::atomic<std::int64_t> qpcTicks{0};
    };
    std::array<AsioTimingHistorySlot, AsioTimingHistorySize> asioTimingHistory_{};
    std::atomic<std::uint64_t> asioTimingHistoryWrite_{0};
    std::atomic<std::int64_t> qpcFrequency_{0};
    std::atomic<std::uint64_t> asioLatestTimingFrames_{0};
    std::atomic<std::int64_t> asioLatestTimingQpcTicks_{0};

    void publishAsioTimingPoint(
        std::uint64_t frames,
        std::int64_t qpcTicks) noexcept;
    bool interpolateAsioPositionAtQpc(
        std::int64_t qpcTicks,
        double& samplePosition) const noexcept;

    struct PendingQpcMargin
    {
        bool valid = false;
        std::int64_t frameReadyQpcTicks = 0;
        std::int64_t bmdCallbackQpcTicks = 0;
        double rawQpcMarginMs = 0.0;
        double bmdIdealElapsedMs = 0.0;
    };
    PendingQpcMargin pendingQpcMargin_{};

    std::atomic_bool hamMode_{false};
    std::atomic<int> bufferMs_{4};
    std::atomic<double> txMarginMs_{0.0};
    std::atomic<double> txAverageMarginMs_{0.0};
    std::atomic<double> txCurrentMarginMs_{0.0};
    std::atomic<double> txCurrentAverageMarginMs_{0.0};
    std::atomic<double> txMicroAverageMarginMs_{0.0};
    std::atomic<double> txContinuousBufferMs_{0.0};
    std::atomic<std::uint64_t> txMarginSequence_{0};
    std::atomic<std::uint64_t> asioCallbackCount_{0};
    std::atomic<std::uint64_t> asioSampleFrameCount_{0};
    std::atomic<std::uint64_t> asioTimingSequence_{0};
    std::atomic<std::int64_t> asioLastCallbackHostNs_{0};

    // Only the hidden ASIO sysRef HWND is owned/pumped here. The ASIO
    // driver itself remains initialized and stopped on the caller thread,
    // matching the known-working pre-0.4.17 behavior.
    std::thread asioHostWindowThread_;
    std::mutex asioHostWindowMutex_;
    std::condition_variable asioHostWindowCv_;
    bool asioHostWindowReady_ = false;
    bool asioHostWindowOk_ = false;
    std::atomic<DWORD> asioHostWindowThreadId_{0};
    std::atomic<double> asrcAppliedPpm_{0.0};
    std::atomic<double> asrcNudgePpm_{0.0};
    std::atomic<double> asrcActualStep_{0.0};

    // Fast controller-average mirror used by the debug graph.
    std::atomic<double> txContinuousAverageBufferMs_{0.0};

    // Exact time-weighted reported completion-margin average over 3 seconds.
    std::deque<std::pair<double, double>> slowMarginAverage3sWindow_; // {dtSec, marginMs}
    double slowMarginAverage3sWindowSec_ = 0.0;
    double slowMarginAverage3sWeightedSum_ = 0.0;
    double slowAverageMargin3sMs_ = 0.0;
    bool slowAverageMargin3sValid_ = false;
    std::atomic<double> txSlowAverageMargin3sMs_{0.0};

    // Exact queue-depth publication for lock-free sampling in the DeckLink callback.
    std::atomic<std::size_t> pcmQueuedFramesPublished_{0};
    std::atomic<std::size_t> hamQueuedFramesPublished_{0};
    std::atomic<bool> nudgeActive_{false};
    std::atomic<bool> maintenanceHoldActive_{false};
    std::atomic<bool> maintenanceTriggerTiming_{false};

    std::deque<std::string> debugLogLines_;
    bool debugStateValid_ = false;
    bool debugPrevNudgeActive_ = false;
    double debugPrevTargetMs_ = 0.0;
    std::int64_t debugLastSnapshotMs_ = 0;


};
