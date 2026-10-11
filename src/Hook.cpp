#include "Hook.h"
#include <chrono>

uintptr_t GameModuleBase = (uintptr_t)GetModuleHandle("ac4bfsp.exe");

namespace Hook
{
	int DetourFunc(BYTE* pAddress, DWORD dwJumpTo, DWORD dwLen)
	{
		DWORD dwOldProtect, dwBkup, dwRelAddr;
		// Basic VirtualProtect... y'all should know this
		VirtualProtect(pAddress, dwLen, PAGE_EXECUTE_READWRITE, &dwOldProtect);
		// Calculate the "distance" we're gonna have to jump - the size of the JMP instruction
		dwRelAddr = (DWORD)(dwJumpTo - (DWORD)pAddress) - 5;
		// Write the JMP opcode @ our jump position...
		*pAddress = 0xE9;
		// Write the offset to where we're gonna jump
		*((DWORD*)(pAddress + 0x1)) = dwRelAddr;
		// Overwrite the rest of the bytes with NOPs
		for (DWORD x = 0x5; x < dwLen; x++)
			*(pAddress + x) = 0x90;
		// Restore the default permissions
		VirtualProtect(pAddress, dwLen, dwOldProtect, &dwBkup);

		return 1;
	}

	void RestoreFunction(uintptr_t address, uintptr_t size, BYTE* values)
	{
		DWORD dwOldProtect, dwBkup;
		BYTE* pAddress = (BYTE*)address;

		VirtualProtect(pAddress, size, PAGE_EXECUTE_READWRITE, &dwOldProtect);

		for (uint32_t i = 0; i < size; i++)
		{
			*((BYTE*)(pAddress + i)) = values[i];
		}

		VirtualProtect(pAddress, size, dwOldProtect, &dwBkup);
	}

	void PreciseSleep(unsigned int microseconds) {
		static bool highResolutionTimerInitialized = false;
		if (!highResolutionTimerInitialized) {
			TIMECAPS tc;
			if (timeGetDevCaps(&tc, sizeof(TIMECAPS)) == TIMERR_NOERROR) {
				timeBeginPeriod(tc.wPeriodMin);
				highResolutionTimerInitialized = true;
			}
		}

		auto start = std::chrono::high_resolution_clock::now();
		auto end = start + std::chrono::microseconds(microseconds);
		while (std::chrono::high_resolution_clock::now() < end) {
			Sleep(0);
		}
	}
}