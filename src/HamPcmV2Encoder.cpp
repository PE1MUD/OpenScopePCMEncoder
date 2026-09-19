#include "HamPcmV2Encoder.h"

#include <QFont>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
constexpr std::array<std::uint8_t, 14> kKey{
    0x87, 0x23, 0x46, 0xDC, 0xB0, 0xDD, 0xEE,
    0xF8, 0xFD, 0xC3, 0x5C, 0xBF, 0x5C, 0x53};

constexpr int kTextFirstRow = 260;
constexpr int kTextLastRow = 291;
constexpr int kFieldRows = 288;
constexpr int kAudioRowsPerField = 271;
constexpr int kPairsPerField = 960;
constexpr int kFourPairRowsPerField = kPairsPerField - kAudioRowsPerField * 3; // 147
static_assert(kFourPairRowsPerField == 147);

// Formal Ham PCM specification 2.0 geometry.
constexpr int kGuardPixels = 12;
constexpr int kDataPixels = 696;
constexpr int kBitsPerRow = 180;

int bitBoundary(int bit)
{
    // Spec 2.0: bit k starts at pixel 12 + floor((696*k + 90) / 180).
    return kGuardPixels + (kDataPixels * bit + 90) / kBitsPerRow;
}
}

void HamPcmV2Encoder::reset()
{
    textRow_ = 0;
    frame_ = 0;
}

void HamPcmV2Encoder::setText(const std::string& value)
{
    std::lock_guard lock(textMutex_);
    text_ = value.substr(0, 10);
}

std::string HamPcmV2Encoder::text() const
{
    std::lock_guard lock(textMutex_);
    return text_;
}

std::uint8_t HamPcmV2Encoder::gfMul(std::uint8_t a, std::uint8_t b)
{
    std::uint16_t aa = a;
    std::uint16_t bb = b;
    std::uint16_t r = 0;
    while (bb != 0)
    {
        if ((bb & 1u) != 0u)
            r ^= aa;
        bb >>= 1u;
        aa <<= 1u;
        if ((aa & 0x100u) != 0u)
            aa ^= 0x11Du;
    }
    return static_cast<std::uint8_t>(r);
}

std::array<std::uint8_t, 4> HamPcmV2Encoder::rsParity(
    const std::array<std::uint8_t, 9>& message)
{
    constexpr std::array<std::uint8_t, 5> g{1, 15, 54, 120, 64};
    std::array<std::uint8_t, 4> rem{};
    for (const auto d : message)
    {
        const auto fb = static_cast<std::uint8_t>(d ^ rem[0]);
        rem = {rem[1], rem[2], rem[3], 0};
        if (fb != 0)
        {
            for (int j = 0; j < 4; ++j)
                rem[static_cast<std::size_t>(j)] ^=
                    gfMul(g[static_cast<std::size_t>(j + 1)], fb);
        }
    }
    return rem;
}

std::uint8_t HamPcmV2Encoder::crc8(const std::uint8_t* data, std::size_t size)
{
    std::uint8_t c = 0;
    for (std::size_t i = 0; i < size; ++i)
    {
        c ^= data[i];
        for (int b = 0; b < 8; ++b)
        {
            c = (c & 0x80u) != 0u
                ? static_cast<std::uint8_t>((static_cast<unsigned int>(c) << 1u) ^ 0x07u)
                : static_cast<std::uint8_t>(static_cast<unsigned int>(c) << 1u);
        }
    }
    return c;
}

