// 0x17106C0  sub_17106C0  size=104
int __thiscall sub_17106C0(_DWORD *this, int a2, int a3)
{
  int result; // eax
  int v4; // edx
  bool v5; // zf
  int savedregs; // [esp+0h] [ebp+0h] BYREF

  result = *(_DWORD *)(a2 + 156);
  if ( (result & 7) == 2 )
  {
    result &= 0x1F8u;
    if ( result == 152 )
    {
      v4 = *(_DWORD *)(a3 + 16);
      result = *(_DWORD *)(a3 + 32);
      if ( result == 3 || result == 6 )
      {
        if ( v4 == 23 || v4 == 5 || v4 == 14 || v4 == 26 )
        {
LABEL_12:
          a2 = *(_DWORD *)(*(this + 1) + 28);
          return sub_1A90920(&a2, (int)&savedregs);
        }
        v5 = v4 == 25;
      }
      else
      {
        v5 = result == 14;
      }
      if ( !v5 )
        return result;
      goto LABEL_12;
    }
  }
  return result;
}
