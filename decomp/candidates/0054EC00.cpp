// 0x54EC00  sub_54EC00  size=31
BOOL __thiscall sub_54EC00(_DWORD *this)
{
  return (*this & 7) == 2 && (*this & 0x1F8) == 0x10;
}
