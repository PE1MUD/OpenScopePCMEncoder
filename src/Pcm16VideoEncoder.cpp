#include "Pcm16VideoEncoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

void Pcm16VideoEncoder::reset()
{
    history_.clear();

    Group silence{};
    for (int i = 0; i < 112; ++i)
        history_.push_back(silence);

    firstGroup_ = -112;
    frame_ = 0;
    preEmphasisStateEnabled_ = false;
    preX1_ = {};
    preY1_ = {};
}

Pcm16VideoEncoder::Group Pcm16VideoEncoder::makeGroup(const std::int16_t* s) const
{
    Group g{};
    for (int i = 0; i < 6; ++i)
        g[i] = static_cast<std::uint16_t>(s[i]);
    g[6] = static_cast<std::uint16_t>(g[0]^g[1]^g[2]^g[3]^g[4]^g[5]);
    return g;
}

Pcm16VideoEncoder::Group Pcm16VideoEncoder::groupAt(std::int64_t g) const
{
    if (g < firstGroup_ || g >= firstGroup_ + static_cast<std::int64_t>(history_.size()))
        return {};
    return history_[static_cast<std::size_t>(g-firstGroup_)];
}

// GF(2) companion operation for the EIAJ 14-bit Q code.
// Generator/primitive polynomial: x^14 + x^8 + 1.
std::uint16_t Pcm16VideoEncoder::gfMulX14(std::uint16_t v)
{
    v &= 0x3FFFu;
    const bool carry = (v & 0x2000u) != 0;
    v = static_cast<std::uint16_t>((v << 1) & 0x3FFFu);
    if (carry)
        v ^= 0x0101u; // x^8 + 1
    return v;
}

std::uint16_t Pcm16VideoEncoder::q14(const Group& g)
{
    // EIAJ: Q = T^6 W1 + T^5 W2 + ... + T W6 (mod 2).
    std::uint16_t q = 0;
    for (int i = 0; i < 6; ++i)
    {
        std::uint16_t v = static_cast<std::uint16_t>((g[i] >> 2) & 0x3FFFu);
        for (int power = 6 - i; power > 0; --power)
            v = gfMulX14(v);
        q ^= v;
    }
    return static_cast<std::uint16_t>(q & 0x3FFFu);
}

std::array<std::uint16_t,8> Pcm16VideoEncoder::physicalWords16(std::int64_t g) const
{
    const auto g0=groupAt(g),g1=groupAt(g-16),g2=groupAt(g-32),g3=groupAt(g-48),
               g4=groupAt(g-64),g5=groupAt(g-80),gp=groupAt(g-96),gq=groupAt(g-112);

    // PCM-F1 16-bit S word: the former Q word carries the two low bits
    // of the six interleaved samples, followed by the two low bits of P.
    // Real PCM-701ES captures decode with adjacent 2-bit pairs:
    //
    //   S[13:12] = W0[1:0]
    //   S[11:10] = W1[1:0]
    //   S[ 9: 8] = W2[1:0]
    //   S[ 7: 6] = W3[1:0]
    //   S[ 5: 4] = W4[1:0]
    //   S[ 3: 2] = W5[1:0]
    //   S[ 1: 0] = P [1:0]
    //
    // Each value belongs to the same logical 16H-spaced interleave group
    // as its transmitted upper-14-bit word.
    const std::array<std::uint16_t,7> ext = {
        g0[0], g1[1], g2[2], g3[3], g4[4], g5[5], gp[6]
    };

    std::uint16_t lsbPack = 0;
    for (const auto v : ext)
        lsbPack = static_cast<std::uint16_t>((lsbPack << 2) | (v & 0x3u));

    return {
        static_cast<std::uint16_t>(g0[0]>>2),
        static_cast<std::uint16_t>(g1[1]>>2),
        static_cast<std::uint16_t>(g2[2]>>2),
        static_cast<std::uint16_t>(g3[3]>>2),
        static_cast<std::uint16_t>(g4[4]>>2),
        static_cast<std::uint16_t>(g5[5]>>2),
        static_cast<std::uint16_t>(gp[6]>>2),
        static_cast<std::uint16_t>(lsbPack & 0x3FFFu)
    };
}

std::array<std::uint16_t,8> Pcm16VideoEncoder::physicalWords14(std::int64_t g) const
{
    const auto g0=groupAt(g),g1=groupAt(g-16),g2=groupAt(g-32),g3=groupAt(g-48),
               g4=groupAt(g-64),g5=groupAt(g-80),gp=groupAt(g-96),gq=groupAt(g-112);

    const auto upper14 = [](std::uint16_t v) {
        return static_cast<std::uint16_t>((v >> 2) & 0x3FFFu);
    };

    const std::uint16_t p14 = static_cast<std::uint16_t>(
        upper14(gp[0]) ^ upper14(gp[1]) ^ upper14(gp[2]) ^
        upper14(gp[3]) ^ upper14(gp[4]) ^ upper14(gp[5]));

    return {
        upper14(g0[0]), upper14(g1[1]), upper14(g2[2]), upper14(g3[3]),
        upper14(g4[4]), upper14(g5[5]), p14, q14(gq)
    };
}

