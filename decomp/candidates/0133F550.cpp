// 0x133F550  sub_133F550  size=70
bool __cdecl sub_133F550(int a1)
{
  bool result; // al
  int v2; // eax
  _DWORD *v3; // eax

  result = false;
  if ( a1 )
  {
    v2 = sub_7025F0(a1);
    if ( !v2 || (*(_BYTE *)(v2 + 184) & 0xC) != 4 )
    {
      v3 = sub_10EF8C0(a1);
      if ( !v3 || (v3[1] & 0x1000) == 0 )
        return true;
    }
  }
  return result;
}
