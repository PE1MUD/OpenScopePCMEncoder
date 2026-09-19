#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class HamPcmV2Encoder final
{
public:
    static constexpr int Width = 720;
    static constexpr int Height = 576;
    static constexpr int SamplesPerFrame = 1920; // stereo pairs at 48 kHz / 25 fps

    void reset();
    void encodeFrame(const std::vector<std::int16_t>& stereo, std::vector<std::uint8_t>& uyvy);

    void setPulseShapingEnabled(bool enabled) { pulseShapingEnabled_.store(enabled); }
    void setVideoBandwidthMHz(double mhz) { videoBandwidthMHz_.store(mhz); }
    void setHorizontalOffsetPixels(int pixels) { horizontalOffsetPixels_.store(pixels); }
    void setText(const std::string& text);

    bool pulseShapingEnabled() const { return pulseShapingEnabled_.load(); }
    double videoBandwidthMHz() const { return videoBandwidthMHz_.load(); }
    int horizontalOffsetPixels() const { return horizontalOffsetPixels_.load(); }
    std::string text() const;
    std::uint64_t frameCount() const { return frame_; }

private:
    static std::uint8_t gfMul(std::uint8_t a, std::uint8_t b);
    static std::array<std::uint8_t, 4> rsParity(const std::array<std::uint8_t, 9>& message);
    static std::uint8_t crc8(const std::uint8_t* data, std::size_t size);
    static int textIndex(char ch);

    std::array<std::uint8_t, 20> makePayload(
        const std::int16_t* stereoPairs,
        int n,
        bool marker,
        std::uint8_t textBits) const;

    void renderLine(
        std::uint8_t* row,
        int frameRow,
        const std::array<std::uint8_t, 20>& payload) const;

    void renderCaptionBand(std::vector<std::uint8_t>& out, const std::string& text) const;
    static void fillBlackLine(std::uint8_t* row);
    static void shapeLuma(std::vector<float>& y, double bandwidthMHz);

    std::atomic_bool pulseShapingEnabled_{ false };
    std::atomic<double> videoBandwidthMHz_{ 3.5 };
    std::atomic<int> horizontalOffsetPixels_{ 5 };

    mutable std::mutex textMutex_;
    std::string text_{ "OPEN SCOPE" };
    int textRow_ = 0;
    std::uint64_t frame_ = 0;
};
