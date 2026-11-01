#pragma once
#include "joy_ipc.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace switch2 {
class SettingsWindow {
  public:
    using Notify = std::function<void(const std::wstring &)>;
    explicit SettingsWindow(Notify notify) : notify_(std::move(notify)) {}
    ~SettingsWindow();
    void show(HWND owner, HICON icon);
    void close();
    HWND window() const {
        return window_;
    }

  private:
    struct Panel {
        std::array<HWND, 8> controls{}; // dead zone / shape per stick, speed, recenter, reset
        ControllerProfile draft;
        bool editing{}, saving{};
        double heading{}, rate{};
        uint32_t rate_samples{};
        uint64_t rate_time{}, last_sample{}, last_received{};
        std::wstring status;
    };
    struct Save {
        joy_ipc::Request request;
        std::wstring description;
        bool success{};
        uint32_t error{};
    };
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam);
    void worker(std::stop_token stop);
    void receive();
    void rebuild();
    void layout();
    void paint(HDC dc);
    void save(unsigned index, const std::wstring &description);
    void changed(HWND control, UINT action);
    void fonts();
    void scroll(int position);
    int units(int value) const;
    HWND window_{};
    HFONT font_{}, heading_font_{}, control_font_{};
    HBRUSH brush_{};
    UINT dpi_{96};
    int scroll_{}, content_height_{};
    Notify notify_;
    std::array<Panel, 5> panels_;
    joy_ipc::SettingsResponse current_;
    bool online_{};
    std::jthread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Save> pending_, completed_;
    joy_ipc::SettingsResponse incoming_;
    bool received_{}, delivered_online_{}, posted_{};
};
} // namespace switch2
