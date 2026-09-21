#pragma once
#include <windows.h>
constexpr int CSIDL_APPDATA = 26, SHGFP_TYPE_CURRENT = 0;
inline int SHGetFolderPathA(HWND, int, void*, DWORD, char* path) {
	std::strcpy(path, fake_win32::appData.c_str());
	return 0;
}
