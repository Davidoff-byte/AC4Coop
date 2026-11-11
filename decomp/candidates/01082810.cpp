// 0x1082810  sub_1082810  size=155  SpawnRequestAlloc   (__cdecl, returns object)
// Verified from the image at 0x1082810:
//   lock   0x2AC1FCC
//   p = 0x418A80(*(0x2AC1F68), 0xDD0, 0x10)   object of 0xDD0 bytes; the two
//                                             stack params' meaning (size/align)
//                                             is not resolved
//   if (p) 0x15666E0(p)                        construct it
//   ring insert: index = (*(u16*)0x2AC1FC2 + 1) against (*(u32*)0x2AC1FC0 & 0x3FFF),
//                table *(0x2AC1FBC), slot dword at table + idx*4 - 4 = p
//   unlock 0x2AC1FCC
//   return p
// This is an allocate-construct-enqueue factory; the object's type is NOT verified.
void *sub_1082810(void)
{
  void *p;
  unsigned int idx;
  sub_D587F0(0x2AC1FCC);
  p = (void *)sub_418A80(*(unsigned int *)0x2AC1F68, 0xDD0, 0x10);
  if (p)
    sub_15666E0(p);
  idx = *(unsigned short *)0x2AC1FC2;
  if ((idx + 1) > (*(unsigned int *)0x2AC1FC0 & 0x3FFF))
    idx = sub_A09AB0(0x2AC1FBC, idx + 1);
  idx = *(unsigned short *)0x2AC1FC2 + 1;
  *(unsigned short *)0x2AC1FC2 = (unsigned short)idx;
  *(void **)(*(unsigned int *)0x2AC1FBC + idx * 4 - 4) = p;
  sub_AC6580(0x2AC1FCC);
  return p;
}
