#pragma once

// A plain Win32 window. No toolkit: the runtime only needs a surface to draw
// into and a few keys, and a dependency-free window keeps the build trivial.

#include <cstdint>
#include <string>

struct HWND__;
struct HINSTANCE__;

namespace render
{

// Virtual key codes the viewer uses, so tools do not have to include windows.h.
namespace key
{
constexpr int Escape = 0x1B;
constexpr int Space = 0x20;
constexpr int Left = 0x25;
constexpr int Up = 0x26;
constexpr int Right = 0x27;
constexpr int Down = 0x28;
constexpr int W = 'W';
constexpr int S = 'S';
} // namespace key

class Window
{
  public:
    Window(const std::string &title, int width, int height);
    ~Window();

    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    // Drains the message queue; returns false once the window is closed.
    bool pump();

    HWND__ *handle() const { return m_handle; }
    HINSTANCE__ *instance() const { return m_instance; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    bool resized();

    bool keyDown(int virtualKey) const;
    // Edge-triggered: true once per press.
    bool keyPressed(int virtualKey);

  private:
    HWND__ *m_handle = nullptr;
    HINSTANCE__ *m_instance = nullptr;
    int m_width = 0;
    int m_height = 0;
};

} // namespace render
