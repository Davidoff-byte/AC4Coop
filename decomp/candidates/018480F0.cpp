// 0x18480F0  sub_18480F0  size=152   (IDA 9.3 decompile, AC4BFSP.exe.i64 private copy session 76541c37, 2026-11-27, verbatim)
int sub_18480F0()
{
  int v0; // eax
  int v1; // esi
  unsigned int v2; // edi
  unsigned int v3; // ecx
  int v4; // eax

  sub_D587F0(unk_2AC1FCC);
  v0 = sub_418A80(16, 4);
  v1 = v0;
  if ( v0 )
    sub_18A6280(v0);
  v2 = unk_2AC1FC0 & 0x3FFF;
  if ( (unsigned int)(unsigned __int16)word_2AC1FC2 + 1 > v2 )
  {
    if ( v2 >= 0x18 )
      v3 = v2 / 3 + v2;
    else
      v3 = v2 + 4;
    if ( v2 < v3 )
      sub_A09AB0(&dword_2AC1FBC, v3, 4, dword_4DD5EE0, 0);
  }
  v4 = (unsigned __int16)word_2AC1FC2++ + 1;
  *(_DWORD *)(dword_2AC1FBC + 4 * v4 - 4) = v1;
  sub_AC6580(unk_2AC1FCC);
  return v1;
}
