#pragma once

#include <QMainWindow>
#include <cstdint>
#include <limits>
#include <QRect>

#include "DeckLinkPalOutput.h"
#include "Pcm16VideoEncoder.h"
#include "HamPcmV2Encoder.h"
#include "AsioCapture.h"

class QCloseEvent;
class QComboBox;
class QDialog;
class QCheckBox;
class QLabel;
class QLineEdit;
class PpmMeter;
class PpmScale;
class QPushButton;
class QProgressBar;
class QPlainTextEdit;
class TxMarginGraph;
class QSlider;
class QString;
class QTimer;
class QTabWidget;
class QStackedWidget;
class QResizeEvent;
class QKeyEvent;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

private slots:
    void refresh();
    void toggle();
    void tick();

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QComboBox* audio_ = nullptr;
    QComboBox* video_ = nullptr;
    QComboBox* mode_ = nullptr;
    QLineEdit* hamText_ = nullptr;
    QPushButton* start_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* stats_ = nullptr;
    QLabel* dbgVideoFrames_ = nullptr;
    QLabel* dbgAsioBufferCaps_ = nullptr;
    TxMarginGraph* dbgTxMarginGraph_ = nullptr;
    QLabel* dbgTxMarginValue_ = nullptr;
    QDialog* txMarginFloaty_ = nullptr;
    QDialog* debugLogFloaty_ = nullptr;
    QPlainTextEdit* debugLogText_ = nullptr;
    QLabel* dbgBuffer_ = nullptr;
    QLabel* dbgAppliedTrim_ = nullptr;
    QLabel* dbgUnderruns_ = nullptr;
    QLabel* dbgWorstJit_ = nullptr;
    QLabel* dbgBmdSubmitFailures_ = nullptr;
    QLabel* dbgBmdCallbacks_ = nullptr;
    QLabel* dbgBmdLate_ = nullptr;
    QLabel* dbgBmdDropped_ = nullptr;
    QLabel* dbgBmdFlushed_ = nullptr;
    QLabel* dbgJitMissedBoundary_ = nullptr;
    QLabel* dbgPcmX_ = nullptr;
    QLabel* dbgFilter_ = nullptr;
    QLabel* dbgMode_ = nullptr;
    QLabel* dbgAsrcOut_ = nullptr;
    QLabel* dbgPreEmphasis_ = nullptr;
    QComboBox* preEmphasis_ = nullptr;
    QSlider* bandwidth_ = nullptr;
    QLabel* bandwidthValue_ = nullptr;
    QSlider* horizontalOffset_ = nullptr;
    QLabel* horizontalOffsetValue_ = nullptr;
    PpmMeter* l_ = nullptr;
    PpmScale* ppmScale_ = nullptr;
    PpmMeter* r_ = nullptr;
    PpmMeter* meterOnlyL_ = nullptr;
    PpmScale* meterOnlyScale_ = nullptr;
    PpmMeter* meterOnlyR_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QStackedWidget* mainStack_ = nullptr;
    QWidget* meterPage_ = nullptr;
    QWidget* meterOnlyInstrument_ = nullptr;
    QWidget* meterPanel_ = nullptr;
    bool meterOnlyView_ = false;
    bool meterOnlyFullscreen_ = false;
    bool restoreMaximizedAfterMeterOnlyFullscreen_ = false;
    QRect restoreGeometryAfterMeterOnlyFullscreen_;
    QSlider* audioBuffer_ = nullptr;
    QLabel* audioBufferLabel_ = nullptr;
    QLabel* audioBufferValue_ = nullptr;
    QLabel* asioBufferInfo_ = nullptr;
    QLabel* audioLatencyInfo_ = nullptr;
    std::uint64_t lastUnderruns_ = 0;
    std::uint64_t lastTxMarginSequence_ = 0;
    int underrunFlashTicks_ = 0;
    std::uint64_t lastBmdDroppedForUi_ = 0;
    std::uint64_t lastDiagAsioCallbacks_ = 0;
    std::uint64_t lastDiagBmdCallbacks_ = 0;
    std::uint64_t lastDiagFramesReady_ = 0;
    int diagTickDivider_ = 0;
    std::uint64_t asioWatchdogLastCallbacks_ = 0;
    int asioWatchdogStallTicks_ = 0;
    bool asioRecoveryPending_ = false;
    bool onAirIndicatorState_ = true; // force first updateOnAirIndicator(false)
    int asioRecoveryRetryTicks_ = 0;
    std::wstring asioRecoveryDeviceId_;
    int asioRecoveryVideoDeviceIndex_ = -1;
    long lastLoggedAsioFrames_ = -1;
    std::uint64_t lastLoggedAsioPerSec_ = std::numeric_limits<std::uint64_t>::max();
    QTimer* timer_ = nullptr;

    void applyPreEmphasisPolicy();
    void updateOnAirIndicator(bool onAir);
    void toggleMeterOnlyView();
    void syncMeterOnlyGeometry();
    void enterMeterOnlyFullscreen();
    void exitMeterOnlyFullscreen();
    void beginAsioRecovery(const QString& reason);
    void tryAsioRecovery();

    std::vector<AudioInputDevice> audioDevices_;
    std::vector<DeckLinkDeviceInfo> videoDevices_;

    AsioCapture capture_;
    DeckLinkPalOutput output_;
    Pcm16VideoEncoder encoder_;
    HamPcmV2Encoder hamEncoder_;
    std::uint64_t lastAudioUnderrunsForUi_ = 0;
    int asrcBufferAlertTicks_ = 0;
};
