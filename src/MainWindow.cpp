#include "MainWindow.h"
#include "PpmMeter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <deque>
#include <QSlider>
#include <QStackedWidget>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QString>
#include <QTimer>
#include <QTabWidget>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWidget>



class TxMarginGraph final : public QWidget
{
public:
    explicit TxMarginGraph(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumWidth(320);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setTargetMargin(double ms)
    {
        targetMarginMs_ = ms;
        update();
    }

    void setLiveMargin(double ms)
    {
        liveMarginMs_ = ms;
        update();
    }

    void appendSample(double rawBufferMs,
                      double fastAverageBufferMs,
                      double average3sReportedMs,
                      double positionCorrectionPpm,
                      double actualAsrcStep)
    {
        actualAsrcStep_ = actualAsrcStep;
        latestRawMs_ = rawBufferMs;
        latestFastAverageMs_ = fastAverageBufferMs;
        latestAverage3sReportedMs_ = average3sReportedMs;
        latestCorrectionPpm_ = positionCorrectionPpm;

        if ((++appendDivider_ & 1U) != 0U)
            return;

        rawBufferSamples_.push_back(rawBufferMs);
        fastAverageBufferSamples_.push_back(fastAverageBufferMs);
        average3sReportedSamples_.push_back(average3sReportedMs);
        correctionPpmSamples_.push_back(positionCorrectionPpm);

        while (rawBufferSamples_.size() > MaxSamples) rawBufferSamples_.pop_front();
        while (fastAverageBufferSamples_.size() > MaxSamples) fastAverageBufferSamples_.pop_front();
        while (average3sReportedSamples_.size() > MaxSamples) average3sReportedSamples_.pop_front();
        while (correctionPpmSamples_.size() > MaxSamples) correctionPpmSamples_.pop_front();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), palette().base());

        const QRectF outer = rect().adjusted(4, 4, -4, -4);
        if (outer.width() < 280.0 || outer.height() < 180.0)
            return;

        const QColor textColor = palette().text().color();
        const QColor gridColor = palette().mid().color();
        const QColor panelFill = palette().alternateBase().color();
        const QColor fillColor(30, 150, 210);
        const QColor ppmColor(190, 90, 30);
        const QColor targetColor(0, 170, 70);

        const double currentFill = rawBufferSamples_.empty()
            ? liveMarginMs_ : latestAverage3sReportedMs_;
        const double correctionPpm = correctionPpmSamples_.empty()
            ? 0.0 : latestCorrectionPpm_;

        const double infoH = 74.0;
        const QRectF info(outer.left() + 48.0, outer.top(),
                          outer.width() - 100.0, infoH);
        p.setPen(QPen(gridColor, 1));
        p.setBrush(panelFill);
        p.drawRoundedRect(info, 4.0, 4.0);