int HamPcmV2Encoder::textIndex(char ch)
{
    static constexpr char alphabet[] =
        " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,-/:;!?'()+=&@#*%<>\"_[]{}|";
    const char upper =
        (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
    for (int i = 0; alphabet[i] != '\0'; ++i)
    {
        if (alphabet[i] == upper)
            return i;
    }
    return 0;
}

std::array<std::uint8_t, 20> HamPcmV2Encoder::makePayload(
    const std::int16_t* stereoPairs,
    int n,
    bool marker,
    std::uint8_t textBits) const
{
    std::array<std::uint8_t, 20> out{};
    out[0] = static_cast<std::uint8_t>(
        0xC0u | (marker ? 0x20u : 0u) | ((textBits & 0x03u) << 3u) | (n & 0x07));

    std::array<std::uint8_t, 8> refinement{};
    for (int pair = 0; pair < 4; ++pair)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const int slot = pair * 2 + ch;
            std::int16_t src = 0;
            if (pair < n && stereoPairs != nullptr)
                src = stereoPairs[pair * 2 + ch];

            const std::uint16_t s14 = static_cast<std::uint16_t>(
                (static_cast<std::int32_t>(src) >> 2) & 0x3FFF);
            out[1 + slot] = static_cast<std::uint8_t>(
                ((s14 >> 6u) & 0xFFu) ^ kKey[static_cast<std::size_t>(slot)]);
            refinement[static_cast<std::size_t>(slot)] =
                static_cast<std::uint8_t>(s14 & 0x3Fu);
        }
    }

    std::uint64_t packed = 0;
    for (int i = 0; i < 8; ++i)
        packed = (packed << 6u) | refinement[static_cast<std::size_t>(i)];

    std::array<std::uint8_t, 6> e{};
    for (int j = 0; j < 6; ++j)
    {
        e[static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(
            ((packed >> (40 - 8 * j)) & 0xFFu) ^ kKey[static_cast<std::size_t>(8 + j)]);
    }

    std::array<std::uint8_t, 9> message{};
    std::copy_n(out.begin(), 9, message.begin());
    const auto parity = rsParity(message);
    for (int i = 0; i < 4; ++i)
        out[9 + i] = parity[static_cast<std::size_t>(i)];
    for (int i = 0; i < 6; ++i)
        out[13 + i] = e[static_cast<std::size_t>(i)];
    out[19] = crc8(e.data(), e.size());
    return out;
}

void HamPcmV2Encoder::shapeLuma(std::vector<float>& y, double bandwidthMHz)
{
    if (y.empty())
        return;
    constexpr double sampleRateMHz = 13.5;
    constexpr double pi = 3.14159265358979323846;
    const double fc = std::clamp(bandwidthMHz, 1.0, 6.0);
    const double a = std::exp(-2.0 * pi * fc / sampleRateMHz);
    const auto pass = [&](bool reverse)
    {
        if (!reverse)
        {
            double state = y.front();
            for (std::size_t i = 1; i < y.size(); ++i)
            {
                state = (1.0 - a) * y[i] + a * state;
                y[i] = static_cast<float>(state);
            }
        }
        else
        {
            double state = y.back();
            for (std::size_t i = y.size() - 1; i-- > 0; )
            {
                state = (1.0 - a) * y[i] + a * state;
                y[i] = static_cast<float>(state);
            }
        }
    };
    pass(false); pass(true); pass(false); pass(true);
}

void HamPcmV2Encoder::fillBlackLine(std::uint8_t* row)
{
    for (int x = 0; x < Width; x += 2)
    {
        row[2 * x] = 128; row[2 * x + 1] = 16;
        row[2 * x + 2] = 128; row[2 * x + 3] = 16;
    }
}

void HamPcmV2Encoder::renderLine(
    std::uint8_t* row,
    int frameRow,
    const std::array<std::uint8_t, 20>& payload) const
{
    std::array<int, kBitsPerRow> bits{};
    const bool invert = ((frameRow >> 1) & 1) != 0;
    for (int i = 0; i < 8; ++i)
        bits[static_cast<std::size_t>(i)] = (((i & 1) == 0) ^ invert) ? 1 : 0;
    for (int i = 8; i < 12; ++i)
        bits[static_cast<std::size_t>(i)] = 0;
    constexpr std::uint8_t sync = 0x2E;
    for (int i = 0; i < 8; ++i)
        bits[static_cast<std::size_t>(12 + i)] = (sync >> (7 - i)) & 1u;
    int k = 20;
    for (const auto byte : payload)
    {
        for (int bit = 7; bit >= 0; --bit)
            bits[static_cast<std::size_t>(k++)] = (byte >> bit) & 1u;
    }

    constexpr int outerGuard = 64;
    constexpr float black = 16.0f;
    constexpr float white = 235.0f;
    std::vector<float> extended(static_cast<std::size_t>(Width + 2 * outerGuard), black);
    const int offset = std::clamp(horizontalOffsetPixels_.load(), -24, 24);

    for (int bit = 0; bit < kBitsPerRow; ++bit)
    {
        const int x0 = bitBoundary(bit);
        const int x1 = bitBoundary(bit + 1);
        for (int x = x0; x < x1; ++x)
        {
            const int dst = outerGuard + x + offset;
            if (dst >= 0 && dst < static_cast<int>(extended.size()))
                extended[static_cast<std::size_t>(dst)] =
                    bits[static_cast<std::size_t>(bit)] != 0 ? white : black;
        }
    }

    if (pulseShapingEnabled_.load())
        shapeLuma(extended, videoBandwidthMHz_.load());

    for (int x = 0; x < Width; x += 2)
    {
        const auto sample = [&](int px)
        {
            return static_cast<std::uint8_t>(std::clamp(
                std::lround(extended[static_cast<std::size_t>(outerGuard + px)]), 0L, 255L));
        };
        row[2 * x] = 128; row[2 * x + 1] = sample(x);
        row[2 * x + 2] = 128; row[2 * x + 3] = sample(x + 1);
    }
}

