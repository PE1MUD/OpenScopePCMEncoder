#include "AudioAsrc.h"

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

float clampToFloatSample(double x)
{
    return static_cast<float>(std::clamp(x, -1.0, 0.999969482421875));
}
}

void AudioAsrc::rebuildKernelTable()
{
    kernels_.resize(PhaseCount);

    constexpr double outputNyquist = 0.5 * OutputRate;
    const double passbandHz = std::min(20000.0, outputNyquist * 0.95);
    const double cutoffHz = 0.5 * (passbandHz + outputNyquist);
    const double fc = cutoffHz / InputRate;

    for (int phase = 0; phase < PhaseCount; ++phase)
    {
        const double frac = static_cast<double>(phase) / PhaseCount;
        double norm = 0.0;

        for (int i = 0; i < TapCount; ++i)
        {
            const int tap = i - HalfTaps + 1;
            const double x = static_cast<double>(tap) - frac;
            const double ideal = 2.0 * fc * sinc(2.0 * fc * x);

            const double windowPosition =
                (x + static_cast<double>(HalfTaps)) /
                (2.0 * static_cast<double>(HalfTaps));
            const double window =
                (windowPosition >= 0.0 && windowPosition <= 1.0)
                    ? 0.42
                        - 0.50 * std::cos(2.0 * Pi * windowPosition)
                        + 0.08 * std::cos(4.0 * Pi * windowPosition)
                    : 0.0;

            const double h = ideal * window;
            kernels_[phase][i] = static_cast<float>(h);
            norm += h;
        }

        if (std::abs(norm) > 1.0e-15)
        {
            const float invNorm = static_cast<float>(1.0 / norm);
            for (float& h : kernels_[phase])
                h *= invNorm;
        }
    }
}

void AudioAsrc::reset(double /*inputRate*/)
{
    fifo_.clear();
    outputFifo_.clear();
    sourcePos_ = HalfTaps;
    underruns_ = 0;
    diagnosticOutputFramesProduced_ = 0;
    diagnosticInputFramesAdvanced_ = 0.0;
    recovering_ = false;
    rebuildKernelTable();

    for (int i = 0; i < HalfTaps; ++i)
    {
        fifo_.push_back(0.0f);
        fifo_.push_back(0.0f);
    }
}

void AudioAsrc::pushInterleavedFloat(const float* stereo, std::size_t frames)
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

AudioAsrc::StereoSample AudioAsrc::interpolateStereo(double position) const
{
    const auto frameCount = static_cast<std::ptrdiff_t>(fifo_.size() / 2);
    auto center = static_cast<std::ptrdiff_t>(std::floor(position));
    const double frac = position - static_cast<double>(center);

    int phase = static_cast<int>(std::llround(frac * PhaseCount));
    if (phase >= PhaseCount)
    {
        phase = 0;
        ++center;
    }

    const auto& kernel = kernels_[phase];
    double left = 0.0;
    double right = 0.0;

    for (int i = 0; i < TapCount; ++i)
    {
        const std::ptrdiff_t index = center + (i - HalfTaps + 1);
        if (index < 0 || index >= frameCount)
            continue;

        const double h = kernel[i];
        const std::size_t base = static_cast<std::size_t>(index) * 2;
        left += static_cast<double>(fifo_[base]) * h;
        right += static_cast<double>(fifo_[base + 1]) * h;
    }

    return { clampToFloatSample(left), clampToFloatSample(right) };
}

void AudioAsrc::trimConsumed()
{
    const auto removable =
        static_cast<std::ptrdiff_t>(std::floor(sourcePos_)) - HalfTaps;

    if (removable <= 0)
        return;

    const auto framesToRemove =
        std::min<std::size_t>(
            static_cast<std::size_t>(removable),
            fifo_.size() / 2);

    for (std::size_t i = 0; i < framesToRemove; ++i)
    {
        fifo_.pop_front();
        fifo_.pop_front();
    }

    sourcePos_ -= static_cast<double>(framesToRemove);
}

void AudioAsrc::produceAvailable()
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

        const StereoSample sample = interpolateStereo(sourcePos_);
        outputFifo_.push_back(toInt16(sample.l));
        outputFifo_.push_back(toInt16(sample.r));

        sourcePos_ += step;
        ++diagnosticOutputFramesProduced_;
        diagnosticInputFramesAdvanced_ += step;
        trimConsumed();
    }
}

std::size_t AudioAsrc::drainAvailableInt16(
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

std::vector<std::int16_t> AudioAsrc::takeInt16(std::size_t requestedFrames)
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
