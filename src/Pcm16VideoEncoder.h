#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

class Pcm16VideoEncoder {
public:
    static constexpr int Width=720, Height=576, SamplesPerFrame=1764;

    void reset();
    void encodeFrame(const std::vector<std::int16_t>& stereo, std::vector<std::uint8_t>& uyvy);

    void setPulseShapingEnabled(bool enabled) { pulseShapingEnabled_.store(enabled); }
    void setVideoBandwidthMHz(double mhz) { videoBandwidthMHz_.store(mhz); }
    void setHorizontalOffsetPixels(int pixels) { horizontalOffsetPixels_.store(pixels); }
    void set16BitMode(bool enabled) { mode16Bit_.store(enabled); }
    void setPreEmphasisEnabled(bool enabled) { preEmphasisEnabled_.store(enabled); }

    bool pulseShapingEnabled() const { return pulseShapingEnabled_.load(); }
    double videoBandwidthMHz() const { return videoBandwidthMHz_.load(); }
    int horizontalOffsetPixels() const { return horizontalOffsetPixels_.load(); }
    bool is16BitMode() const { return mode16Bit_.load(); }
    bool preEmphasisEnabled() const { return preEmphasisEnabled_.load(); }

    std::uint64_t frameCount() const { return frame_; }

private:
    using Group=std::array<std::uint16_t,7>; // six 16-bit samples + 16-bit P

    Group makeGroup(const std::int16_t* s) const;
    Group groupAt(std::int64_t g) const;

    static std::uint16_t q14(const Group& g);
    static std::uint16_t gfMulX14(std::uint16_t v);

    std::array<std::uint16_t,8> physicalWords16(std::int64_t g) const;
    std::array<std::uint16_t,8> physicalWords14(std::int64_t g) const;
    std::array<std::uint16_t,8> controlWords(bool mode16, bool preEmphasis) const;

    static std::uint16_t crc(const std::array<std::uint16_t,8>& w);

    void renderLine(std::uint8_t* uyvyRow, const std::array<std::uint16_t,8>& w) const;
    static void fillBlackLine(std::uint8_t* row);

    static void shapeLuma(std::vector<float>& y, double bandwidthMHz);
    void applyPreEmphasis(std::vector<std::int16_t>& stereo, bool enabled);

    std::atomic_bool pulseShapingEnabled_{ false };
    std::atomic<double> videoBandwidthMHz_{ 3.5 };
    std::atomic<int> horizontalOffsetPixels_{ 5 };
    std::atomic_bool mode16Bit_{ true };
    std::atomic_bool preEmphasisEnabled_{ false };

    // 50/15 us audio pre-emphasis IIR state, owned by the DeckLink encode thread.
    bool preEmphasisStateEnabled_ = false;
    std::array<double,2> preX1_{};
    std::array<double,2> preY1_{};

    std::vector<Group> history_;
    std::int64_t firstGroup_=0;
    std::uint64_t frame_=0;
};