        p.setPen(textColor);
        QFont f = p.font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(info.adjusted(10, 6, -10, -40), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Buffer depth (3 s avg)  %1 ms").arg(currentFill, 0, 'f', 4));
        f.setBold(false);
        p.setFont(f);
        p.drawText(info.adjusted(10, 30, -10, -16), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Position nudge  %1 ppm    step %2")
                       .arg(correctionPpm, 0, 'f', 0)
                       .arg(actualAsrcStep_, 0, 'f', 9));

        const double graphTop = info.bottom() + 8.0;
        const double graphBottom = outer.bottom() - 18.0;
        const double graphH = graphBottom - graphTop;
        if (graphH < 80.0)
            return;

        const double gap = 8.0;
        const double ppmH = std::max(70.0, graphH * 0.25);
        const QRectF fillR(outer.left() + 48.0, graphTop,
                           outer.width() - 100.0, graphH - ppmH - gap);
        const QRectF ppmR(fillR.left(), fillR.bottom() + gap, fillR.width(), ppmH);

        auto panel = [&](const QRectF& r, const QString& title)
        {
            p.setPen(QPen(gridColor, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r);
            p.setPen(textColor);
            p.drawText(QRectF(r.left()+6, r.top()+3, r.width()-12, 18),
                       Qt::AlignLeft | Qt::AlignVCenter, title);
        };
        panel(fillR, QStringLiteral("Completion margin: raw / fast avg / 3 s avg (60 s)"));
        panel(ppmR, QStringLiteral("Temporary position nudge"));

        if (!rawBufferSamples_.empty())
        {
            double ymin = targetMarginMs_;
            double ymax = targetMarginMs_;
            auto includeRange = [&](const std::deque<double>& samples)
            {
                for (double v : samples)
                {
                    if (!std::isfinite(v)) continue;
                    ymin = std::min(ymin, v);
                    ymax = std::max(ymax, v);
                }
            };
            includeRange(rawBufferSamples_);
            includeRange(fastAverageBufferSamples_);
            includeRange(average3sReportedSamples_);
            double span = ymax - ymin;
            if (span < 1.0) span = 1.0;
            const double pad = span * 0.12;
            ymin -= pad; ymax += pad;

            auto yMap = [&](double v)
            {
                return fillR.bottom() - (v-ymin)/(ymax-ymin)*fillR.height();
            };

            p.setPen(QPen(targetColor, 1, Qt::DashLine));
            const double ty = yMap(targetMarginMs_);
            p.drawLine(QPointF(fillR.left(), ty), QPointF(fillR.right(), ty));

            auto drawTrace = [&](const std::deque<double>& samples, const QPen& pen)
            {
                if (samples.empty()) return;
                QPainterPath path;
                bool first = true;
                const std::size_t n = samples.size();
                for (std::size_t i = 0; i < n; ++i)
                {
                    const double x = n > 1
                        ? fillR.left() + static_cast<double>(i) * fillR.width() / static_cast<double>(n-1)
                        : fillR.left();
                    const double y = yMap(samples[i]);
                    if (first) { path.moveTo(x,y); first=false; } else path.lineTo(x,y);
                }
                p.setPen(pen);
                p.drawPath(path);
            };

            drawTrace(rawBufferSamples_, QPen(palette().mid().color(), 1));
            drawTrace(fastAverageBufferSamples_, QPen(fillColor, 2));
            drawTrace(average3sReportedSamples_, QPen(palette().text().color(), 3));

            p.setPen(textColor);
            p.drawText(QRectF(fillR.left()+8, fillR.top()+22, 520, 18), Qt::AlignLeft,
                       QStringLiteral("raw %1 ms   fast avg %2 ms   3 s avg %3 ms")
                           .arg(latestRawMs_, 0, 'f', 3)
                           .arg(latestFastAverageMs_, 0, 'f', 3)
                           .arg(latestAverage3sReportedMs_, 0, 'f', 3));
            p.drawText(QRectF(outer.left(), fillR.top(), 44, 16), Qt::AlignRight, QString::number(ymax,'f',2));
            p.drawText(QRectF(outer.left(), fillR.bottom()-16, 44, 16), Qt::AlignRight, QString::number(ymin,'f',2));
        }

        // Temporary position nudge; acquisition may reach +/-5000 ppm.
        p.setPen(QPen(gridColor, 1, Qt::DotLine));
        p.drawLine(QPointF(ppmR.left(), ppmR.center().y()), QPointF(ppmR.right(), ppmR.center().y()));
        if (!correctionPpmSamples_.empty())
        {
            QPainterPath path;
            bool first = true;
            const std::size_t n = correctionPpmSamples_.size();
            for (std::size_t i = 0; i < n; ++i)
            {
                const double x = n > 1
                    ? ppmR.left() + static_cast<double>(i) * ppmR.width() / static_cast<double>(n-1)
                    : ppmR.left();
                const double v = std::clamp(correctionPpmSamples_[i], -5000.0, 5000.0);
                const double y = ppmR.center().y() - (v/5000.0)*(ppmR.height()*0.46);
                if (first) { path.moveTo(x,y); first=false; } else path.lineTo(x,y);
            }
            p.setPen(QPen(ppmColor, 2));
            p.drawPath(path);
        }
        p.setPen(textColor);
        p.drawText(QRectF(outer.left(), ppmR.top(), 44, 16), Qt::AlignRight, QStringLiteral("+1000"));
        p.drawText(QRectF(outer.left(), ppmR.bottom()-16, 44, 16), Qt::AlignRight, QStringLiteral("-1000"));
    }

private:
    static constexpr std::size_t MaxSamples = 750; // ~60 s at ~12.5 Hz
    static constexpr double GraphSamplesPerSecond = 12.5;
    double targetMarginMs_ = 20.0;
    double liveMarginMs_ = 0.0;
    double actualAsrcStep_ = 0.0;
    double latestRawMs_ = 0.0;
    double latestFastAverageMs_ = 0.0;
    double latestAverage3sReportedMs_ = 0.0;
    double latestCorrectionPpm_ = 0.0;
    unsigned appendDivider_ = 0;
    std::deque<double> rawBufferSamples_;
    std::deque<double> fastAverageBufferSamples_;
    std::deque<double> average3sReportedSamples_;
    std::deque<double> correctionPpmSamples_;
};




MainWindow::MainWindow()
{
    setWindowTitle("OpenScope PCM Encoder 0.9.0");

    updateOnAirIndicator(false);
    setFixedSize(1050, 430);

    auto* root = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    mainStack_ = new QStackedWidget(root);
    rootLayout->addWidget(mainStack_);

    tabs_ = new QTabWidget(mainStack_);
    auto* tabs = tabs_;
    auto* mainPage = new QWidget(tabs);
    auto* vertical = new QVBoxLayout(mainPage);
    auto* form = new QFormLayout();

    audio_ = new QComboBox();
    video_ = new QComboBox();

    form->addRow("ASIO audio input", audio_);
    form->addRow("Blackmagic video output", video_);

    mode_ = new QComboBox();
    mode_->addItem("16-bit PCM-F1");
    mode_->addItem("14-bit EIAJ");
    mode_->addItem("Ham PCM 2.0 (48 kHz / 14-bit)");
    form->addRow("PCM mode", mode_);

    hamText_ = new QLineEdit();
    hamText_->setMaxLength(10);
    hamText_->setText("OPEN SCOPE");
    hamText_->setEnabled(false);
    hamText_->setToolTip("Ham PCM 2.0 protected 10-character text channel.");
    form->addRow("Ham text", hamText_);

    preEmphasis_ = new QComboBox();
    preEmphasis_->addItem("Auto", 0);
    preEmphasis_->addItem("Off", 1);
    preEmphasis_->addItem("On", 2);
    preEmphasis_->setToolTip(
        "50/15 us audio pre-emphasis. Auto enables pre-emphasis for 14-bit EIAJ "
        "and disables it for 16-bit PCM-F1.");
    form->addRow("Pre-emphasis", preEmphasis_);

    vertical->addLayout(form);
    vertical->addSpacing(8);

    l_ = new PpmMeter("L", mainPage);
    ppmScale_ = new PpmScale(mainPage);
    r_ = new PpmMeter("R", mainPage);
    l_->setToolTip("RTW-style 201-segment programme meter, -60 to 0 dBFS. Fast attack, very fast 60 dB/s decay, orange peak-hold and a red top danger zone. Click while encoding for meter-only view; press F11 for meter-only fullscreen.");
    r_->setToolTip(l_->toolTip());
    vertical->setSpacing(6);
    meterPanel_ = new QWidget(mainPage);
    meterPanel_->setStyleSheet(
        "QWidget { background: rgb(5,5,5); }");
    auto* mudtwLogo = new QLabel(QStringLiteral("MUDTW"), meterPanel_);
    QFont mudtwFont(QStringLiteral("Arial"));
    mudtwFont.setBold(true);
    mudtwFont.setStretch(QFont::Expanded);
    mudtwFont.setPointSize(9);
    mudtwLogo->setFont(mudtwFont);
    mudtwLogo->setStyleSheet(
        "QLabel { color: rgb(238,238,235); background: transparent; border: none; }");
    mudtwLogo->setGeometry(18, 3, 110, 20);
    mudtwLogo->raise();

    auto* meterPanelLayout = new QVBoxLayout(meterPanel_);
    meterPanelLayout->setContentsMargins(12, 22, 12, 22);
    meterPanelLayout->setSpacing(1);
    meterPanelLayout->addWidget(l_);
    meterPanelLayout->addWidget(ppmScale_);
    meterPanelLayout->addWidget(r_);

    vertical->addWidget(meterPanel_);
    vertical->addSpacing(12);

    meterPage_ = new QWidget(mainStack_);
    auto* meterOnlyLayout = new QVBoxLayout(meterPage_);
    meterOnlyLayout->setContentsMargins(24, 16, 24, 24);
    meterOnlyLayout->setSpacing(6);

    meterOnlyL_ = new PpmMeter(QString(), meterPage_);
    meterOnlyScale_ = new PpmScale(meterPage_);
    meterOnlyR_ = new PpmMeter(QString(), meterPage_);

    meterOnlyL_->setMinimumHeight(42);
    meterOnlyR_->setMinimumHeight(42);
    meterOnlyScale_->setMinimumHeight(54);
    meterOnlyInstrument_ = nullptr;

    //meterOnlyLayout->addStretch(1);

    meterOnlyInstrument_ = new QWidget(meterPage_);
    meterOnlyInstrument_->setStyleSheet(
        "QWidget { background: rgb(5,5,5); border: 1px solid rgb(35,35,35); }");
    auto* meterOnlyMudtwLogo = new QLabel(QStringLiteral("MUDTW"), meterOnlyInstrument_);
    QFont meterOnlyMudtwFont(QStringLiteral("Arial"));
    meterOnlyMudtwFont.setBold(true);
    meterOnlyMudtwFont.setStretch(QFont::Expanded);
    meterOnlyMudtwFont.setPointSize(12);
    meterOnlyMudtwLogo->setFont(meterOnlyMudtwFont);
    meterOnlyMudtwLogo->setStyleSheet(
        "QLabel { color: rgb(238,238,235); background: transparent; border: none; }");
    meterOnlyMudtwLogo->setGeometry(22, 5, 130, 22);
    meterOnlyMudtwLogo->raise();

    auto* meterOnlyInstrumentLayout = new QVBoxLayout(meterOnlyInstrument_);
    meterOnlyInstrumentLayout->setContentsMargins(18, 30, 18, 30);
    meterOnlyInstrumentLayout->setSpacing(1);
    meterOnlyInstrumentLayout->addWidget(meterOnlyL_);
    meterOnlyInstrumentLayout->addWidget(meterOnlyScale_);
    meterOnlyInstrumentLayout->addWidget(meterOnlyR_);

    meterOnlyLayout->addWidget(meterOnlyInstrument_);
    meterOnlyLayout->addStretch(1);

    meterPage_->setStyleSheet("background: rgb(5, 5, 5);");

    connect(l_, &PpmMeter::clicked, this, &MainWindow::toggleMeterOnlyView);
    connect(r_, &PpmMeter::clicked, this, &MainWindow::toggleMeterOnlyView);
    connect(ppmScale_, &PpmScale::clicked, this, &MainWindow::toggleMeterOnlyView);

    connect(meterOnlyL_, &PpmMeter::clicked, this, &MainWindow::toggleMeterOnlyView);
    connect(meterOnlyR_, &PpmMeter::clicked, this, &MainWindow::toggleMeterOnlyView);
    connect(meterOnlyScale_, &PpmScale::clicked, this, &MainWindow::toggleMeterOnlyView);

    start_ = new QPushButton("Start PCM video output");
    vertical->addWidget(start_);
    vertical->addStretch();

    // Keep status_ for internal/error text updates, but do not clutter the
    // Encoder front panel with a redundant RUNNING/Stopped line.
    status_ = new QLabel("Stopped", mainPage);
    status_->hide();

    auto* advancedPage = new QWidget(tabs);
    auto* advancedLayout = new QVBoxLayout(advancedPage);
    advancedLayout->setContentsMargins(18, 18, 18, 18);
    advancedLayout->setSpacing(16);

    auto* advancedForm = new QFormLayout();
    advancedForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    advancedForm->setHorizontalSpacing(16);
    advancedForm->setVerticalSpacing(18);

    bandwidth_ = new QSlider(Qt::Horizontal, advancedPage);
    bandwidth_->setRange(10, 60);
    bandwidth_->setValue(50);
    bandwidth_->setSingleStep(1);
    bandwidthValue_ = new QLabel("5.0 MHz", advancedPage);

    auto* bandwidthWidget = new QWidget(advancedPage);
    auto* bandwidthRow = new QHBoxLayout(bandwidthWidget);
    bandwidthRow->setContentsMargins(0, 6, 0, 6);
    bandwidthWidget->setMinimumHeight(48);
    bandwidthRow->addWidget(bandwidth_, 1);
    bandwidthRow->addWidget(bandwidthValue_);
    advancedForm->addRow("Video bandwidth", bandwidthWidget);

    horizontalOffset_ = new QSlider(Qt::Horizontal, advancedPage);
    horizontalOffset_->setRange(-24, 24);
    horizontalOffset_->setValue(5);
    horizontalOffset_->setSingleStep(1);
    horizontalOffset_->setPageStep(4);
    horizontalOffsetValue_ = new QLabel("5 px", advancedPage);

    auto* offsetWidget = new QWidget(advancedPage);
    auto* offsetRow = new QHBoxLayout(offsetWidget);
    offsetRow->setContentsMargins(0, 6, 0, 6);
    offsetWidget->setMinimumHeight(48);
    offsetRow->addWidget(horizontalOffset_, 1);
    offsetRow->addWidget(horizontalOffsetValue_);
    advancedForm->addRow("Horizontal offset", offsetWidget);

    audioBuffer_ = new QSlider(Qt::Horizontal, advancedPage);
    audioBufferValue_ = new QLabel(advancedPage);
    audioBuffer_->setRange(4, 100);
    audioBuffer_->setSingleStep(1);
    audioBuffer_->setPageStep(4);
    audioBuffer_->setTickPosition(QSlider::TicksBelow);
    audioBuffer_->setTickInterval(4);
    audioBuffer_->setToolTip(
        "ASRC target buffer. The automatic default is 4 x the ASIO driver buffer duration, rounded to whole milliseconds. "
        "The controller thresholds scale with the ASIO buffer quantum. Use a larger target only when extra transport reserve is needed.");
    audioBufferValue_->setMinimumWidth(190);

    auto* audioBufferWidget = new QWidget(advancedPage);
    auto* audioBufferRow = new QHBoxLayout(audioBufferWidget);
    audioBufferRow->setContentsMargins(0, 6, 0, 6);
    audioBufferWidget->setMinimumHeight(48);
    audioBufferRow->addWidget(audioBuffer_, 1);
    audioBufferRow->addWidget(audioBufferValue_);

    audioBufferLabel_ = new QLabel("ASRC target buffer", advancedPage);
    advancedForm->addRow(audioBufferLabel_, audioBufferWidget);

    asioBufferInfo_ = new QLabel(QStringLiteral("waiting for ASIO driver"), advancedPage);
    asioBufferInfo_->setToolTip(QStringLiteral(
        "This is the buffer quantum supplied by the ASIO driver. Lower end-to-end latency requires a smaller ASIO driver buffer; change it in the driver's own control panel."));
    advancedForm->addRow(QStringLiteral("ASIO driver buffer"), asioBufferInfo_);

    audioLatencyInfo_ = new QLabel(advancedPage);
    audioLatencyInfo_->setText(QStringLiteral(
        "Blackmagic PAL frame latency +40 ms  |  Total TX latency 44 ms"));
    audioLatencyInfo_->setToolTip(QStringLiteral(
        "ASRC buffer is the adjustable part. The additional 40 ms is one fixed PAL frame in the Blackmagic output path."));
    advancedForm->addRow(QStringLiteral("TX latency"), audioLatencyInfo_);

    auto* hamMappingInfo = new QLabel(
        QStringLiteral(
            "Measured direct 1:1 OpenScope/Intensity row mapping: "
            "271 data rows per field, text rows 260..291 and black rows 574/575."),
        advancedPage);
    hamMappingInfo->setWordWrap(true);
    advancedForm->addRow(QStringLiteral("Ham PCM 2.0 mapping"), hamMappingInfo);

    advancedLayout->addLayout(advancedForm);

    auto* advancedDefaults = new QPushButton("Defaults", advancedPage);
    advancedDefaults->setToolTip(
        "Restore advanced video defaults: 5.0 MHz bandwidth and 5 px horizontal offset.");
    advancedLayout->addWidget(advancedDefaults, 0, Qt::AlignLeft);
    advancedLayout->addStretch();

    auto* debugPage = new QWidget(tabs);
    auto* debugForm = new QFormLayout(debugPage);
    debugForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto addDebugRow = [&](const QString& name, QLabel*& value)
    {
        value = new QLabel("-", debugPage);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        debugForm->addRow(name, value);
    };


    addDebugRow("Video frames", dbgVideoFrames_);
    addDebugRow("ASIO buffer capabilities", dbgAsioBufferCaps_);

    auto* txMarginButton = new QPushButton(QStringLiteral("Open graph"), debugPage);
    debugForm->addRow("TX margin", txMarginButton);

    txMarginFloaty_ = new QDialog(this);
    txMarginFloaty_->setWindowTitle(QStringLiteral("JIT Completion Margin / Buffer Nudge"));
    txMarginFloaty_->setWindowFlags(
        Qt::Window |
        Qt::WindowMinMaxButtonsHint |
        Qt::WindowCloseButtonHint);
    txMarginFloaty_->resize(1200, 700);

    auto* txMarginFloatyLayout = new QVBoxLayout(txMarginFloaty_);
    txMarginFloatyLayout->setContentsMargins(8, 8, 8, 8);

    dbgTxMarginGraph_ = new TxMarginGraph(txMarginFloaty_);
    dbgTxMarginGraph_->setMinimumSize(500, 300);
    txMarginFloatyLayout->addWidget(dbgTxMarginGraph_, 1);

    dbgTxMarginValue_ = new QLabel(QStringLiteral("00.00 ms"), txMarginFloaty_);
    dbgTxMarginValue_->setVisible(false);

    connect(txMarginButton, &QPushButton::clicked, this, [this]()
    {
        txMarginFloaty_->show();
        txMarginFloaty_->raise();
        txMarginFloaty_->activateWindow();
    });

    auto* debugLogButton = new QPushButton(QStringLiteral("Open log"), debugPage);
    debugForm->addRow("Buffer nudge event log", debugLogButton);

    debugLogFloaty_ = new QDialog(this);
    debugLogFloaty_->setWindowTitle(QStringLiteral("ASRC Buffer Nudge Debug Log"));
    debugLogFloaty_->setWindowFlags(
        Qt::Window |
        Qt::WindowMinMaxButtonsHint |
        Qt::WindowCloseButtonHint);
    debugLogFloaty_->resize(1100, 650);

    auto* debugLogLayout = new QVBoxLayout(debugLogFloaty_);
    debugLogLayout->setContentsMargins(8, 8, 8, 8);

    debugLogText_ = new QPlainTextEdit(debugLogFloaty_);
    debugLogText_->setReadOnly(true);
    debugLogText_->setLineWrapMode(QPlainTextEdit::NoWrap);
    debugLogText_->setMaximumBlockCount(5000);
    debugLogText_->setStyleSheet(
        QStringLiteral("QPlainTextEdit { font-family: Consolas, 'Courier New', monospace; font-size: 10pt; }"));
    debugLogLayout->addWidget(debugLogText_, 1);

    auto* debugLogButtons = new QHBoxLayout();
    auto* clearDebugLog = new QPushButton(QStringLiteral("Clear"), debugLogFloaty_);
    auto* copyDebugLog = new QPushButton(QStringLiteral("Copy all"), debugLogFloaty_);
    debugLogButtons->addWidget(clearDebugLog);
    debugLogButtons->addWidget(copyDebugLog);
    debugLogButtons->addStretch();
    debugLogLayout->addLayout(debugLogButtons);

    connect(debugLogButton, &QPushButton::clicked, this, [this]()
    {
        debugLogFloaty_->show();
        debugLogFloaty_->raise();
        debugLogFloaty_->activateWindow();
    });
    connect(clearDebugLog, &QPushButton::clicked, debugLogText_, &QPlainTextEdit::clear);
    connect(copyDebugLog, &QPushButton::clicked, this, [this]()
    {
        debugLogText_->selectAll();
        debugLogText_->copy();
        QTextCursor cursor = debugLogText_->textCursor();
        cursor.clearSelection();
        debugLogText_->setTextCursor(cursor);
    });

    addDebugRow("Latency budget", dbgBuffer_);
    addDebugRow("Applied ASRC nudge", dbgAppliedTrim_);
    addDebugRow("Audio underruns", dbgUnderruns_);

    addDebugRow("Worst JIT margin", dbgWorstJit_);
    addDebugRow("BMD schedule failures", dbgBmdSubmitFailures_);
    addDebugRow("BMD scheduled callbacks", dbgBmdCallbacks_);
    addDebugRow("BMD displayed late", dbgBmdLate_);
    addDebugRow("BMD dropped frames", dbgBmdDropped_);
    addDebugRow("BMD flushed frames", dbgBmdFlushed_);
    addDebugRow("JIT missed boundaries", dbgJitMissedBoundary_);
    addDebugRow("PCM horizontal offset", dbgPcmX_);
    addDebugRow("Video filter", dbgFilter_);
    addDebugRow("PCM mode", dbgMode_);
    addDebugRow("ASRC output", dbgAsrcOut_);
    addDebugRow("Pre-emphasis", dbgPreEmphasis_);

    tabs->addTab(mainPage, "Encoder");
    tabs->addTab(advancedPage, "Advanced");
    tabs->addTab(debugPage, "Debug");

    mainStack_->addWidget(tabs_);
    mainStack_->addWidget(meterPage_);
    mainStack_->setCurrentWidget(tabs_);
    setCentralWidget(root);

    // Windowed Encoder view is deliberately fixed so the MUDTW front always
    // keeps the tuned proportions. F11 temporarily releases this constraint.
    setFixedSize(1050, 430);

    connect(
        start_,
        &QPushButton::clicked,
        this,
        &MainWindow::toggle);


    connect(
        bandwidth_,
        &QSlider::valueChanged,
        this,
        [this](int value)
        {
            const double mhz =
                static_cast<double>(value) /
                10.0;

            encoder_.setVideoBandwidthMHz(mhz);
            hamEncoder_.setVideoBandwidthMHz(mhz);

            bandwidthValue_->setText(QString::number(mhz, 'f', 1) + " MHz");
            QSettings settings("OpenScope", "OpenScopePcmEncoder");
            settings.setValue("video/bandwidthTenthsMHz", value);
        });

    connect(
        horizontalOffset_,
        &QSlider::valueChanged,
        this,
        [this](int pixels)
        {
            encoder_.setHorizontalOffsetPixels(pixels);
            hamEncoder_.setHorizontalOffsetPixels(pixels);

            horizontalOffsetValue_->setText(QString::number(pixels) + " px");
            QSettings settings("OpenScope", "OpenScopePcmEncoder");
            settings.setValue("video/horizontalOffset", pixels);
        });

    connect(
        advancedDefaults,
        &QPushButton::clicked,
        this,
        [this]()
        {
            bandwidth_->setValue(50);
            horizontalOffset_->setValue(5);

            // valueChanged handlers apply the values to both encoders and
            // persist them, so one button really restores the complete
            // Advanced video setup.
        });

    connect(mode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int index)
        {
            const bool ham = index == 2;
            if (!ham)
                encoder_.set16BitMode(index == 0);
            capture_.setHamMode(ham);
            output_.setHamMode(ham);
            hamText_->setEnabled(ham);
            preEmphasis_->setEnabled(!ham);
            applyPreEmphasisPolicy();
            QSettings settings("OpenScope", "OpenScopePcmEncoder");
            settings.setValue("pcm/format", std::clamp(index, 0, 2));
        });

