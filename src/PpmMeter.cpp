#include "PpmMeter.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QColor>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QTimer>

namespace
{
constexpr double kMinDb = -60.0;
constexpr double kMaxDb = 0.0;
constexpr double kDangerDb = -1.0;
constexpr int kSegmentCount = 201;

// Deliberately much quicker than the previous meter.
// 60 dB/s means a full-scale indication can fall to -60 dB in ~1 second.
constexpr double kDecayDbPerSecond = 60.0;
constexpr double kPeakHoldSeconds = 1.00;
constexpr double kPeakHoldDecayDbPerSecond = 24.0;

constexpr int kOuterMarginX = 8;
constexpr int kOuterMarginY = 5;
constexpr int kSegmentGap = 2;

double dbToUnit(double db)
{
    // RTW-like visual law: spend much more horizontal space on the useful
    // upper range.  -20 dB sits exactly halfway across the meter.
    //
    // The anchors also keep the printed scale and the 201-segment fill on
    // exactly the same geometry.
    constexpr std::array<double, 10> dbPoints{
        -60.0, -40.0, -30.0, -20.0, -15.0,
        -12.0,  -9.0,  -6.0,  -3.0,   0.0
    };
    constexpr std::array<double, 10> unitPoints{
         0.00,  0.17,  0.32,  0.50,  0.61,
         0.68,  0.75,  0.83,  0.91,  1.00
    };

    db = std::clamp(db, kMinDb, kMaxDb);

    for (std::size_t i = 1; i < dbPoints.size(); ++i)
    {
        if (db <= dbPoints[i])
        {
            const double spanDb = dbPoints[i] - dbPoints[i - 1];
            const double t = spanDb > 0.0
                ? (db - dbPoints[i - 1]) / spanDb
                : 0.0;
            return unitPoints[i - 1] +
                t * (unitPoints[i] - unitPoints[i - 1]);
        }
    }

    return 1.0;
}

QRectF meterTrackRect(const QWidget* widget)
{
    return QRectF(widget->rect()).adjusted(
        kOuterMarginX,
        kOuterMarginY,
        -kOuterMarginX,
        -kOuterMarginY);
}

QColor inactiveSegment()
{
    return QColor(22, 17, 12);
}

QColor normalSegment()
{
    // Warm off-white, closer to the classic RTW incandescent/LED appearance
    // than a modern saturated green bar.
    return QColor(238, 132, 28);
}

QColor dangerSegment()
{
    return QColor(220, 28, 28);
}

int dbToSegmentIndex(double db)
{
    return std::clamp(
        static_cast<int>(std::lround(
            dbToUnit(db) * static_cast<double>(kSegmentCount - 1))),
        0,
        kSegmentCount - 1);
}

QRect segmentRectForIndex(const QRectF& track, int index, int top, int height)
{
    const double usableWidth = track.width() - 2.0;
    const double segmentPitch = usableWidth / static_cast<double>(kSegmentCount);
    const int x0 = static_cast<int>(std::round(track.left() + 1.0 + index * segmentPitch));
    const int x1 = static_cast<int>(std::round(track.left() + 1.0 + (index + 1) * segmentPitch));
    const int segWidth = std::max(1, x1 - x0 - kSegmentGap);
    return QRect(x0, top, segWidth, height);
}

double segmentCenterX(const QRectF& track, int index)
{
    const double usableWidth = track.width() - 2.0;
    const double segmentPitch = usableWidth / static_cast<double>(kSegmentCount);
    const int x0 = static_cast<int>(std::round(track.left() + 1.0 + index * segmentPitch));
    const int x1 = static_cast<int>(std::round(track.left() + 1.0 + (index + 1) * segmentPitch));
    const int segWidth = std::max(1, x1 - x0 - kSegmentGap);
    return static_cast<double>(x0) + static_cast<double>(segWidth) * 0.5;
}

double scaleAnchorX(const QRectF& track, double db)
{
    return segmentCenterX(track, dbToSegmentIndex(db));
}

double labelLeftForAnchor(const QFontMetrics& fm, const QString& label, int db, double anchorX)
{
    if (db < 0)
    {
        const int signWidth = fm.horizontalAdvance(QStringLiteral("-"));
        const int magnitudeWidth = fm.horizontalAdvance(QString::number(-db));
        return anchorX - static_cast<double>(signWidth) - static_cast<double>(magnitudeWidth) * 0.5;
    }

    const int labelWidth = fm.horizontalAdvance(label);
    return anchorX - static_cast<double>(labelWidth) * 0.5;
}
}

