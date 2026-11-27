// 0x569560  sub_569560  size=65   (IDA 9.3 decompile, AC4BFSP.exe.i64, 2026-11-27, verbatim)
char __cdecl sub_569560(int a1, int a2, int a3, int a4, int a5)
{
  int v5; // eax
  _DWORD *v6; // esi

  v5 = (*(int (__thiscall **)(int, int, int))(*(_DWORD *)dword_2AC1E88 + 132))(dword_2AC1E88, a3, a1);
  v6 = (_DWORD *)v5;
  if ( v5 )
  {
    sub_79C6C0(a2, a4, a5);
    LOBYTE(v5) = sub_79B8D0(v6);
  }
  return v5;
}
