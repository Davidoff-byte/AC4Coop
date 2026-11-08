// 0x106ABF0  sub_106ABF0  size=142
__m128 *__thiscall sub_106ABF0(int *this, __int32 *a2, __m128 *a3, __m128 *a4)
{
  __m128 *v5; // ecx
  int v6; // esi
  int v7; // esi
  int v8; // eax
  volatile __int32 *v9; // eax

  if ( ((*(int *)(*(this + 50) + 8) >> 31) & *(_DWORD *)*(this + 50)) == 0 )
    return nullptr;
  v5 = a4;
  if ( !a4 )
    return nullptr;
  a4 = nullptr;
  sub_527BA0(v5, &a4);
  if ( !a4 )
    return nullptr;
  v6 = *(this + 2);
  sub_431490(a4, a3);
  if ( !v6 )
    return nullptr;
  sub_5A8A80(v6, (int)a4, 0xFFFF, 1);
  v7 = (int)a4;
  v8 = sub_15C92E0();
  v9 = (volatile __int32 *)sub_5200C0(v7, v8);
  if ( (int *)*a2 != &dword_4DD5F8C )
  {
    if ( v9 )
      sub_105AE30(v9, a2);
  }
  return a4;
}
