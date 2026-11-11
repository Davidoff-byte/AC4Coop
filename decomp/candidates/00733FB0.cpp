// 0x733FB0  sub_733FB0  size=50
int __thiscall sub_733FB0(unsigned __int8 *this)
{
  int result; // eax
  int v2; // ecx
  _DWORD *v3; // esi

  result = (int)this;
  v2 = *(this + 212);
  if ( v2 != 4 )
  {
    if ( v2 == 5 )
    {
      v3 = *(_DWORD **)(result + 560);
      result = sub_E10E50(v3);
      if ( !result )
        return sub_4956E0(v3);
    }
    else
    {
      return 0;
    }
  }
  return result;
}
