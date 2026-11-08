// 0xA0BDE0  sub_A0BDE0  size=108
int *__thiscall sub_A0BDE0(char *this, __int64 a2, volatile signed __int32 *a3)
{
  char *v5; // esi
  volatile signed __int32 **v6; // edi

  if ( a2 || a3 )
  {
    v5 = this + 124;
    sub_D587F0(this + 124);
    v6 = sub_A0BBF0((int)this, a2, a3);
    _InterlockedExchangeAdd((volatile signed __int32 *)v6 + 1, 1u);
    if ( v5 )
      sub_AC6580(v5);
    return (int *)v6;
  }
  else
  {
    _InterlockedExchangeAdd(&dword_4DD5F90, 1u);
    return &dword_4DD5F8C;
  }
}
