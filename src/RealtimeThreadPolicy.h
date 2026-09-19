#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <avrt.h>

class RealtimeThreadPolicy
{
public:
    explicit RealtimeThreadPolicy(const wchar_t* mmcssTask)
    {
        // Do not let Windows treat an encoder realtime thread as background
        // work merely because the visible Qt window has been minimized.
        THREAD_POWER_THROTTLING_STATE throttling{};
        throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        throttling.StateMask = 0;
        SetThreadInformation(
            GetCurrentThread(),
            ThreadPowerThrottling,
            &throttling,
            sizeof(throttling));

        DWORD taskIndex = 0;
        mmcssHandle_ = AvSetMmThreadCharacteristicsW(mmcssTask, &taskIndex);
        if (mmcssHandle_ != nullptr)
            AvSetMmThreadPriority(mmcssHandle_, AVRT_PRIORITY_HIGH);

        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }

    ~RealtimeThreadPolicy()
    {
        if (mmcssHandle_ != nullptr)
            AvRevertMmThreadCharacteristics(mmcssHandle_);
    }

    RealtimeThreadPolicy(const RealtimeThreadPolicy&) = delete;
    RealtimeThreadPolicy& operator=(const RealtimeThreadPolicy&) = delete;

private:
    HANDLE mmcssHandle_ = nullptr;
};
