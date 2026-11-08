// 0x1371140  sub_1371140  size=80
void *__thiscall sub_1371140(_DWORD *this)
{
  int v2; // eax
  void **v3; // esi
  void **v4; // edi
  void *v6; // esi

  if ( *(this + 22) )
    return (void *)*(this + 22);
  v2 = *(this + 3);
  v3 = *(void ***)(v2 + 112);
  v4 = &v3[*(unsigned __int16 *)(v2 + 118)];
  if ( v3 == v4 )
  {
LABEL_5:
    *(this + 22) = 0;
    return nullptr;
  }
  else
  {
    while ( sub_A1D0D0(*v3) != 1475276487 )
    {
      if ( ++v3 == v4 )
        goto LABEL_5;
    }
    v6 = *v3;
    *(this + 22) = v6;
    return v6;
  }
}
