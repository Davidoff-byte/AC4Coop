// 0x503330  sub_503330  size=322  ShipEntityCtor   (__thiscall, returns this)
// Verified from the image at 0x503330:
//   call 0x52A4A0           base-class ctor (this already in ecx)
//   mov  [esi], 0x1E4A128   vtable - the exact vtable phase1.cpp:1732 expects
//   one-time copy of 0x40 bytes of static data (0x1E49060..0x1E49090) into
//   0x2ABE5A0.., gated on byte 0x2ABE5E0 (init-once flag)
//   those four 16-byte blocks are then stored at this+0x100, +0x110, +0x120, +0x130
//   word at this+0x148 = 0
// PARTIAL: the identity of the static data blocks is not established.
int __thiscall sub_503330(void *this)
{
  sub_52A4A0();
  *(unsigned int *)this = 0x1E4A128u;
  if (!(*(unsigned char *)0x2ABE5E0 & 1))
  {
    *(unsigned int *)0x2ABE5E0 |= 1u;
    copy16((void *)0x2ABE5A0, (const void *)0x1E49060);
    copy16((void *)0x2ABE5B0, (const void *)0x1E49070);
    copy16((void *)0x2ABE5C0, (const void *)0x1E49080);
    copy16((void *)0x2ABE5D0, (const void *)0x1E49090);
  }
  copy16((char *)this + 0x100, (const void *)0x2ABE5A0);
  copy16((char *)this + 0x110, (const void *)0x2ABE5B0);
  copy16((char *)this + 0x120, (const void *)0x2ABE5C0);
  copy16((char *)this + 0x130, (const void *)0x2ABE5D0);
  *(unsigned short *)((char *)this + 0x148) = 0;
  return (int)this;
}
