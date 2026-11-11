// 0x1719FF0  sub_1719FF0  size=72
BOOL __cdecl sub_1719FF0(int a1)
{
  int v1; // edi
  int v2; // esi
  int v3; // eax

  v1 = sub_6F6010(a1);
  v2 = sub_133A4C0(a1);
  v3 = sub_133A4C0(v1);
  return v2 && v3 && (unsigned __int8)sub_19017C0(v3, v2 + 20);
}
