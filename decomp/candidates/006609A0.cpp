// 0x6609A0  sub_6609A0  size=173
int __thiscall sub_6609A0(int this, int a2)
{
  int result; // eax
  _DWORD *v4; // ecx
  _DWORD *v5; // edx
  _DWORD *v6; // edi
  int v7; // esi
  int i; // edi

  result = sub_A0BDE0(0, 0, a2);
  v4 = *(_DWORD **)(this + 24);
  v5 = &v4[*(unsigned __int16 *)(this + 30)];
  if ( v4 == v5 )
  {
LABEL_4:
    v6 = nullptr;
  }
  else
  {
    while ( result != *v4 )
    {
      if ( ++v4 == v5 )
        goto LABEL_4;
    }
    v6 = v4;
  }
  if ( !_InterlockedDecrement((volatile signed __int32 *)(result + 4))
    && (((unsigned int)&loc_FFFFF9 + 6) & *(_DWORD *)(result + 8)) == 0 )
  {
    result = sub_A094D0(result);
  }
  if ( v6 )
  {
    sub_11B44C0(((int)v6 - *(_DWORD *)(this + 24)) >> 2);
    result = *(unsigned __int16 *)(this + 14);
    v7 = *(_DWORD *)(this + 8);
    for ( i = v7 + 4 * result; v7 != i; v7 += 4 )
    {
      result = *(int *)(*(_DWORD *)v7 + 8) >> 31;
      if ( (result & **(_DWORD **)v7) != 0 )
        result = (*(int (__thiscall **)(int, int))(*(_DWORD *)(result & **(_DWORD **)v7) + 88))(
                   result & **(_DWORD **)v7,
                   a2);
    }
  }
  return result;
}
