// 0x5FAC00  sub_5FAC00  size=94
int __thiscall sub_5FAC00(void *this, _DWORD *a2, int a3)
{
  int v4; // eax

  sub_7A22F0(a3);
  sub_79DD50(a3);
  sub_A2D5A0(0, 1);
  *a2 = (*(int (__thiscall **)(void *, int))(*(_DWORD *)this + 76))(this, a3);
  (*(void (__thiscall **)(void *, _DWORD *, int))(*(_DWORD *)this + 64))(this, a2, a3);
  sub_A2F320();
  v4 = sub_162BEE0(a3);
  sub_8B2B10(v4);
  return sub_7A2380(a3);
}