std::array<std::uint16_t,8> Pcm16VideoEncoder::controlWords(bool mode16, bool preEmphasis) const
{
    // 56-bit heading = 1100 repeated 14 times. Because 14-bit word
    // boundaries are not multiples of four, the words alternate 0x3333/0x0CCC.
    // Content ID and address are zero for this test encoder.
    // CT bits 1..10 = 0, 11 copy inhibit absent = 0, 12 P present = 0,
    // 13 Q present=0 / absent=1, 14 pre-emphasis present=0 / absent=1.
    const std::uint16_t control = static_cast<std::uint16_t>(
        (mode16 ? 0x0002u : 0x0000u) |
        (preEmphasis ? 0x0000u : 0x0001u));

    return {0x3333u,0x0CCCu,0x3333u,0x0CCCu,0u,0u,0u,control};
}

std::uint16_t Pcm16VideoEncoder::crc(const std::array<std::uint16_t,8>& w)
{
    std::array<int,128> b{};
    int k=0;
    for(auto v:w)
        for(int i=13;i>=0;--i)
            b[k++]=(v>>i)&1;
    for(int i=0;i<16;++i)
        b[i]^=1;
    for(int i=0;i<112;++i)
        if(b[i])
        {
            b[i]^=1;
            b[i+4]^=1;
            b[i+11]^=1;
            b[i+16]^=1;
        }
    std::uint16_t r=0;
    for(int i=112;i<128;++i)
        r=static_cast<std::uint16_t>((r<<1)|b[i]);
    return r;
}

void Pcm16VideoEncoder::shapeLuma(std::vector<float>& y, double bandwidthMHz)
{
    if (y.empty())
        return;

    // Restore the original 0.2.4 pulse shaper: two forward/reverse one-pole
    // pairs. It is symmetric (zero phase), keeps bit centres in place and
    // produced the cleaner eye/plateaus seen in the early encoder tests.
    constexpr double SampleRateMHz = 13.5;
    constexpr double Pi = 3.14159265358979323846;
    const double fc = std::clamp(bandwidthMHz, 1.0, 6.0);
    const double a = std::exp(-2.0 * Pi * fc / SampleRateMHz);

    auto forward = [&](bool reverse)
    {
        if (!reverse)
        {
            double state = y.front();
            for (std::size_t x = 1; x < y.size(); ++x)
            {
                state = (1.0-a)*static_cast<double>(y[x]) + a*state;
                y[x] = static_cast<float>(state);
            }
        }
        else
        {
            double state = y.back();
            for (std::size_t x = y.size()-1; x-- > 0; )
            {
                state = (1.0-a)*static_cast<double>(y[x]) + a*state;
                y[x] = static_cast<float>(state);
            }
        }
    };

    forward(false);
    forward(true);
    forward(false);
    forward(true);
}

void Pcm16VideoEncoder::applyPreEmphasis(std::vector<std::int16_t>& stereo, bool enabled)
{
    if (!enabled)
    {
        if (preEmphasisStateEnabled_)
        {
            preX1_ = {};
            preY1_ = {};
        }
        preEmphasisStateEnabled_ = false;
        return;
    }

    if (!preEmphasisStateEnabled_)
    {
        preX1_ = {};
        preY1_ = {};
        preEmphasisStateEnabled_ = true;
    }

    // Standard PCM adaptor 50/15 us pre-emphasis at 44.1 kHz.
    //
    // Do not use the plain bilinear transform here: close to Nyquist its
    // frequency warping produces almost +1 dB too much boost around
    // 15..18 kHz when played through the Sony PCM-701ES de-emphasis path.
    // These first-order IIR coefficients are a direct 44.1 kHz fit to the
    // analog target H(s)=(1+s*50us)/(1+s*15us), with unity DC gain.
    // Magnitude error over 20 Hz..20 kHz is kept within about +/-0.08 dB.
    constexpr double b0 =  2.1712914738343962;
    constexpr double b1 = -1.3609515879979208;
    constexpr double a1 = -0.18966011416352457;

    for (std::size_t i = 0; i + 1 < stereo.size(); i += 2)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const double x = static_cast<double>(stereo[i+static_cast<std::size_t>(ch)]) / 32768.0;
            const double y = b0*x + b1*preX1_[ch] - a1*preY1_[ch];
            preX1_[ch] = x;
            preY1_[ch] = y;

            const long long q = std::llround(std::clamp(y, -1.0, 32767.0/32768.0) * 32768.0);
            stereo[i+static_cast<std::size_t>(ch)] = static_cast<std::int16_t>(
                std::clamp(q,
                    static_cast<long long>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<long long>(std::numeric_limits<std::int16_t>::max())));
        }
    }
}

