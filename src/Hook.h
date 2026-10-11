#pragma once
#pragma comment(lib, "winmm.lib")

#include <Windows.h>

extern uintptr_t GameModuleBase;

namespace Hook
{
	int DetourFunc(BYTE* pAddress, DWORD dwJumpTo, DWORD dwLen);
	void RestoreFunction(uintptr_t address, uintptr_t size, BYTE* values);

	template <typename Ret, typename C, typename... Args>
	Ret CallClassMethod(unsigned long address, C _this, Args... args)
	{
		return (reinterpret_cast<Ret(__thiscall*)(C, Args...)>(address))(_this, args...);
	}

	template <typename Ret, typename... Args>
	Ret CallCdeclMethod(unsigned long address, Args... args)
	{
		return (reinterpret_cast<Ret(__cdecl*)(Args...)>(address))(args...);
	}

	template <typename Ret, typename... Args>
	Ret CallStdMethod(unsigned long address, Args... args)
	{
		return (reinterpret_cast<Ret(_stdcall*)(Args...)>(address))(args...);
	}

	template <typename Ret, typename... Args>
	Ret CallFastCallMethod(unsigned long address, Args... args)
	{
		return (reinterpret_cast<Ret(_fastcall*)(Args...)>(address))(args...);
	}

	void PreciseSleep(unsigned int microseconds);
}