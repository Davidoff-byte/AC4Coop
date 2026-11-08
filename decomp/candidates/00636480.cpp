// 0x636480  sub_636480  size=421
char __thiscall sub_636480(int *this, int a2)
{
  int v3; // eax
  bool v4; // al
  bool v5; // al
  int v6; // ebx
  _DWORD *v7; // edi
  int v8; // eax
  int v9; // ebx
  int v10; // ebx
  int v12; // [esp+10h] [ebp+8h]
  int v13; // [esp+10h] [ebp+8h]

  v3 = *this & 0x3F;
  if ( v3 != a2 )
  {
    if ( a2 != 40 && v3 != 40 )
    {
      v4 = !a2 || ((unsigned __int64)(1LL << a2) >> 32) & 0x804 | (unsigned int)&loc_404207 & (unsigned int)(1LL << a2);
      *this ^= (unsigned int)&unk_4000000 & (*this ^ (!v4 << 26));
      v5 = !a2 || ((unsigned __int64)(1LL << a2) >> 32) & 4 | (unsigned int)(1LL << a2) & 0xA0584203;
      v6 = *this ^ ((!v5 << 27) ^ *this) & 0x8000000;
      *this = v6;
      LOBYTE(v3) = !a2
                || ((unsigned __int64)(1LL << a2) >> 32) & 0xC04
                 | ((unsigned int)&loc_C03FFF + 4) & (unsigned int)(1LL << a2);
      *this = v6 ^ (v6 ^ (((_BYTE)v3 == 0) << 28)) & 0x10000000;
    }
    v7 = (_DWORD *)*(this + 2);
    *this ^= ((unsigned __int8)a2 ^ (unsigned __int8)*this) & 0x3F;
    if ( v7 )
    {
      v3 = *(this + 1);
      if ( v3 )
      {
        v8 = v3 - 1;
        if ( v8 )
        {
          v3 = v8 - 1;
          if ( !v3 )
            v7[7] = *this;
        }
        else
        {
          v9 = v7[2];
          v12 = 0;
          if ( v9 )
          {
            v12 = *(_DWORD *)(v9 + 896);
            sub_8264E0(*(_DWORD *)(v12 + 288));
          }
          v3 = *this;
          v7[11] = *this;
          if ( v9 )
          {
            sub_C3C4D0(v7, 0, 0);
            LOBYTE(v3) = sub_826360(*(_DWORD *)(v12 + 288));
          }
        }
      }
      else
      {
        v10 = v7[2];
        v13 = 0;
        if ( v10 )
        {
          v13 = *(_DWORD *)(v10 + 896);
          LOBYTE(v3) = sub_8264E0(*(_DWORD *)(v13 + 288));
        }
        v7[11] = *this;
        if ( v10 )
        {
          sub_C3C250(v7, 0);
          LOBYTE(v3) = sub_826360(*(_DWORD *)(v13 + 288));
        }
      }
    }
  }
  return v3;
}
