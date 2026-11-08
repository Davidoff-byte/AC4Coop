// 0x527BA0  sub_527BA0  size=117
int __thiscall sub_527BA0(void *this, _DWORD *a2)
{
  _DWORD *v3; // esi
  int v4; // ecx
  int v5; // eax
  float v6; // xmm0_4

  sub_A2D5A0(0, 1);
  v3 = (_DWORD *)(*(int (__thiscall **)(void *, _DWORD, _DWORD))(*(_DWORD *)this + 12))(this, 0, 0);
  sub_A2F320();
  *a2 = v3;
  v4 = v3[21];
  v3[20] |= 4u;
  v3[20] |= 8u;
  v3[20] |= (unsigned int)&loc_800000;
  v3[21] = v4;
  v5 = v3[23];
  if ( v5 )
    v6 = *(float *)(v5 + 1832);
  else
    v6 = 1.0;
  return sub_522610((int)v3, 0, COERCE_INT(v6 * v6));
}