void Pcm16VideoEncoder::renderLine(std::uint8_t* row, const std::array<std::uint16_t,8>& w) const
{
    std::array<int,140> cells{};
    cells[0]=1; cells[1]=0; cells[2]=1; cells[3]=0;
    int k=4;
    for(auto v:w)
        for(int i=13;i>=0;--i)
            cells[k++]=(v>>i)&1;

    const auto c=crc(w);
    for(int i=15;i>=0;--i)
        cells[k++]=(c>>i)&1;
    cells[k++]=0;
    for(int i=0;i<7;++i)
        cells[k++]=1;

    // Sony/EIAJ PCM adaptor levels are deliberately NOT full black/white
    // for the NRZ data.  In studio-range BT.601 luma these values correspond
    // to approximately:
    //   Y=16  -> 0.30 V blanking/black
    //   Y=16  -> 0.30 V data 0 (same as black/pedestal)
    //   Y=141 -> 0.70 V data 1
    //   Y=235 -> 1.00 V peak-white reference
    //
    // The final seven bit cells are the peak-white reference pulse.  Keep
    // them at 100 % white; they are a level/AGC reference and must not be
    // confused with ordinary data-1 cells.
    constexpr float BlankLevel = 16.0f;
    constexpr float Data0Level = 16.0f;
    constexpr float Data1Level = 141.0f;
    constexpr float PeakWhiteLevel = 235.0f;
    constexpr int PeakWhiteFirstCell = 133; // cell 132 is the preceding 0 cell

    std::array<float, Width> original{};
    for(int x=0;x<Width;++x)
    {
        const int cell = std::min(139, x*140/720);
        if(cell >= PeakWhiteFirstCell)
            original[static_cast<std::size_t>(x)] = PeakWhiteLevel;
        else
            original[static_cast<std::size_t>(x)] = cells[static_cast<std::size_t>(cell)] ? Data1Level : Data0Level;
    }

    const int offset=std::clamp(horizontalOffsetPixels_.load(),-24,24);
    constexpr int Guard=64;
    std::vector<float> extended(static_cast<std::size_t>(Width+2*Guard),BlankLevel);

    for(int x=0;x<Width;++x)
    {
        const int destination=Guard+x+offset;
        if(destination>=0 && destination<static_cast<int>(extended.size()))
            extended[static_cast<std::size_t>(destination)]=original[static_cast<std::size_t>(x)];
    }

    if(pulseShapingEnabled_.load())
        shapeLuma(extended,videoBandwidthMHz_.load());

    for(int x=0;x<Width;x+=2)
    {
        const auto sampleAt=[&](int activeX)
        {
            const auto value=extended[static_cast<std::size_t>(Guard+activeX)];
            return static_cast<std::uint8_t>(std::clamp(std::lround(value),0L,255L));
        };
        row[2*x]=128; row[2*x+1]=sampleAt(x);
        row[2*x+2]=128; row[2*x+3]=sampleAt(x+1);
    }
}

void Pcm16VideoEncoder::fillBlackLine(std::uint8_t* row)
{
    for(int x=0;x<Width;x+=2)
    {
        row[2*x]=128; row[2*x+1]=16;
        row[2*x+2]=128; row[2*x+3]=16;
    }
}

void Pcm16VideoEncoder::encodeFrame(const std::vector<std::int16_t>& stereoIn,std::vector<std::uint8_t>& out)
{
    out.assign(Width*Height*2,0);
    const auto base=static_cast<std::int64_t>(frame_)*588;

    // Snapshot format state before touching audio. This single snapshot is
    // used for pre-emphasis, control-H and all PCM data lines in this frame.
    const bool mode16=mode16Bit_.load();
    const bool framePreEmphasis=preEmphasisEnabled_.load();

    std::vector<std::int16_t> stereo=stereoIn;
    applyPreEmphasis(stereo, framePreEmphasis);

    for(int g=0;g<588;++g)
    {
        std::int16_t six[6]{};
        for(int j=0;j<3;++j)
        {
            const auto si=(g*3+j)*2;
            if(si+1<static_cast<int>(stereo.size()))
            {
                six[2*j]=stereo[static_cast<std::size_t>(si)];
                six[2*j+1]=stereo[static_cast<std::size_t>(si+1)];
            }
        }
        history_.push_back(makeGroup(six));
    }

    if(history_.size()>2048)
    {
        const auto drop=history_.size()-1024;
        history_.erase(history_.begin(),history_.begin()+static_cast<std::ptrdiff_t>(drop));
        firstGroup_+=static_cast<std::int64_t>(drop);
    }

    // Sony/EIAJ output starts at the top of each captured field.  The control-H
    // is the first visible PCM line, followed immediately by audio/data H.
    // This is the proven mapping accepted by the Sony hardware in this setup.
    for(int fld=0;fld<2;++fld)
    {
        for(int r=0;r<288;++r)
        {
            auto* row=out.data()+((2*r+fld)*Width*2);

            if(r==0)
            {
                renderLine(row,controlWords(mode16, framePreEmphasis));
                continue;
            }

            const int audioRow = r - 1;
            const int logicalLine = 1 + audioRow; // 1..287 visible here

            if(audioRow>=0 && logicalLine>=1 && logicalLine<=294)
            {
                const auto physical=base+fld*294+(logicalLine-1);
                renderLine(row,mode16 ? physicalWords16(physical) : physicalWords14(physical));
            }
            else
            {
                fillBlackLine(row);
            }
        }
    }

    ++frame_;
}
