#include "branding.hpp"
#include "joy_ipc.hpp"
#include "tray_visibility.hpp"
#include "tray_caption.hpp"
#include "settings_window.hpp"
#include <memory>
#include <shellapi.h>
#include <commctrl.h>
#include <bit>
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace ipc = switch2::joy_ipc;
namespace {
constexpr UINT TrayMessage = WM_APP + 1, ShowChoice = WM_APP + 2, ShowMenu = WM_APP + 3,
               CaptionClosed = WM_APP + 4, ShowSettings = WM_APP + 5;
constexpr UINT MenuMode = 100, MenuBluetooth = 2, MenuExit = 4, MenuAlwaysShow = 5, MenuMouse = 6,
               MenuOptical = 7, MenuGyro = 8, MenuSettings = 9;
NOTIFYICONDATAW icon{sizeof(icon)};
ipc::Response current;
bool dialog_open = false, have_prompted = false;
uint32_t prompted_generation = 0;
uint8_t prompted_pair = 0;
UINT taskbar_created = 0;
bool icon_added = false;
int exit_code = 0;
unsigned failed_refreshes = 0;
HICON app_icon{};
bool always_show = true;
unsigned visibility_attempts{};
switch2::tray::Events notification_events;
switch2::tray::Caption caption;
bool closing{};
std::unique_ptr<switch2::SettingsWindow> settings;

void announce(const switch2::tray::Notification &message) {
    caption.show(icon.hWnd, icon.uID, app_icon, message, CaptionClosed);
}
void show_settings(HWND window) {
    if (!settings) settings = std::make_unique<switch2::SettingsWindow>([](const std::wstring &text) {
        announce({L"Controller settings", text, L"controller-settings"});
    });
    settings->show(window, app_icon);
}
void exit_after_caption(HWND window) {
    closing = true;
    // Keep the icon as the caption's visible anchor until the final fade ends.
    if (!caption.visible()) DestroyWindow(window);
}

void add_icon(HWND window) {
    icon.hWnd = window;
    icon.uID = 1;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = TrayMessage;
    icon.hIcon = app_icon;
    wcscpy_s(icon.szTip, switch2::AppName);
    icon_added = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);
    visibility_attempts = 10;
}
std::wstring description() {
    if (current.pro_connected && !current.status.connected)
        return L"Switch 2 Pro Controller connected";
    if (!current.available)
        return L"Waiting for the Joy-Con driver";
    if (current.device_error)
        return L"Windows could not activate a Joy-Con controller";
    if (!current.status.connected)
        return L"No Joy-Cons connected - press a button to wake one";
    const auto physical = std::popcount(unsigned(current.status.connected));
    const auto logical = std::popcount(unsigned(current.status.active_slots));
    if (physical == 1)
        return (current.status.right_sides & current.status.connected)
                   ? L"Right Joy-Con - sideways controller"
                   : L"Left Joy-Con - sideways controller";
    return (current.pro_connected ? L"Pro Controller + " : L"") + std::to_wstring(physical) +
           L" Joy-Cons connected - " + std::to_wstring(logical) +
           (logical == 1 ? L" Joy-Con controller" : L" Joy-Con controllers");
}
void refresh(HWND window) {
    DWORD session = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session) && session != WTSGetActiveConsoleSessionId()) {
        DestroyWindow(window);
        return;
    }
    ipc::Response response;
    if (ipc::exchange({}, response)) {
        if ((response.status.connected || response.pro_connected) && !icon_added) add_icon(window);
        const auto messages = notification_events.observe(response.status, response.pro_connected != 0);
        if (messages.size() == 1) announce(messages.front());
        else if (!messages.empty()) {
            std::wstring body;
            for (const auto &message : messages) {
                if (!body.empty()) body += L"\n";
                body += message.title + L": " + message.body;
            }
            announce({L"Controller update", body, L"status"});
        }
        if (response.device_error && response.device_error != current.device_error)
            announce({L"Controller unavailable", L"Windows could not activate a controller device.", L"device-error"});
        current = response;
        failed_refreshes = 0;
        if (!current.status.connected && !current.pro_connected) {
            if (!closing) exit_after_caption(window);
            return;
        }
        closing = false;
    } else if (++failed_refreshes >= 3) {
        announce({L"Controller driver unavailable", L"The controller manager is not responding.", L"driver-error"});
        KillTimer(window, 1);
        exit_after_caption(window);
        return;
    } else return;
    if (!icon_added) add_icon(window);
    if (visibility_attempts) {
        --visibility_attempts;
        if (switch2::tray::apply_visibility(always_show)) visibility_attempts = 0;
    }
    if (current.available && current.status.needs_choice && !dialog_open &&
        (!have_prompted || (prompted_generation != current.status.generation ||
                            prompted_pair != current.status.needs_choice))) {
        prompted_generation = current.status.generation;
        prompted_pair = current.status.needs_choice;
        have_prompted = true;
        PostMessageW(window, ShowChoice, 0, 0);
    }
}
struct ChoiceContext {
    uint32_t generation;
    uint8_t pair;
    bool stale{};
};
std::wstring pair_description(uint8_t pair) {
    std::wstring text;
    for (unsigned device = 0; device < switch2::MaxJoyCons; ++device) {
        if (!(pair & (1u << device))) continue;
        if (!text.empty()) text += L" and ";
        text += (current.status.right_sides & (1u << device)) ? L"right Joy-Con" : L"left Joy-Con";
        for (unsigned slot = 0; slot < switch2::MaxJoyCons; ++slot) {
            if (current.status.groups[slot] & (1u << device)) {
                text += L" (Player " + std::to_wstring(slot + 1) + L")";
                break;
            }
        }
    }
    return L"Choose how to use the " + text + L". You can change this later from the tray icon.";
}
HRESULT CALLBACK choice_callback(HWND dialog, UINT notification, WPARAM, LPARAM, LONG_PTR data) {
    if (notification == TDN_TIMER) {
        auto &context = *reinterpret_cast<ChoiceContext *>(data);
        ipc::Response response;
        if (!ipc::exchange({}, response) || !response.available ||
            (context.pair != response.status.pairs[0] &&
             context.pair != response.status.pairs[1]) ||
            response.status.generation != context.generation) {
            context.stale = true;
            SendMessageW(dialog, TDM_CLICK_BUTTON, IDCANCEL, 0);
        }
    }
    return S_OK;
}
void choose_mode(HWND window, uint8_t pair = 0) {
    if (!pair)
        pair = current.status.needs_choice ? current.status.needs_choice : current.status.pairs[0];
    if (dialog_open || !current.available || !pair)
        return;
    dialog_open = true;
    ChoiceContext context{current.status.generation, pair};
    const TASKDIALOG_BUTTON buttons[]{
        {100,
         L"Use both Joy-Cons as one controller?\nHold both Joy-Cons upright, one in each hand."},
        {101, L"Use both Joy-Cons separately?\nHold each Joy-Con sideways. SL and SR are the "
              L"shoulder buttons."}};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = window;
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER |
                     TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = switch2::AppName;
    config.pszMainInstruction = L"How would you like to use your Joy-Cons?";
    const auto content = pair_description(pair);
    config.pszContent = content.c_str();
    config.cButtons = ARRAYSIZE(buttons);
    config.pButtons = buttons;
    config.nDefaultButton = 101;
    config.pfCallback = choice_callback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&context);
    int selected = IDCANCEL;
    const HRESULT result = TaskDialogIndirect(&config, &selected, nullptr, nullptr);
    if (SUCCEEDED(result) && !context.stale) {
        ipc::Request request;
        request.command = ipc::Command::Choose;
        request.generation = context.generation;
        request.pair = context.pair;
        request.mode =
            uint32_t(selected == 100 ? switch2::JoyMode::Combined : switch2::JoyMode::Separate);
        ipc::Response response;
        if (!ipc::exchange(request, response) || response.result) {
            MessageBoxW(window,
                        L"The connection changed before your choice was applied. Try again from "
                        L"the tray icon.",
                        switch2::AppName, MB_OK | MB_ICONINFORMATION);
        }
    }
    dialog_open = false;
}
void open_bluetooth_settings(HWND window) {
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        window, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32)
        MessageBoxW(window, L"Open Windows Settings > Bluetooth & devices to pair a controller.",
                    switch2::AppName, MB_OK | MB_ICONINFORMATION);
}
void menu(HWND window) {
    HMENU menu = CreatePopupMenu();
    const auto text = description();
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, text.c_str());
    if (current.message[0])
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, current.message);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (unsigned index = 0; index < 2; ++index) {
        if (!current.status.pairs[index])
            continue;
        const auto label = current.status.pairs[1]
                               ? L"Choose for Joy-Con pair " + std::to_wstring(index + 1) + L"..."
                               : std::wstring(L"Choose one or two controllers...");
        AppendMenuW(menu, MF_STRING | (current.available ? 0 : MF_GRAYED), MenuMode + index,
                    label.c_str());
    }
    AppendMenuW(menu, MF_STRING, MenuBluetooth, L"Pair a controller in Windows...");
    AppendMenuW(menu, MF_STRING, MenuSettings, L"Settings...");
    AppendMenuW(menu, MF_STRING | (current.status.mouse_enabled ? MF_CHECKED : MF_UNCHECKED) |
                         (current.available && current.status.connected ? 0 : MF_GRAYED),
                MenuMouse, L"Mouse Mode");
    const auto source_flags = MF_STRING | (current.available && current.status.connected ? 0 : MF_GRAYED);
    AppendMenuW(menu, source_flags, MenuOptical, L"Optical Mouse");
    AppendMenuW(menu, source_flags, MenuGyro, L"Gyro Mouse");
    CheckMenuRadioItem(menu, MenuOptical, MenuGyro, current.status.mouse_source ? MenuGyro : MenuOptical, MF_BYCOMMAND);
    if (current.status.mouse_enabled && current.status.mouse_source)
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, current.status.gyro_paused ?
                    L"Movement paused - click stick to resume" : L"Movement active - click stick to pause");
    if (current.available && current.status.connected) {
        if (current.status.mouse_enabled) {
            const auto target = switch2::tray::mouse_target(current.status);
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, target.c_str());
        }
        if (!current.status.mouse_source) {
            const auto sensitivity = L"Optical sensitivity: " +
                                     std::to_wstring(unsigned(current.status.mouse_sensitivity) * 25) + L"%";
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, sensitivity.c_str());
        }
    }
    if (current.status.mouse_enabled && current.status.mouse_errors)
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"Mouse input unavailable on a connected Joy-Con");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (always_show ? MF_CHECKED : MF_UNCHECKED), MenuAlwaysShow,
                L"Always show in taskbar tray");
    AppendMenuW(menu, MF_STRING, MenuExit, L"Exit tray app");
    POINT point;
    GetCursorPos(&point);
    SetForegroundWindow(window);
    const auto selected =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    if (selected == MenuMode || selected == MenuMode + 1)
        choose_mode(window, current.status.pairs[selected - MenuMode]);
    else if (selected == MenuBluetooth)
        open_bluetooth_settings(window);
    else if (selected == MenuSettings)
        show_settings(window);
    else if (selected == MenuMouse || selected == MenuOptical || selected == MenuGyro) {
        ipc::Request request;
        request.command = selected == MenuMouse ? ipc::Command::MouseMode : ipc::Command::MouseSource;
        request.mode = selected == MenuMouse ? !current.status.mouse_enabled : selected == MenuGyro;
        ipc::Response response;
        if (!ipc::exchange(request, response) || response.result)
            MessageBoxW(window, L"Mouse mode could not be changed. Please try again.",
                        switch2::AppName, MB_OK | MB_ICONINFORMATION);
    }
    else if (selected == MenuAlwaysShow) {
        if (switch2::tray::save_preference(!always_show)) {
            always_show = !always_show;
            visibility_attempts = switch2::tray::apply_visibility(always_show) ? 0 : 10;
            announce({L"Tray visibility", always_show ? L"Always show in taskbar tray: On." :
                                                    L"Always show in taskbar tray: Off.", L"tray-visibility"});
        } else {
            MessageBoxW(window, L"Windows could not save the tray visibility preference.",
                        switch2::AppName, MB_OK | MB_ICONINFORMATION);
        }
    }
    else if (selected == MenuExit) {
        const bool gyro = current.status.mouse_enabled && current.status.mouse_source;
        if (gyro) {
            ipc::Request request;
            request.command = ipc::Command::MouseMode;
            ipc::Response response;
            ipc::exchange(request, response);
        }
        announce({L"Tray app closed", gyro ?
            L"Gyro mouse stopped. Controller input continues. The tray returns after the next connection." :
            L"Controller input continues. The tray returns after the next connection.", L"tray-exit"});
        exit_code = 2; // Preserve an explicit Exit until the next connection.
        KillTimer(window, 1);
        exit_after_caption(window);
    }
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (taskbar_created && message == taskbar_created) {
        icon_added = false;
        refresh(window);
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        icon.hWnd = window;
        icon.uID = 1;
        SetTimer(window, 1, 500, nullptr);
        PostMessageW(window, WM_TIMER, 1, 0);
        return 0;
    case WM_TIMER:
        if (!dialog_open)
            refresh(window);
        return 0;
    case ShowChoice:
        choose_mode(window);
        return 0;
    case ShowMenu:
        menu(window);
        return 0;
    case ShowSettings:
        show_settings(window);
        return 0;
    case CaptionClosed:
        if (closing && !caption.visible()) DestroyWindow(window);
        return 0;
    case TrayMessage:
        if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == NIN_SELECT ||
            LOWORD(lparam) == NIN_KEYSELECT)
            menu(window);
        return 0;
    case WM_DESTROY:
        settings.reset();
        KillTimer(window, 1);
        if (icon_added) Shell_NotifyIconW(NIM_DELETE, &icon);
        icon_added = false;
        caption.close();
        PostQuitMessage(exit_code);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argument_count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments) return ERROR_INVALID_DATA;
    const std::wstring option = argument_count == 2 ? arguments[1] : L"";
    LocalFree(arguments);
    if (argument_count > 2 || (argument_count == 2 && option != L"--connected-only" &&
        option != L"--show-menu" && option != L"--settings")) return ERROR_BAD_ARGUMENTS;
    switch2::Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\Switch2Native.JoyCons.Tray"));
    if (!mutex.valid()) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (option == L"--show-menu" || option == L"--settings")
            if (auto window = FindWindowW(L"Switch2Native.JoyCons.Tray", nullptr))
                PostMessageW(window, option == L"--settings" ? ShowSettings : ShowMenu, 0, 0);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    always_show = switch2::tray::always_show();
    app_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    // An explicit launch while disconnected opens Windows pairing settings.
    // Automatic service launches never open Settings.
    if (option != L"--connected-only" && ipc::exchange({}, current) &&
        !current.status.connected && !current.pro_connected) {
        open_bluetooth_settings(nullptr);
        if (app_icon) DestroyIcon(app_icon);
        return 0;
    }
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = app_icon;
    window_class.lpszClassName = L"Switch2Native.JoyCons.Tray";
    if (!RegisterClassW(&window_class))
        return 1;
    const auto window = CreateWindowExW(0, window_class.lpszClassName, switch2::AppName, WS_OVERLAPPED, 0,
                                       0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window)
        return 1;
    if (option == L"--show-menu") PostMessageW(window, ShowMenu, 0, 0);
    if (option == L"--settings") PostMessageW(window, ShowSettings, 0, 0);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (settings && settings->window() && IsDialogMessageW(settings->window(), &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (app_icon) DestroyIcon(app_icon);
    return exit_code;
}
