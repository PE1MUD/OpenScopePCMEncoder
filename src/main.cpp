#include <Windows.h>
#include <QApplication>
#include "MainWindow.h"

namespace
{
void configureRealtimeProcessPolicy()
{
    // Keep realtime audio timing intact even when the GUI is minimized or
    // fully occluded. Windows 11 may otherwise ignore high-resolution timer
    // requests for window-owning processes in that state.
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask =
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
        PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;

    SetProcessInformation(
        GetCurrentProcess(),
        ProcessPowerThrottling,
        &throttling,
        sizeof(throttling));

    // Keep normal process priority; the timing policy above is sufficient.
}
} // namespace

int main(int argc,char** argv){
    configureRealtimeProcessPolicy();
QApplication app(argc,argv);MainWindow w;w.show();return app.exec();}
