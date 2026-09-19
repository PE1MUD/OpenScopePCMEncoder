#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class HamAudioAsrc
{
public:
    static constexpr double InputRate = 48000.0;
    static constexpr double OutputRate = 48000.0;

    void reset(double inputRate = InputRate);
    void pushInterleavedFloat(const float* stereo, std::size_t frames);

    std::size_t drainAvailableInt16(
        std::vector<std::int16_t>& destination,
        std::size_t maxFrames);
    std::vector<std::int16_t> takeInt16(std::size_t requestedFrames);

    std::size_t queuedOutputFrames() const { return outputFifo_.size() / 2; }
    double inputRate() const { return InputRate; }
    double outputRate() const { return OutputRate; }
    void setClockCorrectionPpm(double ppm) { clockCorrectionPpm_ = ppm; }
    double clockCorrectionPpm() const { return clockCorrectionPpm_; }
    double appliedPpm() const { return clockCorrectionPpm_; }

    void beginRebuffer(bool countRealStarvation = true)
    {
        if (countRealStarvation && !recovering_)
            ++underruns_;

        recovering_ = true;
    }

    std::uint64_t underruns() const { return underruns_; }
    bool rebuffering() const { return recovering_; }

private:
    static constexpr int HalfTaps = 16;

    float interpolate(int channel, double position) const;
    void trimConsumed();
    void produceAvailable();

    std::deque<float> fifo_;
    std::deque<std::int16_t> outputFifo_;
    double sourcePos_ = HalfTaps;
    std::uint64_t underruns_ = 0;
    bool recovering_ = false;

    // Diagnostic counters used by the current .cpp implementation.
    // They are reset in reset() and advanced while producing output.
    std::uint64_t diagnosticOutputFramesProduced_ = 0;
    std::uint64_t diagnosticInputFramesAdvanced_ = 0;

    double clockCorrectionPpm_ = 0.0;
};
