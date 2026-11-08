// 0x66F630  sub_66F630  size=246
char __thiscall sub_66F630(int this, int a2)
{
  int v3; // esi
  unsigned int v4; // ecx
  unsigned int v5; // ebx
  int v6; // ecx
  int v7; // eax
  _DWORD *v8; // eax
  int v9; // ecx
  int v10; // edi
  int i; // ebx
  char result; // al

  v3 = (*(int *)(*(_DWORD *)a2 + 8) >> 31) & **(_DWORD **)a2;
  (*(void (__thiscall **)(int, int))(*(_DWORD *)v3 + 56))(v3, this);
  v4 = *(_DWORD *)(this + 12) & 0x3FFF;
  if ( (unsigned int)*(unsigned __int16 *)(this + 14) + 1 > v4 )
  {
    v5 = v4 >= 0x18 ? v4 / 3 + v4 : v4 + 4;
    if ( v4 < v5 )
      sub_A09AB0(v5, 4, dword_4DD5EE0, 0);
  }
  v6 = *(_DWORD *)(this + 8);
  v7 = *(unsigned __int16 *)(this + 14) + 1;
  *(_WORD *)(this + 14) = v7;
  v8 = (_DWORD *)(v6 + 4 * v7 - 4);
  if ( v8 )
  {
    v9 = *(_DWORD *)a2;
    *v8 = *(_DWORD *)a2;
    _InterlockedExchangeAdd((volatile signed __int32 *)(v9 + 8), 1u);
  }
  LOBYTE(a2) = 0;
  sub_66D160(
    *(_DWORD *)(this + 8),
    (volatile __int32 *)(*(_DWORD *)(this + 8) + 4 * *(unsigned __int16 *)(this + 14)),
    (4 * *(unsigned __int16 *)(this + 14)) >> 2,
    a2);
  v10 = sub_554FA0();
  for ( i = sub_554FB0(); v10 != i; v10 += 4 )
    (*(void (__thiscall **)(int, int))(*(_DWORD *)v3 + 84))(v3, (*(int *)(*(_DWORD *)v10 + 8) >> 31) & **(_DWORD **)v10);
  result = sub_45BED0();
  if ( !result )
    return (*(int (__thiscall **)(int))(*(_DWORD *)v3 + 72))(v3);
  return result;
}
