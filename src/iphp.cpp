#include <windows.h>
#include "InitState.h"

#pragma pack(1)


HINSTANCE hLThis = 0;
HINSTANCE hL = 0;
FARPROC p[267] = {0};

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{

	if (reason == DLL_PROCESS_ATTACH)
	{
		hLThis = hInst;

		TCHAR imagePath[MAX_PATH];
		DWORD len = GetModuleFileName(hInst, imagePath, MAX_PATH);
		moduleFile = std::string(imagePath, len) + std::string("\0", 1);

		InitiateStates();

	}
	if (reason == DLL_PROCESS_DETACH)
	{
		DestroyStates();

		return 1;
	}
}