PpmMeter::PpmMeter(const QString&, QWidget* parent)
    : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(28);

    animationTimer_ = new QTimer(this);
    animationTimer_->setInterval(16);
    connect(
        animationTimer_,
        &QTimer::timeout,
        this,
        &PpmMeter::animateBallistics);

    elapsed_.start();
    animationTimer_->start();
}

void PpmMeter::setInputPeak(float linearPeak)
{
    targetDb_ = linearToDb(static_cast<double>(linearPeak));

    // PPM attack is effectively immediate for this display.
    if (targetDb_ > displayedDb_)
        displayedDb_ = targetDb_;

    // One discrete peak-hold segment, just like the old hardware meters.
    if (targetDb_ >= peakHoldDb_)
    {
        peakHoldDb_ = targetDb_;
        peakHoldSecondsRemaining_ = kPeakHoldSeconds;
    }

    update();
}

QSize PpmMeter::sizeHint() const
{
    return QSize(900, 34);
}

QSize PpmMeter::minimumSizeHint() const
{
    return QSize(420, 28);
}

void PpmMeter::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    p.fillRect(rect(), QColor(5, 5, 5));

    QRectF track = meterTrackRect(this);
    if (track.width() <= 20.0 || track.height() <= 4.0)
        return;

    p.setPen(QPen(QColor(5, 5, 5), 1.0));
    p.setBrush(QColor(8, 8, 8));
    p.drawRect(track);

    const int activeSegments = std::clamp(
        static_cast<int>(std::floor(
            dbToUnit(displayedDb_) * static_cast<double>(kSegmentCount) + 0.5)),
        0,
        kSegmentCount);

    const int segTop = static_cast<int>(std::round(track.top() + 4.0));
    const int segHeight = std::max(1, static_cast<int>(std::round(track.height() - 8.0)));

    for (int i = 0; i < kSegmentCount; ++i)
    {
        const QRect segment = segmentRectForIndex(track, i, segTop, segHeight);

        const double segmentUnit =
            static_cast<double>(i + 1) /
            static_cast<double>(kSegmentCount);

        QColor fill = inactiveSegment();
        if (i < activeSegments)
            fill = segmentUnit >= dbToUnit(kDangerDb)
                ? dangerSegment()
                : normalSegment();

        p.fillRect(segment, fill);
    }

    // One bright peak-hold segment. It is intentionally not a line: it is
    // rendered using exactly the same discrete LED geometry as the meter.
    if (peakHoldDb_ > kMinDb + 0.01)
    {
        const int holdIndex = dbToSegmentIndex(peakHoldDb_);
        const QRect holdSegment = segmentRectForIndex(track, holdIndex, segTop, segHeight);

        const QColor holdColor =
            peakHoldDb_ >= kDangerDb
                ? QColor(255, 70, 55)
                : QColor(238, 132, 28);

        p.fillRect(holdSegment, holdColor);
    }

    // Thin red danger-zone marker at -1 dBFS, like a hardware scale warning.
    const double dangerX = scaleAnchorX(track, kDangerDb);

    p.setPen(QPen(QColor(185, 25, 25), 1));
    p.drawLine(
        QPointF(dangerX, track.top() + 1.0),
        QPointF(dangerX, track.bottom() - 1.0));
}

void PpmMeter::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        emit clicked();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void PpmMeter::animateBallistics()
{
    const qint64 elapsedMs = elapsed_.restart();
    if (elapsedMs <= 0)
        return;

    const double dt = static_cast<double>(elapsedMs) / 1000.0;
    bool changed = false;

    if (displayedDb_ > targetDb_)
    {
        displayedDb_ = std::max(
            targetDb_,
            displayedDb_ - kDecayDbPerSecond * dt);
        changed = true;
    }

    if (peakHoldSecondsRemaining_ > 0.0)
    {
        peakHoldSecondsRemaining_ =
            std::max(0.0, peakHoldSecondsRemaining_ - dt);
    }
    else if (peakHoldDb_ > displayedDb_)
    {
        peakHoldDb_ = std::max(
            displayedDb_,
            peakHoldDb_ - kPeakHoldDecayDbPerSecond * dt);
        changed = true;
    }
    else
    {
        peakHoldDb_ = displayedDb_;
    }

    if (changed)
        update();
}

