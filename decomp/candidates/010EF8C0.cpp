// 0x10EF8C0  sub_10EF8C0  size=72
void *__cdecl sub_10EF8C0(int a1)
{
  int v1; // eax
  void **v2; // esi
  void **v3; // edi

  v1 = sub_6F5F70(a1);
  if ( !v1 )
    return nullptr;
  v2 = *(void ***)(v1 + 92);
  v3 = &v2[*(unsigned __int16 *)(v1 + 98)];
  if ( v2 == v3 )
    return nullptr;
  while ( sub_A1D0D0(*v2) != 1588608236 )
  {
    if ( ++v2 == v3 )
      return nullptr;
  }
  return *v2;
}
