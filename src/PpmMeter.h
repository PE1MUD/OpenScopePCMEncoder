#pragma once

#include <QElapsedTimer>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;
class QTimer;

class PpmMeter final : public QWidget
{
    Q_OBJECT

public:
    explicit PpmMeter(const QString& channelName = QString(),
                      QWidget* parent = nullptr);

    void setInputPeak(float linearPeak);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void animateBallistics();
    static double linearToDb(double linear);

    QTimer* animationTimer_ = nullptr;
    QElapsedTimer elapsed_;
    double targetDb_ = -60.0;
    double displayedDb_ = -60.0;
    double peakHoldDb_ = -60.0;
    double peakHoldSecondsRemaining_ = 0.0;
};

class PpmScale final : public QWidget
{
    Q_OBJECT

public:
    explicit PpmScale(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
};
