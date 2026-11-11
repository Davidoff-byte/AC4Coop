// 0xFB3220  sub_FB3220  size=338   name withheld: semantics not established
// Signature is stack-based - reads arg1 at [ebp+8], arg2 (char) at [ebp+0xC] -
// i.e. __cdecl/__stdcall, NOT __thiscall (the call site at phase1.cpp:7833/7838
// casts it to __thiscall, so this call shape does not match the callee).
// Verified control flow:
//   if (!arg1) return 0
//   build a temp at [ebp-0x10] (sub_FA3AA0(temp, 1))
//   if (arg2)  vcall arg1->vtbl[0x68/4](&temp)
//   else       sub_525590(arg1, sub_79B5E0(&temp))
//   iterate the container at [ebp-0x10] (count >> 16, 8-byte records) and call a
//     global's vtbl[0x14/4] per record
//   return arg1
int __cdecl sub_FB3220(void *a1, char a2)
{
  int tmp[4];
  if (!a1)
    return 0;
  sub_FA3AA0(tmp, 1);
  if (a2)
    (*(void (__thiscall **)(void *, int *))(*(unsigned int *)a1 + 0x68))(a1, tmp);
  else
    sub_525590(a1, (void *)sub_79B5E0(tmp));
  /* container walk over tmp[0]/tmp[1] (see 0xFB3265..0xFB3355) */
  return (int)a1;
}