    connect(hamText_, &QLineEdit::textChanged, this,
        [this](const QString& value)
        {
            hamEncoder_.setText(value.toStdString());
            QSettings settings("OpenScope", "OpenScopePcmEncoder");
            settings.setValue("pcm/hamText2", value);
        });

    connect(preEmphasis_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int index)
        {
            QSettings settings("OpenScope", "OpenScopePcmEncoder");
            settings.setValue("audio/preEmphasisMode", std::clamp(index, 0, 2));
            applyPreEmphasisPolicy();
        });

    // While dragging, only preview the requested target in the UI.
    // The buffer controller must see one atomic target change, so the active
    // target is committed only when the user releases the slider.
    connect(audioBuffer_, &QSlider::valueChanged, this,
        [this](int ms)
        {
            if (dbgTxMarginGraph_ != nullptr)
                dbgTxMarginGraph_->setTargetMargin(static_cast<double>(ms));

            // Show the prospective value while dragging. The controller still
            // receives one atomic target change only on sliderReleased.
            if (audioBufferValue_ != nullptr)
            {
                const double actualMs = capture_.txAverageMarginMs();
                audioBufferValue_->setText(
                    QStringLiteral("target %1 ms | actual %2 ms")
                        .arg(ms)
                        .arg(actualMs, 0, 'f', 1));
            }

            if (audioLatencyInfo_ != nullptr)
                audioLatencyInfo_->setText(
                    QStringLiteral("Blackmagic PAL frame latency +40 ms  |  Total TX latency %1 ms")
                        .arg(40 + ms));
        });

    connect(audioBuffer_, &QSlider::sliderReleased, this,
        [this]()
        {
            const int ms = audioBuffer_->value();

            capture_.setBufferMs(ms);

            QSettings settings("OpenScope", "OpenScopePcmEncoder");
            settings.setValue("audio/jitBufferMs", ms);
        });

    encoder_.setPulseShapingEnabled(true);
    encoder_.setVideoBandwidthMHz(5.0);
    encoder_.setHorizontalOffsetPixels(5);

    QSettings startupSettings(
        "OpenScope",
        "OpenScopePcmEncoder");

    int savedFormat = 0;
    if (startupSettings.contains("pcm/format"))
        savedFormat = std::clamp(startupSettings.value("pcm/format", 0).toInt(), 0, 2);
    else
        savedFormat = startupSettings.value("pcm/mode16", true).toBool() ? 0 : 1;
    const QString savedHamText = startupSettings.value("pcm/hamText2", "OPEN SCOPE").toString().left(10);
    int savedPreMode = 0;
    if (startupSettings.contains("audio/preEmphasisMode"))
        savedPreMode = std::clamp(startupSettings.value("audio/preEmphasisMode", 0).toInt(), 0, 2);
    else if (startupSettings.contains("audio/preEmphasis"))
        savedPreMode = startupSettings.value("audio/preEmphasis", false).toBool() ? 2 : 1;
    const int savedBw = std::clamp(startupSettings.value("video/bandwidthTenthsMHz", 50).toInt(), 10, 60);
    const int savedOffset = std::clamp(startupSettings.value("video/horizontalOffset", 5).toInt(), -24, 24);
    int savedBufferMsRaw = 4;
    if (startupSettings.contains("audio/jitBufferMs"))
        savedBufferMsRaw = startupSettings.value("audio/jitBufferMs", 4).toInt();
    else if (startupSettings.contains("audio/asrcBufferMs"))
        savedBufferMsRaw = std::max(0, startupSettings.value("audio/asrcBufferMs", 44).toInt() - 40);
    const int savedBufferMs = std::clamp(savedBufferMsRaw, 4, 100);

    mode_->setCurrentIndex(savedFormat);
    hamText_->setText(savedHamText);
    preEmphasis_->setCurrentIndex(savedPreMode);
    bandwidth_->setValue(savedBw);
    horizontalOffset_->setValue(savedOffset);
    audioBuffer_->setValue(savedBufferMs);

    if (savedFormat != 2)
        encoder_.set16BitMode(savedFormat == 0);
    hamEncoder_.setText(savedHamText.toStdString());
    capture_.setHamMode(savedFormat == 2);
    applyPreEmphasisPolicy();
    encoder_.setPulseShapingEnabled(true);
    encoder_.setVideoBandwidthMHz(savedBw / 10.0);
    encoder_.setHorizontalOffsetPixels(savedOffset);
    hamEncoder_.setPulseShapingEnabled(true);
    hamEncoder_.setVideoBandwidthMHz(savedBw / 10.0);
    hamEncoder_.setHorizontalOffsetPixels(savedOffset);
    hamText_->setEnabled(savedFormat == 2);
    preEmphasis_->setEnabled(savedFormat != 2);
    const int startupAudioBufferMs = savedBufferMs;
    capture_.setBufferMs(startupAudioBufferMs);
    audioBufferValue_->setText(QStringLiteral("target -- ms | filtered --.- ms"));
    audioLatencyInfo_->setText(
        QStringLiteral("Blackmagic PAL frame latency +40 ms  |  Total TX latency %1 ms")
            .arg(40 + startupAudioBufferMs));

    timer_ = new QTimer(this);

    connect(
        timer_,
        &QTimer::timeout,
        this,
        &MainWindow::tick);

    timer_->start(25);

    refresh();

    connect(
        audio_,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int index)
        {
            if (index < 0 ||
                index >= static_cast<int>(audioDevices_.size()))
            {
                return;
            }

            QSettings settings(
                "OpenScope",
                "OpenScopePcmEncoder");

            settings.setValue(
                "audio/deviceId",
                QString::fromStdWString(
                    audioDevices_[index].id));

            settings.sync();
        });

    connect(
        video_,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int index)
        {
            if (index < 0 ||
                index >= static_cast<int>(videoDevices_.size()) ||
                videoDevices_[index].busy)
            {
                return;
            }

            QSettings settings(
                "OpenScope",
                "OpenScopePcmEncoder");

            settings.setValue(
                "video/deviceName",
                QString::fromStdWString(
                    videoDevices_[index].name));

            settings.setValue(
                "video/deviceIndex",
                videoDevices_[index].index);

            settings.sync();
        });
}

