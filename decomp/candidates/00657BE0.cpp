// 0x657BE0  sub_657BE0  size=47
int __thiscall sub_657BE0(int this)
{
  int result; // eax
  int v2; // esi
  int i; // edi

  result = *(unsigned __int16 *)(this + 14);
  v2 = *(_DWORD *)(this + 8);
  for ( i = v2 + 4 * result; v2 != i; v2 += 4 )
  {
    result = **(_DWORD **)v2 & (*(int *)(*(_DWORD *)v2 + 8) >> 31);
    if ( result )
      result = (*(int (__thiscall **)(int))(*(_DWORD *)result + 60))(**(_DWORD **)v2 & (*(int *)(*(_DWORD *)v2 + 8) >> 31));
  }
  return result;
}
