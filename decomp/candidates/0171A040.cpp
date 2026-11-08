// 0x171A040  sub_171A040  size=76
BOOL __cdecl sub_171A040(int a1)
{
  int v1; // edi
  int v2; // esi
  int v3; // eax

  v1 = sub_6F6010(a1);
  v2 = sub_133A4C0(a1);
  v3 = sub_133A4C0(v1);
  return v2 && v3 && (v3 == v2 || (unsigned __int8)sub_19017C0(v3, v2 + 12));
}