void MainWindow::applyPreEmphasisPolicy()
{
    const int policy = preEmphasis_ != nullptr
        ? std::clamp(preEmphasis_->currentIndex(), 0, 2)
        : 0;

    const bool ham = mode_ != nullptr && mode_->currentIndex() == 2;
    const bool enabled = !ham &&
        (policy == 2 || (policy == 0 && !encoder_.is16BitMode()));

    encoder_.setPreEmphasisEnabled(enabled);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (event == nullptr)
        return;

    if (output_.running())
    {
        const auto answer =
            QMessageBox::warning(
                this,
                QStringLiteral("PCM Encoder is ON-AIR"),
                QStringLiteral(
                    "The PCM Encoder is still ON-AIR.\n\n"
                    "Do you really want to close it?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);

        if (answer != QMessageBox::Yes)
        {
            event->ignore();
            return;
        }
    }

    event->accept();
}

MainWindow::~MainWindow()
{
    // Output consumes capture data, so first stop/join the DeckLink worker.
    // Only then tear down ASIO.
    output_.stop();
    capture_.stop();
}

void MainWindow::refresh()
{
    const QSignalBlocker audioBlocker(audio_);
    const QSignalBlocker videoBlocker(video_);

    audio_->clear();
    video_->clear();

    audioDevices_ = AsioCapture::enumerate();
    videoDevices_ = DeckLinkPalOutput::enumerate();

    for (const auto& device : audioDevices_)
        audio_->addItem(QString::fromStdWString(device.name));

    for (const auto& device : videoDevices_)
    {
        const QString name =
            QString::fromStdWString(device.name) +
            (device.busy ? QStringLiteral(" [BUSY]") : QString());

        video_->addItem(name);
    }

    if (auto* model =
            qobject_cast<QStandardItemModel*>(video_->model()))
    {
        for (int i = 0;
             i < static_cast<int>(videoDevices_.size());
             ++i)
        {
            if (videoDevices_[i].busy)
            {
                if (QStandardItem* item = model->item(i))
                {
                    item->setEnabled(false);
                    item->setSelectable(false);
                }
            }
        }
    }

    QSettings settings(
        "OpenScope",
        "OpenScopePcmEncoder");

    const QString savedAudioDeviceId =
        settings.value(
            "audio/deviceId").toString();

    if (!savedAudioDeviceId.isEmpty())
    {
        for (int i = 0;
             i < static_cast<int>(audioDevices_.size());
             ++i)
        {
            if (QString::fromStdWString(
                    audioDevices_[i].id) ==
                savedAudioDeviceId)
            {
                audio_->setCurrentIndex(i);
                break;
            }
        }
    }

    const QString savedVideoDeviceName =
        settings.value("video/deviceName").toString();

    const int savedVideoDeviceIndex =
        settings.value("video/deviceIndex", -1).toInt();

    bool restoredVideoDevice = false;

    if (!savedVideoDeviceName.isEmpty())
    {
        for (int i = 0;
             i < static_cast<int>(videoDevices_.size());
             ++i)
        {
            if (!videoDevices_[i].busy &&
                QString::fromStdWString(
                    videoDevices_[i].name) ==
                    savedVideoDeviceName)
            {
                video_->setCurrentIndex(i);
                restoredVideoDevice = true;
                break;
            }
        }
    }

    if (!restoredVideoDevice &&
        savedVideoDeviceIndex >= 0)
    {
        for (int i = 0;
             i < static_cast<int>(videoDevices_.size());
             ++i)
        {
            if (!videoDevices_[i].busy &&
                videoDevices_[i].index == savedVideoDeviceIndex)
            {
                video_->setCurrentIndex(i);
                restoredVideoDevice = true;
                break;
            }
        }
    }

    if (!restoredVideoDevice)
    {
        for (int i = 0;
             i < static_cast<int>(videoDevices_.size());
             ++i)
        {
            if (!videoDevices_[i].busy)
            {
                video_->setCurrentIndex(i);
                restoredVideoDevice = true;
                break;
            }
        }
    }

    const bool haveFreeVideoDevice =
        std::any_of(
            videoDevices_.begin(),
            videoDevices_.end(),
            [](const DeckLinkDeviceInfo& device)
            {
                return !device.busy;
            });

    start_->setEnabled(
        !audioDevices_.empty() &&
        haveFreeVideoDevice);
}

void MainWindow::updateOnAirIndicator(bool onAir)
{
    if (onAirIndicatorState_ == onAir)
        return;

    onAirIndicatorState_ = onAir;

    if (start_)
    {
        if (onAir)
        {
            start_->setText("ON-AIR");
            start_->setStyleSheet(
                "QPushButton {"
                " background-color: rgb(190, 0, 0);"
                " color: white;"
                " font-weight: bold;"
                "}");
        }
        else
        {
            start_->setText("Start");
            start_->setStyleSheet(QString());
        }
    }
}

void MainWindow::syncMeterOnlyGeometry()
{
    if (meterOnlyInstrument_ == nullptr ||
        meterOnlyL_ == nullptr ||
        meterOnlyScale_ == nullptr ||
        meterOnlyR_ == nullptr ||
        meterPage_ == nullptr)
    {
        return;
    }

    constexpr double kFrontAspect = 5.35;
    constexpr int kOuterMarginX = 72;
    constexpr int kOuterMarginY = 21;
    constexpr int kInnerTop = 8;
    constexpr int kInnerBottom = 8;
    constexpr int kInnerSpacing = 1;

    const int availableWidth =
        std::max(420, meterPage_->width() - kOuterMarginX);
    const int availableHeight =
        std::max(180, meterPage_->height() - kOuterMarginY);

    int targetWidth = availableWidth;
    int targetHeight = static_cast<int>(std::lround(
        static_cast<double>(targetWidth) / kFrontAspect));

    if (meterOnlyFullscreen_ && targetHeight > availableHeight)
    {
        targetHeight = availableHeight;
        targetWidth = static_cast<int>(std::lround(
            static_cast<double>(targetHeight) * kFrontAspect));
    }
    else if (!meterOnlyFullscreen_)
    {
        targetHeight = availableHeight;
    }

    targetWidth = std::max(420, targetWidth);
    targetHeight = std::max(160, targetHeight);

    meterOnlyInstrument_->setFixedSize(targetWidth, targetHeight);

    // Windowed MUDTW deliberately sits near the top. In fullscreen, center
    // the complete front vertically instead of leaving it glued to the top.
    if (auto* meterOnlyLayout =
            qobject_cast<QVBoxLayout*>(meterPage_->layout()))
    {
        if (meterOnlyFullscreen_)
        {
            const int top = std::max(
                0,
                (meterPage_->height() - targetHeight) / 2);

            meterOnlyLayout->setContentsMargins(24, top, 24, 0);
        }
        else
        {
            meterOnlyLayout->setContentsMargins(24, 16, 24, 24);
        }
    }

    const int usableHeight = std::max(80,
        targetHeight - kInnerTop - kInnerBottom - 2 * kInnerSpacing);

    const bool fullscreenFront = meterOnlyFullscreen_;

    // In fullscreen the scale text/ticks need more authority. Keep the bars
    // slim, but let the whole front breathe a bit more vertically so the
    // geometry does not look miniaturized.
    const int meterHeight = std::clamp(
        static_cast<int>(std::lround(
            usableHeight * (fullscreenFront ? 0.24 : 0.22))),
        34,
        fullscreenFront ? 68 : 58);
    const int scaleHeight = std::clamp(
        static_cast<int>(std::lround(
            usableHeight * (fullscreenFront ? 0.38 : 0.28))),
        48,
        fullscreenFront ? 96 : 66);

    meterOnlyL_->setFixedHeight(meterHeight);
    meterOnlyScale_->setFixedHeight(scaleHeight);
    meterOnlyR_->setFixedHeight(meterHeight);
}

void MainWindow::enterMeterOnlyFullscreen()
{
    if (mainStack_ == nullptr || tabs_ == nullptr || meterPage_ == nullptr)
        return;

    if (!output_.running())
        return;

    restoreMaximizedAfterMeterOnlyFullscreen_ = isMaximized();
    if (!isFullScreen())
        restoreGeometryAfterMeterOnlyFullscreen_ = geometry();

    mainStack_->setCurrentWidget(meterPage_);
    meterOnlyView_ = true;
    meterOnlyFullscreen_ = true;

    // Release the fixed windowed size while presenting the pure meter.
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    showFullScreen();
    syncMeterOnlyGeometry();
}

void MainWindow::exitMeterOnlyFullscreen()
{
    meterOnlyFullscreen_ = false;
    if (isFullScreen())
        showNormal();

    if (restoreGeometryAfterMeterOnlyFullscreen_.isValid())
        setGeometry(restoreGeometryAfterMeterOnlyFullscreen_);
    if (restoreMaximizedAfterMeterOnlyFullscreen_)
        showMaximized();

    if (mainStack_ != nullptr && tabs_ != nullptr)
        mainStack_->setCurrentWidget(tabs_);
    meterOnlyView_ = false;

    // Re-apply the tuned windowed geometry after leaving F11.
    setFixedSize(1050, 430);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);

    if (meterOnlyView_ || meterOnlyFullscreen_)
        syncMeterOnlyGeometry();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event == nullptr)
        return;

    if (event->key() == Qt::Key_F11)
    {
        if (meterOnlyFullscreen_)
            exitMeterOnlyFullscreen();
        else
            enterMeterOnlyFullscreen();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Escape && meterOnlyFullscreen_)
    {
        exitMeterOnlyFullscreen();
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::toggleMeterOnlyView()
{
    if (mainStack_ == nullptr || tabs_ == nullptr || meterPage_ == nullptr)
        return;

    if (mainStack_->currentWidget() == meterPage_)
    {
        if (meterOnlyFullscreen_)
        {
            exitMeterOnlyFullscreen();
            return;
        }

        mainStack_->setCurrentWidget(tabs_);
        meterOnlyView_ = false;
        setFixedSize(1050, 430);
        return;
    }

    // Enter the clean meter-only presentation only while PCM output is
    // actually running. One click on either meter (or the scale) enters it;
    // one click in the meter-only view returns to the full application.
    if (!output_.running())
        return;

    mainStack_->setCurrentWidget(meterPage_);
    meterOnlyView_ = true;

    // Windowed MUDTW view: keep the existing meter geometry, but remove the
    // large unused black area above and below it.
    setFixedSize(1050, 220);
    syncMeterOnlyGeometry();
}

void MainWindow::toggle()
{
    if (output_.running() || asioRecoveryPending_)
    {
        start_->setEnabled(false);
        status_->setText("Stopping...");
        updateOnAirIndicator(false);
        asioRecoveryPending_ = false;
        asioRecoveryRetryTicks_ = 0;

        // Keep this order: DeckLink consumer stops before ASIO producer.
        output_.stop();
        capture_.stop();

        start_->setText("Start PCM video output");
        start_->setEnabled(true);
        mode_->setEnabled(true);
        status_->setText("Stopped");
        return;
    }

    if (audio_->currentIndex() < 0 ||
        video_->currentIndex() < 0)
    {
        return;
    }

    if (video_->currentIndex() >=
            static_cast<int>(videoDevices_.size()) ||
        videoDevices_[video_->currentIndex()].busy)
    {
        return;
    }

    const bool ham = mode_->currentIndex() == 2;
    const auto audioId = audioDevices_[audio_->currentIndex()].id;
    const int videoDeviceIndex = videoDevices_[video_->currentIndex()].index;

    capture_.setHamMode(ham);
    if (!capture_.start(audioId))
    {
        status_->setText(QString::fromStdWString(capture_.lastError()));
        start_->setText("Start PCM video output");
        mode_->setEnabled(true);
        return;
    }

    capture_.setHamMode(ham);

    // AsioQuantumLeap: derive the default transport reserve from the actual
    // driver quantum. Four quanta gives room for callback phase/jitter while
    // keeping the target proportional to the latency selected in the driver.
    const double asioQms = capture_.asioBufferMs();
    if (asioQms > 0.0)
    {
        const int quantumTargetMs = std::clamp(
            static_cast<int>(std::lround(4.0 * asioQms)), 4, 100);
        capture_.setBufferMs(quantumTargetMs);
        if (audioBuffer_ != nullptr)
            audioBuffer_->setValue(quantumTargetMs);
    }

    // Encoder type stays selectable while running.

    output_.start(
        videoDeviceIndex,
        &capture_,
        &encoder_,
        &hamEncoder_,
        ham);

    asioWatchdogLastCallbacks_ = capture_.asioCallbackCount();
    asioWatchdogStallTicks_ = 0;
    lastDiagAsioCallbacks_ = capture_.asioCallbackCount();
    lastDiagBmdCallbacks_ = output_.bmdScheduledCallbacks();
    lastDiagFramesReady_ = output_.frameReadyCount();
    diagTickDivider_ = 0;
    lastLoggedAsioFrames_ = -1;
    lastLoggedAsioPerSec_ = std::numeric_limits<std::uint64_t>::max();

    start_->setText("Stop");
    status_->setText("Starting...");
}

void MainWindow::beginAsioRecovery(const QString& reason)
{
    if (asioRecoveryPending_)
        return;

    if (audio_->currentIndex() < 0 || video_->currentIndex() < 0)
        return;

    asioRecoveryDeviceId_ = audioDevices_[audio_->currentIndex()].id;
    asioRecoveryVideoDeviceIndex_ = videoDevices_[video_->currentIndex()].index;
    asioRecoveryPending_ = true;
    asioRecoveryRetryTicks_ = 0;
    asioWatchdogStallTicks_ = 0;

    capture_.addDebugLogLine(
        "ASIO RECOVERY begin reason=\"" + reason.toStdString() + "\"");

    status_->setText("ASIO lost - reconnecting...");
    updateOnAirIndicator(false);
    start_->setText("Stop");

    // Rebuild the whole producer/consumer chain. Stopping DeckLink first keeps
    // its worker from spinning on an ASIO object that is being reinitialized.
    output_.stop();
    capture_.stop();
}

void MainWindow::tryAsioRecovery()
{
    if (!asioRecoveryPending_)
        return;

    if (asioRecoveryRetryTicks_ > 0)
    {
        --asioRecoveryRetryTicks_;
        return;
    }

    const bool ham = mode_->currentIndex() == 2;
    capture_.setHamMode(ham);
    capture_.addDebugLogLine(
        "ASIO RECOVERY attempt driver=\"" +
        QString::fromStdWString(asioRecoveryDeviceId_).toStdString() + "\"");

    if (!capture_.start(asioRecoveryDeviceId_))
    {
        const std::wstring error = capture_.lastError();
        capture_.addDebugLogLine(
            "ASIO RECOVERY failed; retry in 1 s");
        status_->setText(
            error.empty()
                ? QStringLiteral("ASIO unavailable - retrying...")
                : QStringLiteral("ASIO unavailable - retrying: %1")
                    .arg(QString::fromStdWString(error)));

        // tick() runs every 25 ms.
        asioRecoveryRetryTicks_ = 40;
        return;
    }

    capture_.setHamMode(ham);

    const double asioQms = capture_.asioBufferMs();
    if (asioQms > 0.0)
    {
        const int quantumTargetMs = std::clamp(
            static_cast<int>(std::lround(4.0 * asioQms)), 4, 100);
        capture_.setBufferMs(quantumTargetMs);
        if (audioBuffer_ != nullptr)
            audioBuffer_->setValue(quantumTargetMs);
    }

    output_.start(
        asioRecoveryVideoDeviceIndex_,
        &capture_,
        &encoder_,
        &hamEncoder_,
        ham);

    asioRecoveryPending_ = false;
    asioWatchdogLastCallbacks_ = capture_.asioCallbackCount();
    asioWatchdogStallTicks_ = 0;
    lastDiagAsioCallbacks_ = capture_.asioCallbackCount();
    lastDiagBmdCallbacks_ = output_.bmdScheduledCallbacks();
    lastDiagFramesReady_ = output_.frameReadyCount();
    diagTickDivider_ = 0;
    lastLoggedAsioFrames_ = -1;
    lastLoggedAsioPerSec_ = std::numeric_limits<std::uint64_t>::max();

    capture_.addDebugLogLine("ASIO RECOVERY success");
    status_->setText("ASIO reconnected - starting...");
}


void MainWindow::tick()
{
    if (debugLogText_ != nullptr)
    {
        const auto lines = capture_.takeDebugLogLines();
        for (const auto& line : lines)
            debugLogText_->appendPlainText(QString::fromStdString(line));
    }

    const float peakLeft = capture_.consumePeakLeft();
    const float peakRight = capture_.consumePeakRight();

    l_->setInputPeak(peakLeft);
    r_->setInputPeak(peakRight);
    if (meterOnlyL_ != nullptr)
        meterOnlyL_->setInputPeak(peakLeft);
    if (meterOnlyR_ != nullptr)
        meterOnlyR_->setInputPeak(peakRight);

    // ASIO callbacks are the definitive liveness signal. Some drivers issue
    // kAsioResetRequest when their backing engine disappears; others simply
    // stop calling us. Handle both without doing teardown inside the ASIO
    // callback thread.
    if (!asioRecoveryPending_ && output_.running())
    {
        if (capture_.consumeRestartRequested())
        {
            beginAsioRecovery(QStringLiteral("driver reset request"));
        }
        else
        {
            const std::uint64_t callbacks = capture_.asioCallbackCount();
            if (callbacks != asioWatchdogLastCallbacks_)
            {
                asioWatchdogLastCallbacks_ = callbacks;
                asioWatchdogStallTicks_ = 0;
            }
            else
            {
                ++asioWatchdogStallTicks_;
                if (asioWatchdogStallTicks_ >= 20) // 20 x 25 ms = 500 ms
                    beginAsioRecovery(QStringLiteral("no ASIO callbacks for 500 ms"));
            }
        }
    }

    if (asioRecoveryPending_)
        tryAsioRecovery();

    updateOnAirIndicator(output_.running());

    if (meterOnlyView_ && !output_.running())
    {
        if (meterOnlyFullscreen_)
            exitMeterOnlyFullscreen();
        else if (mainStack_ != nullptr && tabs_ != nullptr)
        {
            mainStack_->setCurrentWidget(tabs_);
            meterOnlyView_ = false;
        }
    }

    if (output_.running())
    {
        const bool ham = mode_->currentIndex() == 2;
        status_->setText(ham
            ? "RUNNING - Ham PCM 2.0 48 kHz / 14-bit -> PAL video"
            : (encoder_.is16BitMode()
                ? "RUNNING - PCM 16-bit -> PAL video"
                : "RUNNING - PCM 14-bit EIAJ -> PAL video"));
    }
    else if (!asioRecoveryPending_ && start_->text() == "Stop")
    {
        auto error = output_.lastError();

        if (error.empty())
            error = capture_.lastError();

        status_->setText(
            error.empty()
                ? "Stopped"
                : QString::fromStdWString(error));

        start_->setText("Start PCM video output");
        mode_->setEnabled(true);
    }

    const QString modeText = mode_->currentIndex() == 2
        ? QStringLiteral("Ham PCM 2.0")
        : (encoder_.is16BitMode() ? QStringLiteral("16-bit PCM-F1") : QStringLiteral("14-bit EIAJ"));
    const QString filterText =
        QString::number(encoder_.videoBandwidthMHz(), 'f', 1) + QStringLiteral(" MHz");
    const QString preText = encoder_.preEmphasisEnabled()
        ? QStringLiteral("ON")
        : QStringLiteral("OFF");

    const std::uint64_t underrunsNow = capture_.underruns();
    const std::uint64_t droppedNow = output_.bmdDroppedFrames();

    if (underrunsNow != lastUnderruns_ ||
        droppedNow != lastBmdDroppedForUi_)
    {
        lastUnderruns_ = underrunsNow;
        lastBmdDroppedForUi_ = droppedNow;
        underrunFlashTicks_ = 40; // 40 x 25 ms = 1 second
    }
    else if (underrunFlashTicks_ > 0)
    {
        --underrunFlashTicks_;
    }

    const bool bufferFaultFlash = underrunFlashTicks_ > 0;
    const QString asrcStyle = bufferFaultFlash
        ? QStringLiteral("color: #ff4040; font-weight: 600;")
        : QString();

    // Only the ASRC buffer indication flashes red. The latency line remains
    // informational and should not visually imply that the BMD latency changed.
    audioBufferLabel_->setStyleSheet(asrcStyle);
    audioBufferValue_->setStyleSheet(asrcStyle);

    dbgVideoFrames_->setText(QString::number(output_.frames()));
    // 0.4.04 intentionally does not estimate BMD or ASIO clock rates.
    const double txMarginMs = capture_.txMarginMs();
    const double averageMarginMs = capture_.txAverageMarginMs();
    const double targetMarginMs = static_cast<double>(capture_.bufferMs());
    const bool haveMargin = capture_.txMarginSequence() != 0;

    if (audioBufferValue_ != nullptr)
    {
        int displayedTargetMs = capture_.bufferMs();
        if (audioBuffer_ != nullptr && audioBuffer_->isSliderDown())
        {
            displayedTargetMs = audioBuffer_->value();
        }

        audioBufferValue_->setText(
            haveMargin
                ? QStringLiteral("target %1 ms | actual %2 ms")
                    .arg(displayedTargetMs)
                    .arg(averageMarginMs, 0, 'f', 1)
                : QStringLiteral("target %1 ms | waiting")
                    .arg(displayedTargetMs));
        audioBufferValue_->setToolTip(
            QStringLiteral("AsioQuantumLeap: buffer control thresholds are expressed in ASIO quanta (Q), so the regulation scales with the driver buffer size. Settled drift beyond 3Q re-arms acquisition. No slope, PID or clock learning."));
    }

    if (asioBufferInfo_ != nullptr)
    {
        const long asioFrames = capture_.asioBufferFrames();
        const double asioQms = capture_.asioBufferMs();
        asioBufferInfo_->setText(
            asioFrames > 0 && asioQms > 0.0
                ? QStringLiteral("%1 samples / %2 ms   (default target %3 ms = 4Q)")
                    .arg(asioFrames)
                    .arg(asioQms, 0, 'f', 3)
                    .arg(static_cast<int>(std::lround(4.0 * asioQms)))
                : QStringLiteral("waiting for ASIO driver"));
    }

    dbgAsioBufferCaps_->setText(
        QStringLiteral("min %1  max %2  preferred %3  granularity %4")
            .arg(capture_.asioBufferMinFrames())
            .arg(capture_.asioBufferMaxFrames())
            .arg(capture_.asioBufferPreferredFrames())
            .arg(capture_.asioBufferGranularity()));

    dbgTxMarginGraph_->setTargetMargin(targetMarginMs);
    const std::uint64_t txSeq = capture_.txMarginSequence();
    if (haveMargin)
    {
        dbgTxMarginGraph_->setLiveMargin(averageMarginMs);
        if (txSeq != lastTxMarginSequence_)
        {
            lastTxMarginSequence_ = txSeq;
            dbgTxMarginGraph_->appendSample(
                capture_.txMarginMs(),
                capture_.txContinuousAverageBufferMs(),
                capture_.txSlowAverageMargin3sMs(),
                capture_.asrcNudgePpm(),
                capture_.asrcActualStep());

            dbgTxMarginValue_->setText(
                QStringLiteral("%1 ms")
                    .arg(averageMarginMs, 5, 'f', 2, QLatin1Char('0')));
        }
    }
    else
    {
        lastTxMarginSequence_ = txSeq;
        dbgTxMarginValue_->setText(QStringLiteral("OFF"));
    }

    dbgBuffer_->setText(
        QStringLiteral("%1 ms ASRC buffer + 40 ms BMD = %2 ms total")
            .arg(capture_.bufferMs())
            .arg(capture_.bufferMs() + 40));
    dbgBuffer_->setStyleSheet(asrcBufferAlertTicks_ > 0
        ? QStringLiteral("color: rgb(255,80,80); font-weight: 600;")
        : QString());

    dbgAppliedTrim_->setText(
        haveMargin
            ? QString::number(capture_.asrcAppliedPpm(), 'f', 0) + QStringLiteral(" ppm")
            : QStringLiteral("OFF"));

    const double worstJitMs = output_.worstJitMarginMs();
    dbgWorstJit_->setText(worstJitMs > 1.0e8
        ? QStringLiteral("-")
        : QString::number(worstJitMs, 'f', 3) + QStringLiteral(" ms"));
    dbgUnderruns_->setText(QString::number(underrunsNow));
    dbgBmdSubmitFailures_->setText(QString::number(output_.bmdSubmitFailures()));
    const std::uint64_t asioCallbacksNow = capture_.asioCallbackCount();
    const std::uint64_t bmdCallbacksNow = output_.bmdScheduledCallbacks();
    const std::uint64_t framesReadyNow = output_.frameReadyCount();

    ++diagTickDivider_;
    if (diagTickDivider_ >= 40)
    {
        diagTickDivider_ = 0;

        const auto asioPerSec = asioCallbacksNow - lastDiagAsioCallbacks_;
        const auto bmdPerSec = bmdCallbacksNow - lastDiagBmdCallbacks_;
        const auto readyPerSec = framesReadyNow - lastDiagFramesReady_;

        lastDiagAsioCallbacks_ = asioCallbacksNow;
        lastDiagBmdCallbacks_ = bmdCallbacksNow;
        lastDiagFramesReady_ = framesReadyNow;

        dbgBmdCallbacks_->setToolTip(
            QStringLiteral("Last ~1 s: ASIO callbacks %1 | BMD callbacks %2 | frames ready %3")
                .arg(asioPerSec)
                .arg(bmdPerSec)
                .arg(readyPerSec));

        if (capture_.running() && !asioRecoveryPending_)
        {
            const long frames = capture_.asioBufferFrames();
            const bool first = lastLoggedAsioPerSec_ == std::numeric_limits<std::uint64_t>::max();
            const std::uint64_t delta = first
                ? 0
                : (asioPerSec > lastLoggedAsioPerSec_
                    ? asioPerSec - lastLoggedAsioPerSec_
                    : lastLoggedAsioPerSec_ - asioPerSec);
            const std::uint64_t threshold = first
                ? 0
                : std::max<std::uint64_t>(5, lastLoggedAsioPerSec_ / 20);

            if (first || frames != lastLoggedAsioFrames_ || delta > threshold)
            {
                std::ostringstream os;
                os << "ASIO CALLBACK RATE " << asioPerSec << "/s"
                   << " bufferFrames=" << frames;
                if (frames > 0)
                {
                    os << " expected="
                       << std::fixed << std::setprecision(2)
                       << (48000.0 / static_cast<double>(frames))
                       << "/s";
                }
                capture_.addDebugLogLine(os.str());
                lastLoggedAsioPerSec_ = asioPerSec;
                lastLoggedAsioFrames_ = frames;
            }
        }
    }

    dbgBmdCallbacks_->setText(QString::number(bmdCallbacksNow));
    dbgBmdLate_->setText(QString::number(output_.bmdDisplayedLate()));
    dbgBmdDropped_->setText(QString::number(output_.bmdDroppedFrames()));
    dbgBmdFlushed_->setText(QString::number(output_.bmdFlushedFrames()));
    dbgJitMissedBoundary_->setText(QString::number(output_.jitMissedBoundaries()));
    dbgPcmX_->setText(QString::number(encoder_.horizontalOffsetPixels()) + QStringLiteral(" px"));
    dbgFilter_->setText(filterText);
    dbgMode_->setText(modeText);
    dbgAsrcOut_->setText(QString::number(capture_.outputSampleRate(), 'f', 3) + QStringLiteral(" Hz fixed"));
    dbgPreEmphasis_->setText(preText);
}