void HamPcmV2Encoder::renderCaptionBand(
    std::vector<std::uint8_t>& out,
    const std::string& caption) const
{
    QImage image(Width, 32, QImage::Format_Grayscale8);
    image.fill(16);
    QPainter painter(&image);
    painter.setPen(QColor(235, 235, 235));
    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(22);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRect(0, 0, Width, 32), Qt::AlignCenter, QString::fromStdString(caption));
    painter.end();

    for (int y = 0; y < 32; ++y)
    {
        auto* row = out.data() + (kTextFirstRow + y) * Width * 2;
        const auto* src = image.constScanLine(y);
        for (int x = 0; x < Width; x += 2)
        {
            row[2 * x] = 128; row[2 * x + 1] = src[x];
            row[2 * x + 2] = 128; row[2 * x + 3] = src[x + 1];
        }
    }
}

void HamPcmV2Encoder::encodeFrame(
    const std::vector<std::int16_t>& stereo,
    std::vector<std::uint8_t>& out)
{
    out.assign(Width * Height * 2, 0);
    for (int row = 0; row < Height; ++row)
        fillBlackLine(out.data() + row * Width * 2);

    std::string caption;
    {
        std::lock_guard lock(textMutex_);
        caption = text_;
    }
    caption.resize(10, ' ');
    renderCaptionBand(out, caption);

    std::uint64_t text60 = 0;
    for (const char ch : caption)
        text60 = (text60 << 6u) | static_cast<std::uint64_t>(textIndex(ch) & 0x3F);

    int samplePair = 0;
    const auto emitRow = [&](int frameRow, int n)
    {
        const int cyclePos = textRow_ % 30;
        const bool marker = cyclePos == 0;
        const int shift = 58 - 2 * cyclePos;
        const auto textBits = static_cast<std::uint8_t>((text60 >> shift) & 0x03u);
        const std::int16_t* source =
            (n > 0 && samplePair * 2 < static_cast<int>(stereo.size()))
                ? stereo.data() + samplePair * 2
                : nullptr;
        const auto payload = makePayload(source, n, marker, textBits);
        renderLine(out.data() + frameRow * Width * 2, frameRow, payload);
        samplePair += n;
        ++textRow_;
    };

    // OpenScope/Intensity Pro 4K measured mapping override:
    // TX row N == RX row N. Therefore the two n=0 rows described in the
    // reference generator are represented here by black final field rows
    // 574/575, preserving 271 audio rows per field and all 960 pairs/field.
    for (int field = 0; field < 2; ++field)
    {
        int accumulator = 0;
        int fieldPairs = 0;
        int fieldAudioRows = 0;

        for (int fieldRow = 0; fieldRow < kFieldRows; ++fieldRow)
        {
            const int frameRow = fieldRow * 2 + field;
            if (frameRow >= kTextFirstRow && frameRow <= kTextLastRow)
                continue;
            if (fieldRow == kFieldRows - 1)
                continue; // rows 574/575 black

            int n = 3;
            accumulator += kFourPairRowsPerField;
            if (accumulator >= kAudioRowsPerField)
            {
                ++n;
                accumulator -= kAudioRowsPerField;
            }
            n = std::min(n, kPairsPerField - fieldPairs);
            emitRow(frameRow, n);
            fieldPairs += n;
            ++fieldAudioRows;
        }

        // Constants above must deliver exactly 271 rows and 960 pairs.
        if (fieldAudioRows != kAudioRowsPerField || fieldPairs != kPairsPerField)
        {
            // Keep the raster deterministic; this branch is defensive only.
        }
    }

    ++frame_;
}
