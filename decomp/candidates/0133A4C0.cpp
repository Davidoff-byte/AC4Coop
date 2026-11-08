// 0x133A4C0  sub_133A4C0  size=125
int __cdecl sub_133A4C0(int a1)
{
  int v1; // esi
  _DWORD *v3; // eax
  _DWORD *v4; // edi
  _DWORD *v5; // eax
  _DWORD *v6; // ecx
  int v7; // esi
  int v8; // eax
  int v9; // esi

  v1 = a1;
  if ( !a1 )
    return 0;
  v3 = (_DWORD *)sub_6F5F70(a1);
  v4 = v3;
  if ( v3 && sub_F916A0(v3) )
    return v4[41];
  v5 = sub_11DDD60(&a1, v1);
  v6 = (_DWORD *)*v5;
  v7 = *(_DWORD *)(*v5 + 8);
  v8 = a1;
  v9 = *v6 & (v7 >> 31);
  if ( !_InterlockedDecrement((volatile signed __int32 *)(a1 + 4)) && (*(_DWORD *)(v8 + 8) & 0xFFFFFF) == 0 )
    sub_A094D0(byte_4DD5FA0, v8);
  return v9;
}
