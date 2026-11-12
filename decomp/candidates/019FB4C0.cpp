// 0x19FB4C0  sub_19FB4C0  size=50
int __thiscall sub_19FB4C0(__int16 *this, int a2)
{
  bool v3; // sf
  int result; // eax

  v3 = sub_1BFF520(this) < 0;
  result = a2;
  if ( !v3 || a2 >= 0 )
  {
    *(this + 46) = a2;
    if ( a2 < 0 )
      return sub_A014A0(dword_4E1CC40, 0);
  }
  return result;
}
