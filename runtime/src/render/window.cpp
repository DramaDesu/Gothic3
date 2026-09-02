#include "window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>

namespace render
{
namespace
{

struct State
{
    bool closed = false;
    bool resized = false;
    std::array<bool, 256> down{};
    std::array<bool, 256> pressed{};
};

State &stateOf(HWND window)
{
    static State fallback;
    auto *state = reinterpret_cast<State *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return state ? *state : fallback;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_CLOSE:
        case WM_DESTROY: stateOf(window).closed = true; return 0;
        case WM_SIZE: stateOf(window).resized = true; return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wparam < 256)
            {
                State &state = stateOf(window);
                if (!state.down[wparam])
                    state.pressed[wparam] = true;
                state.down[wparam] = true;
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wparam < 256)
                stateOf(window).down[wparam] = false;
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

State g_state;

} // namespace

Window::Window(const std::string &title, int width, int height) : m_width(width), m_height(height)
{
    m_instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = m_instance;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.lpszClassName = L"GenomeRuntimeWindow";
    RegisterClassExW(&windowClass);

    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    const std::wstring wide(title.begin(), title.end());
    m_handle = CreateWindowExW(0, windowClass.lpszClassName, wide.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                               m_instance, nullptr);
    SetWindowLongPtrW(m_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&g_state));

    // Shown without being activated: the viewer is started from a terminal,
    // often several times in a row while something is being measured, and
    // taking the keyboard away each time is worse than useless. Clicking it
    // still brings it forward. Input does not need focus - movement and look
    // are read through GetAsyncKeyState and GetCursorPos, which are global.
    ShowWindow(m_handle, SW_SHOWNOACTIVATE);
    SetWindowPos(m_handle, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

Window::~Window()
{
    if (m_handle)
        DestroyWindow(m_handle);
}

bool Window::pump()
{
    g_state.pressed.fill(false);

    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    RECT client{};
    GetClientRect(m_handle, &client);
    m_width = client.right - client.left;
    m_height = client.bottom - client.top;

    return !g_state.closed;
}

bool Window::resized()
{
    const bool value = g_state.resized;
    g_state.resized = false;
    return value;
}

bool Window::keyDown(int virtualKey) const
{
    return virtualKey >= 0 && virtualKey < 256 && g_state.down[virtualKey];
}

bool Window::keyPressed(int virtualKey)
{
    if (virtualKey < 0 || virtualKey >= 256)
        return false;
    const bool value = g_state.pressed[virtualKey];
    g_state.pressed[virtualKey] = false;
    return value;
}

} // namespace render
