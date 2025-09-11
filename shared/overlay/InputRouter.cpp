#include "InputRouter.h"
#include "Overlay.h"
#include "../AfxConsole.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace advancedfx {
namespace overlay {

#ifdef _WIN32
InputRouter* InputRouter::s_Active = nullptr;
std::atomic<bool> InputRouter::s_KeydownSeenThisFrame{false};
bool InputRouter::s_FirstKeydownLogged = false;
const int InputRouter::kToggleVk = OVERLAY_TOGGLE_VK;
const int InputRouter::kToggleAltVk = OVERLAY_TOGGLE_ALT_VK;

long __stdcall InputRouter::WndProcThunk(void* hwnd, unsigned int msg, unsigned long long wParam, long long lParam) {
    HWND hWnd = (HWND)hwnd;
    // Toggle visibility on F10 keydown regardless of visibility (do not consume).
    if (msg == WM_KEYDOWN) {
        if (!s_FirstKeydownLogged) {
            s_FirstKeydownLogged = true;
            advancedfx::Message("Overlay: first WM_KEYDOWN vk=0x%02X\n", (unsigned)wParam);
        }
        s_KeydownSeenThisFrame.store(true, std::memory_order_relaxed);
        if (wParam == kToggleVk || wParam == kToggleAltVk) {
            advancedfx::Message("Overlay: toggle hotkey (WndProc) vk=0x%02X\n", (unsigned)wParam);
            Overlay::Get().ToggleVisible();
        }
    }

    // If visible and callback consumes input, return 1 to mark handled
    if (Overlay::Get().IsVisible() && s_Active && s_Active->m_Callback) {
        if (s_Active->m_Callback(hWnd, msg, (uint64_t)wParam, (int64_t)lParam)) {
            return 1;
        }
    }

    // Pass through to original WndProc
    if (s_Active && s_Active->m_OldProc) {
        return CallWindowProcW((WNDPROC)s_Active->m_OldProc, hWnd, (UINT)msg, (WPARAM)wParam, (LPARAM)lParam);
    }
    return 0;
}
#endif

InputRouter::InputRouter() {}
InputRouter::~InputRouter() { Detach(); }

bool InputRouter::Attach(void* hwnd) {
#ifdef _WIN32
    if (!hwnd) return false;
    if (m_Hwnd) return true;
    m_Hwnd = hwnd;
    s_Active = this;
    m_OldProc = (void*)SetWindowLongPtrW((HWND)hwnd, GWLP_WNDPROC, (LONG_PTR)&InputRouter::WndProcThunk);
    advancedfx::Message("Overlay: WndProc hook installed hwnd=0x%p tid=%lu\n", hwnd, (unsigned long)GetCurrentThreadId());
    return m_OldProc != nullptr;
#else
    (void)hwnd; return false;
#endif
}

void InputRouter::Detach() {
#ifdef _WIN32
    if (m_Hwnd && m_OldProc) {
        SetWindowLongPtrW((HWND)m_Hwnd, GWLP_WNDPROC, (LONG_PTR)m_OldProc);
    }
    m_Hwnd = nullptr;
    m_OldProc = nullptr;
    if (s_Active == this) s_Active = nullptr;
#endif
}

void InputRouter::SetMessageCallback(MsgCallback cb) {
    m_Callback = std::move(cb);
}

#ifdef _WIN32
bool InputRouter::ConsumeKeydownSeenThisFrame() {
    bool seen = s_KeydownSeenThisFrame.load(std::memory_order_relaxed);
    s_KeydownSeenThisFrame.store(false, std::memory_order_relaxed);
    return seen;
}

void InputRouter::NotifyKeydown() {
    s_KeydownSeenThisFrame.store(true, std::memory_order_relaxed);
}

int InputRouter::GetToggleKey() { return kToggleVk; }
int InputRouter::GetAltToggleKey() { return kToggleAltVk; }
#endif

} // namespace overlay
} // namespace advancedfx
