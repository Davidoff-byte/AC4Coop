// 0x744CF0  sub_744CF0  size=192
char __thiscall sub_744CF0(_DWORD *this, _BYTE *a2)
{
  _BYTE *v3; // ecx
  _BYTE *v4; // eax
  _BYTE *v5; // esi
  _BYTE *v6; // esi
  int v7; // eax
  _BYTE *v9; // esi
  int v10; // eax
  _BYTE *v11; // [esp+Ch] [ebp-8h] BYREF
  unsigned int v12; // [esp+10h] [ebp-4h]

  sub_A062A0(0);
  sub_A0A790(&v11);
  v3 = v11;
  v4 = &v11[HIWORD(v12)];
  v5 = v11;
  if ( v11 == v4 )
  {
LABEL_4:
    if ( (v12 & 0x8000) == 0 )
    {
      if ( v11 )
      {
        v6 = v11;
        v7 = sub_80AC30(v11);
        (*(void (__thiscall **)(_DWORD, _BYTE *, int))(**(_DWORD **)dword_2AC40F0 + 20))(
          *(_DWORD *)dword_2AC40F0,
          v6,
          v7);
      }
    }
    return 0;
  }
  else
  {
    while ( *(this + (unsigned __int8)*v5) )
    {
      if ( ++v5 == v4 )
        goto LABEL_4;
    }
    *a2 = *v5;
    if ( (v12 & 0x8000) == 0 && v3 )
    {
      v9 = v3;
      v10 = sub_80AC30(v3);
      (*(void (__thiscall **)(_DWORD, _BYTE *, int))(**(_DWORD **)dword_2AC40F0 + 20))(
        *(_DWORD *)dword_2AC40F0,
        v9,
        v10);
    }
    return 1;
  }
}
