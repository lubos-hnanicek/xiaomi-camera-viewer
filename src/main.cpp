#include <windows.h>

#include "app/App.h"

// The process is windowed, so there is no console to inherit and no stdout to
// write to; diagnostics go to the log file and the in-app log pane instead.
int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int showCommand) {
    xv::App app;
    return app.run(instance, showCommand);
}
