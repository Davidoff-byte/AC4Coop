// 0x18B9EA0  sub_18B9EA0  size=56
int __thiscall sub_18B9EA0(void *this, int a2)
{
  int v2; // esi
  int (__thiscall ***v3)(_DWORD, int); // ecx

  if ( *(_DWORD *)this
    && (v2 = dword_4E20544,
        (v3 = *(int (__thiscall ****)(_DWORD, int))(*(_DWORD *)(*(int (__thiscall **)(_DWORD))(**(_DWORD **)(*(_DWORD *)this + 32)
                                                                                             + 48))(*(_DWORD *)(*(_DWORD *)this + 32))
                                                  + 4 * *(unsigned __int8 *)(v2 + 53))) != nullptr) )
  {
    return (**v3)(v3, a2);
  }
  else
  {
    return 3;
  }
}