double PpmMeter::linearToDb(double linear)
{
    if (!std::isfinite(linear) || linear <= 0.0)
        return kMinDb;

    const double db = 20.0 * std::log10(linear);
    return std::clamp(db, kMinDb, kMaxDb);
}

PpmScale::PpmScale(QWidget* parent)
    : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(36);
}

QSize PpmScale::sizeHint() const
{
    return QSize(900, 50);
}

QSize PpmScale::minimumSizeHint() const
{
    return QSize(420, 28);
}

void PpmScale::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.fillRect(rect(), QColor(5, 5, 5));

    const QRectF track = meterTrackRect(this);
    if (track.width() <= 20.0)
        return;

    constexpr std::array<int, 10> majorDb{
        -60, -40, -30, -20, -15, -12, -9, -6, -3, 0
    };
    constexpr std::array<int, 9> minorDb{
        -50, -35, -25, -18, -14, -10, -8, -5, -1
    };

    const QColor text(232, 232, 228);
    const QColor tick(210, 210, 205);

    const double h = static_cast<double>(height());
    const double topPad = std::clamp(h * 0.06, 4.0, 10.0);
    const double bottomPad = std::clamp(h * 0.06, 4.0, 10.0);
    const double tickLen = std::clamp(h * 0.16, 8.0, 18.0);
    const double gapAboveText = std::clamp(h * 0.08, 6.0, 12.0);
    const double gapBelowText = std::clamp(h * 0.08, 6.0, 12.0);
    const double minorInset = std::clamp(tickLen * 0.45, 3.0, 7.0);

    QFont f = p.font();
    const double pointSize = std::clamp(h * 0.24, 9.5, 16.5);
    f.setPointSizeF(pointSize);
    f.setWeight(QFont::Bold);
    p.setFont(f);
    const QFontMetrics fm(f);

    // Symmetrical RTW-style scale: ticks belong to the meter above/below,
    // while the text floats in a clean black band between them.
    const double upperTickTop = topPad;
    const double upperTickBottom = upperTickTop + tickLen;
    const double lowerTickBottom = h - bottomPad;
    const double lowerTickTop = lowerTickBottom - tickLen;

    p.setPen(QPen(tick, 1));
    for (const int db : majorDb)
    {
        const double x = scaleAnchorX(track, static_cast<double>(db));

        p.drawLine(
            QPointF(x, upperTickTop),
            QPointF(x, upperTickBottom));
        p.drawLine(
            QPointF(x, lowerTickTop),
            QPointF(x, lowerTickBottom));
    }

    p.setPen(QPen(QColor(145, 145, 140), 1));
    for (const int db : minorDb)
    {
        const double x = scaleAnchorX(track, static_cast<double>(db));

        p.drawLine(
            QPointF(x, upperTickTop + minorInset),
            QPointF(x, upperTickBottom));
        p.drawLine(
            QPointF(x, lowerTickTop),
            QPointF(x, lowerTickBottom - minorInset));
    }

    // Labels live in a separate band, but negative labels are anchored on the
    // numeric part only, so the tick sits under '40' in '-40' rather than
    // under the full string width.
    const double textBandTop = upperTickBottom + gapAboveText;
    const double textBandBottom = lowerTickTop - gapBelowText;
    p.setPen(text);
    for (const int db : majorDb)
    {
        const double x = scaleAnchorX(track, static_cast<double>(db));
        const QString label = QString::number(db);
        const int labelWidth = fm.horizontalAdvance(label);
        const double labelX = labelLeftForAnchor(fm, label, db, x);

        const QRectF labelRect(
            labelX,
            textBandTop,
            static_cast<double>(labelWidth),
            std::max(1.0, textBandBottom - textBandTop));
        p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, label);
    }

    // Last dB is the danger zone; mark its start on both tick rows, but
    // leave the central text band untouched.
    const double dangerX = scaleAnchorX(track, kDangerDb);
    p.setPen(QPen(QColor(205, 30, 30), 2));
    p.drawLine(
        QPointF(dangerX, upperTickTop),
        QPointF(dangerX, upperTickBottom));
    p.drawLine(
        QPointF(dangerX, lowerTickTop),
        QPointF(dangerX, lowerTickBottom));
}

void PpmScale::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        emit clicked();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}
