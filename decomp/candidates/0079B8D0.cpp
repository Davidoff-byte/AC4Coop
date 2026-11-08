// 0x79B8D0  sub_79B8D0  size=74
char __thiscall sub_79B8D0(char **this)
{
  char *v2; // ecx
  int v3; // eax

  if ( !(*(unsigned __int8 (__thiscall **)(char **))*this)(this) )
    return 0;
  v2 = *(this + 3);
  *(this + 9) = (char *)1;
  *(this + 11) = nullptr;
  *(this + 10) = nullptr;
  *(this + 8) = (char *)1;
  sub_48A390(v2, (int)this);
  v3 = (*((int (__thiscall **)(char **))*this + 3))(this);
  (*(void (__thiscall **)(int, char **))(*(_DWORD *)v3 + 136))(v3, this);
  return 1;
}
