// 0x527BA0  ListNodeAcquireAndTag  size=117
//
// Role, as far as the body itself shows (no engine-intent claim):
//   sub_A2D5A0(0, 1)                 - enter a guard/lock (exact semantics unverified)
//   (*(this+12))(this, 0, 0)         - vtbl[3]; the returned pointer is the interesting value
//   sub_A2F320()                     - the matching leave/release
//   out = node; node[20] flags |= 4 | 8 | 0x800000
//   node[21] is read then written back unchanged (bitfield read-modify-write)
//   if node[23] != 0 => scale = *(float*)(node[23] + 1832) else scale = 1.0f
//   return sub_522610(node, 0, scale * scale)
//
// It allocates nothing, names no class, and writes the node pointer through a2.
// Earlier name `EntityBuilderInstantiate` was a claim the body does not support:
// there is no allocation, no class name and no builder call here.
int __thiscall ListNodeAcquireAndTag(void *this, _DWORD *a2)
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
  v3[20] |= 0x800000u;
  v3[21] = v4;
  v5 = v3[23];
  if ( v5 )
    v6 = *(float *)(v5 + 1832);
  else
    v6 = 1.0;
  return sub_522610((int)v3, 0, COERCE_INT(v6 * v6));
}
