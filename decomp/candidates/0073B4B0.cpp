// 0x73B4B0  sub_73B4B0  size=31
BOOL __thiscall sub_73B4B0(_BYTE *this)
{
  return (*this & 7) == 3 && (*(_DWORD *)this & 0x1F8) == 8;
}
