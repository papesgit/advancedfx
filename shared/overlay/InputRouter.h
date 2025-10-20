#pragma once

#include <functional>
#include <stdint.h>
#include <atomic>

struct HWND__;
typedef HWND__* HWND;

namespace advancedfx {
namespace overlay {

class InputRouter {
public:
    using MsgCallback = std::function<bool(void* hwnd, unsigned int msg, uint64_t wparam, int64_t lparam)>;

    InputRouter();
    ~InputRouter();

    // Install WndProc hook for given HWND (Win32 only). Returns true on success.
    bool Attach(void* hwnd);
    void Detach();

    void SetMessageCallback(MsgCallback cb);
    void* GetAttachedHwnd() const { return m_Hwnd; }
    
#ifdef _WIN32
    // Diagnostics & fallback: query if any WM_KEYDOWN was seen since last frame.
    static bool ConsumeKeydownSeenThisFrame();
    // Mark that a WM_KEYDOWN was observed this frame (for external WndProc integrations).
    static void NotifyKeydown();

    // Public accessor for configured toggle key.
    static int GetToggleKey();
#endif

private:
#ifdef _WIN32
    // Win32 WndProc thunk used to intercept input when overlay is visible.
    static long __stdcall WndProcThunk(void* hWnd, unsigned int msg, unsigned long long wParam, long long lParam);
#endif

    void* m_Hwnd = nullptr;
    void* m_OldProc = nullptr;
    MsgCallback m_Callback;

#ifdef _WIN32
    static InputRouter* s_Active;
    static std::atomic<bool> s_KeydownSeenThisFrame;
    static bool s_FirstKeydownLogged;

    // Compile-time selectable toggle key
    #ifndef OVERLAY_TOGGLE_VK
    #define OVERLAY_TOGGLE_VK VK_F8
    #endif
    static const int kToggleVk;
#endif
};

} // namespace overlay
} // namespace advancedfx
