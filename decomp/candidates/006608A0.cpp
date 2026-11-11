// 0x6608A0  sub_6608A0  size=246
int __thiscall sub_6608A0(int this, int a2)
{
  int result; // eax
  int v4; // esi
  bool v5; // bl
  int v6; // ebx
  int v7; // esi
  int v8; // esi
  int i; // edi
  int v10; // [esp+8h] [ebp-4h] BYREF

  result = a2;
  if ( a2 && *(_DWORD *)(a2 + 2176) <= 1u )
  {
    v4 = sub_A0BDE0(0, 0, a2);
    v10 = v4;
    result = sub_15DFF10(&v10);
    v5 = (_BYTE)result == 0;
    if ( !_InterlockedDecrement((volatile signed __int32 *)(v4 + 4))
      && (((unsigned int)&loc_FFFFF9 + 6) & *(_DWORD *)(v4 + 8)) == 0 )
    {
      result = sub_A094D0(v4);
    }
    if ( v5 )
    {
      v6 = a2;
      v7 = sub_A0BDE0(0, 0, a2);
      a2 = v7;
      sub_1401BB0(&a2, 0);
      if ( !_InterlockedDecrement((volatile signed __int32 *)(v7 + 4))
        && (((unsigned int)&loc_FFFFF9 + 6) & *(_DWORD *)(v7 + 8)) == 0 )
      {
        sub_A094D0(v7);
      }
      v8 = *(_DWORD *)(this + 8);
      result = *(unsigned __int16 *)(this + 14);
      for ( i = v8 + 4 * result; v8 != i; v8 += 4 )
      {
        result = *(int *)(*(_DWORD *)v8 + 8) >> 31;
        if ( (result & **(_DWORD **)v8) != 0 )
          result = (*(int (__thiscall **)(int, int))(*(_DWORD *)(result & **(_DWORD **)v8) + 84))(
                     result & **(_DWORD **)v8,
                     v6);
      }
    }
  }
  return result;
}
