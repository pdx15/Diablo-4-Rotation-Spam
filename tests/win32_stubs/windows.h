#pragma once

// Test-only Win32 stand-ins. Never add this directory to the application include path.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

using HRESULT = std::int32_t;
constexpr bool FAILED(HRESULT result) { return result < 0; }
using WORD = std::uint16_t;
using UINT = unsigned int;
using DWORD = std::uint32_t;
using LONG = std::int32_t;
using COLORREF = std::uint32_t;
using LANGID = WORD;
using HWND = void*;
using HDC = void*;
using HMODULE = void*;
using HRSRC = void*;
using HGLOBAL = void*;
struct POINT { LONG x, y; };
struct KEYBDINPUT { WORD wVk, wScan; DWORD dwFlags, time; std::uintptr_t dwExtraInfo; };
struct MOUSEINPUT { LONG dx, dy; DWORD mouseData, dwFlags, time; std::uintptr_t dwExtraInfo; };
struct INPUT { DWORD type; union { KEYBDINPUT ki; MOUSEINPUT mi; }; };

constexpr int MAX_PATH = 260;
constexpr int VK_LBUTTON = 1, VK_RBUTTON = 2, VK_MBUTTON = 4;
constexpr int VK_XBUTTON1 = 5, VK_XBUTTON2 = 6;
constexpr int VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12;
constexpr int VK_F1 = 0x70, VK_F5 = 0x74, VK_F6 = 0x75, VK_F7 = 0x76, VK_F8 = 0x77;
constexpr int VK_F9 = 0x78, VK_F10 = 0x79, VK_F11 = 0x7a, VK_F12 = 0x7b;
constexpr int XBUTTON1 = 1, XBUTTON2 = 2, MAPVK_VK_TO_VSC = 0;
constexpr DWORD INPUT_MOUSE = 0, INPUT_KEYBOARD = 1, KEYEVENTF_KEYUP = 2;
constexpr DWORD MOUSEEVENTF_LEFTDOWN = 2, MOUSEEVENTF_LEFTUP = 4;
constexpr DWORD MOUSEEVENTF_RIGHTDOWN = 8, MOUSEEVENTF_RIGHTUP = 16;
constexpr DWORD MOUSEEVENTF_MIDDLEDOWN = 32, MOUSEEVENTF_MIDDLEUP = 64;
constexpr DWORD MOUSEEVENTF_XDOWN = 128, MOUSEEVENTF_XUP = 256;
constexpr COLORREF CLR_INVALID = 0xffffffff;
constexpr LANGID LANG_RUSSIAN = 0x19;
constexpr WORD PRIMARYLANGID(LANGID value) { return value & 0x3ff; }
#define MAKEINTRESOURCEW(value) reinterpret_cast<const wchar_t*>(static_cast<std::uintptr_t>(value))
#define RT_RCDATA MAKEINTRESOURCEW(10)
constexpr COLORREF RGB(int r, int g, int b) {
	return static_cast<COLORREF>((r & 255) | ((g & 255) << 8) | ((b & 255) << 16));
}
constexpr int GetRValue(COLORREF color) { return color & 255; }
constexpr int GetGValue(COLORREF color) { return (color >> 8) & 255; }
constexpr int GetBValue(COLORREF color) { return (color >> 16) & 255; }

namespace fake_win32 {
struct LoopComplete {};
inline int handle = 1;
inline int tick = 0;
inline int maxTicks = 1;
inline bool gameActive = true;
inline bool healthy = false;
inline bool keys[256] = {};
inline std::function<void(int)> onKeyQuery;
inline std::vector<INPUT> inputs;
inline std::function<void(int)> beforeTick;
inline std::string appData;
inline DWORD lastError = 0;
inline LANGID language = 9;
inline std::map<int, std::string> resources;
}
inline int CreateDirectoryA(const char* path, void*) {
	return std::filesystem::create_directories(path) ? 1 : 0;
}
inline UINT MapVirtualKeyA(UINT key, UINT) { return key; }
inline int GetKeyNameTextA(LONG, char*, int) { return 0; }
inline UINT SendInput(UINT count, INPUT* input, int) {
	fake_win32::inputs.insert(fake_win32::inputs.end(), input, input + count);
	return count;
}
inline HWND GetForegroundWindow() {
	if (fake_win32::tick >= fake_win32::maxTicks) throw fake_win32::LoopComplete{};
	if (fake_win32::beforeTick) fake_win32::beforeTick(fake_win32::tick);
	++fake_win32::tick;
	return fake_win32::gameActive ? &fake_win32::handle : nullptr;
}
inline int GetClassNameW(HWND, wchar_t* name, int count) {
	constexpr auto value = L"Diablo IV Main Window Class";
	std::wcsncpy(name, value, count);
	return static_cast<int>(std::wcslen(value));
}
inline HDC GetDC(HWND) { return &fake_win32::handle; }
inline COLORREF GetPixel(HDC, int, int) {
	return fake_win32::healthy ? RGB(0x9E, 0x30, 0x38) : RGB(0, 0, 0);
}
inline int ReleaseDC(HWND, HDC) { return 1; }
inline short GetAsyncKeyState(int key) {
	if (fake_win32::onKeyQuery) fake_win32::onKeyQuery(key);
	return fake_win32::keys[key] ? -32768 : 0;
}
inline int GetCursorPos(POINT*) { return 0; }
inline HWND FindWindowW(const wchar_t*, const wchar_t*) { return nullptr; }
inline int ScreenToClient(HWND, POINT*) { return 0; }
inline int Beep(DWORD, DWORD) { return 1; }
inline void PostQuitMessage(int) {}
[[noreturn]] inline void ExitProcess(UINT) { std::abort(); }
inline HMODULE GetModuleHandleW(const wchar_t*) { return &fake_win32::handle; }
inline HRSRC FindResourceW(HMODULE, const wchar_t* id, const wchar_t*) {
	auto it = fake_win32::resources.find(static_cast<int>(reinterpret_cast<std::uintptr_t>(id)));
	return it == fake_win32::resources.end() ? nullptr : &it->second;
}
inline HGLOBAL LoadResource(HMODULE, HRSRC resource) { return resource; }
inline DWORD SizeofResource(HMODULE, HRSRC resource) {
	return static_cast<DWORD>(static_cast<std::string*>(resource)->size());
}
inline void* LockResource(HGLOBAL data) { return static_cast<std::string*>(data)->data(); }
inline LANGID GetUserDefaultUILanguage() { return fake_win32::language; }
inline DWORD GetLastError() { return fake_win32::lastError; }
inline void SetLastError(DWORD error) { fake_win32::lastError = error; }
