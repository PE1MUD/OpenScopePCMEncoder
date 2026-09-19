#include "HamAudioAsrc.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double Pi = 3.1415926535897932384626433832795;

double sinc(double x)
{
    if (std::abs(x) < 1.0e-12)
        return 1.0;

    const double pix = Pi * x;
    return std::sin(pix) / pix;
}
}

void HamAudioAsrc::reset(double /*inputRate*/)
{
    fifo_.clear();
    outputFifo_.clear();
    sourcePos_ = HalfTaps;
    underruns_ = 0;
    diagnosticOutputFramesProduced_ = 0;
    diagnosticInputFramesAdvanced_ = 0.0;
    recovering_ = false;
    for (int i = 0; i < HalfTaps; ++i)
    {
        fifo_.push_back(0.0f);
        fifo_.push_back(0.0f);
    }
}

void HamAudioAsrc::pushInterleavedFloat(const float* stereo, std::size_t frames)
{
    if (stereo == nullptr || frames == 0)
        return;

    for (std::size_t i = 0; i < frames * 2; ++i)
        fifo_.push_back(stereo[i]);

    const std::size_t maxFrames = static_cast<std::size_t>(InputRate * 0.50);
    while (fifo_.size() / 2 > maxFrames)
    {
        fifo_.pop_front();
        fifo_.pop_front();
        sourcePos_ = std::max(0.0, sourcePos_ - 1.0);
    }

    produceAvailable();
}

float HamAudioAsrc::interpolate(int channel, double position) const
{
    const auto frameCount = static_cast<std::ptrdiff_t>(fifo_.size() / 2);
    const auto center = static_cast<std::ptrdiff_t>(std::floor(position));
    const double frac = position - static_cast<double>(center);

    double sum = 0.0;
    double norm = 0.0;

    for (int tap = -HalfTaps + 1; tap <= HalfTaps; ++tap)
    {
        const std::ptrdiff_t index = center + tap;
        if (index < 0 || index >= frameCount)
            continue;

        const double x = static_cast<double>(tap) - frac;
        const double ax = std::abs(x);
        if (ax > HalfTaps)
            continue;

        const double window = 0.5 + 0.5 * std::cos(Pi * x / HalfTaps);
        const double h = sinc(x) * window;
        sum += static_cast<double>(fifo_[static_cast<std::size_t>(index) * 2 + channel]) * h;
        norm += h;
    }

    if (std::abs(norm) > 1.0e-12)
        sum /= norm;

    return static_cast<float>(std::clamp(sum, -1.0, 0.999969482421875));
}

void HamAudioAsrc::trimConsumed()
{
    const auto removable =
        static_cast<std::ptrdiff_t>(std::floor(sourcePos_)) - HalfTaps;
    if (removable <= 0)
        return;

    const auto framesToRemove =
        std::min<std::size_t>(static_cast<std::size_t>(removable), fifo_.size() / 2);

    for (std::size_t i = 0; i < framesToRemove; ++i)
    {
        fifo_.pop_front();
        fifo_.pop_front();
    }
    sourcePos_ -= static_cast<double>(framesToRemove);
}

void HamAudioAsrc::produceAvailable()
{
    const double step =
        (InputRate / OutputRate) * (1.0 + appliedPpm() * 1.0e-6);

    const auto toInt16 = [](float x)
    {
        const double scaled = static_cast<double>(x) * 32768.0;
        return static_cast<std::int16_t>(
            std::clamp(
                std::llround(scaled),
                static_cast<long long>(std::numeric_limits<std::int16_t>::min()),
                static_cast<long long>(std::numeric_limits<std::int16_t>::max())));
    };

    const std::size_t maxOutputFrames =
        static_cast<std::size_t>(OutputRate * 0.25);

    while (outputFifo_.size() / 2 < maxOutputFrames)
    {
        const auto availableFrames = fifo_.size() / 2;
        const double latestNeeded = sourcePos_ + HalfTaps + 1;
        if (latestNeeded >= static_cast<double>(availableFrames))
            break;

        outputFifo_.push_back(toInt16(interpolate(0, sourcePos_)));
        outputFifo_.push_back(toInt16(interpolate(1, sourcePos_)));

        sourcePos_ += step;
        ++diagnosticOutputFramesProduced_;
        diagnosticInputFramesAdvanced_ += step;
        trimConsumed();
    }
}

std::size_t HamAudioAsrc::drainAvailableInt16(
    std::vector<std::int16_t>& destination,
    std::size_t maxFrames)
{
    if (maxFrames == 0)
        return 0;

    const std::size_t availableFrames = outputFifo_.size() / 2;
    const std::size_t takeFrames = std::min(maxFrames, availableFrames);
    if (takeFrames == 0)
        return 0;

    const std::size_t oldSize = destination.size();
    destination.resize(oldSize + takeFrames * 2);

    for (std::size_t i = 0; i < takeFrames * 2; ++i)
    {
        destination[oldSize + i] = outputFifo_.front();
        outputFifo_.pop_front();
    }

    return takeFrames;
}

std::vector<std::int16_t> HamAudioAsrc::takeInt16(std::size_t requestedFrames)
{
    std::vector<std::int16_t> out;
    out.reserve(requestedFrames * 2);

    const std::size_t taken = drainAvailableInt16(out, requestedFrames);
    if (taken < requestedFrames)
    {
        out.resize(requestedFrames * 2, 0);
        beginRebuffer();
    }

    return out;
}
