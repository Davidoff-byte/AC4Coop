// 0x5FD730  sub_5FD730  size=95
int __thiscall sub_5FD730(void *this, int a2)
{
  int v2; // eax
  _DWORD *v4; // esi
  int v5; // eax
  int v6; // ecx

  if ( *(_BYTE *)dword_2AC1E68 )
  {
    v2 = sub_5FAC60((int)this, a2);
    return (*(int *)(*(_DWORD *)v2 + 8) >> 31) & **(_DWORD **)v2;
  }
  else
  {
    v4 = nullptr;
    v5 = sub_5FAC60((int)this, a2);
    v6 = (*(int *)(*(_DWORD *)v5 + 8) >> 31) & **(_DWORD **)v5;
    if ( v6 )
    {
      v4 = (_DWORD *)(*(int (__thiscall **)(int, _DWORD, _DWORD))(*(_DWORD *)v6 + 12))(v6, 0, 0);
      sub_526590(v4);
      sub_A2E820();
    }
    return (int)v4;
  }
}
