// 0x5FD520  sub_5FD520  size=59
_DWORD *__thiscall sub_5FD520(void *this, int a2)
{
  _DWORD *v2; // esi
  int v3; // eax
  int v4; // eax

  v2 = nullptr;
  v3 = sub_5FAC60((int)this, a2);
  v4 = **(_DWORD **)v3 & (*(int *)(*(_DWORD *)v3 + 8) >> 31);
  if ( v4 )
  {
    v2 = (_DWORD *)(*(int (__thiscall **)(int, _DWORD, _DWORD))(*(_DWORD *)v4 + 12))(v4, 0, 0);
    sub_526590(v2);
    sub_A2E820();
  }
  return v2;
}
