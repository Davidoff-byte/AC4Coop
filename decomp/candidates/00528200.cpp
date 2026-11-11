// 0x528200  sub_528200  size=139
int __thiscall sub_528200(char *this, int a2, int a3)
{
  int v4; // ecx
  _BYTE *v5; // eax
  int result; // eax
  _DWORD *v7; // edi
  _DWORD *v8; // ebx
  char *v9; // esi

  sub_63C2D0(a2, a3);
  v4 = *((_DWORD *)this + 21);
  *((_DWORD *)this + 20) |= 0x20000u;
  *((_DWORD *)this + 21) = v4;
  v5 = (_BYTE *)sub_520F80((unsigned __int8 *)this + 225, (int)this);
  if ( v5 )
    sub_423470(v5);
  result = (*(int (__thiscall **)(char *, int))(*(_DWORD *)this + 128))(this, a3);
  if ( (_BYTE)a3 )
  {
    v7 = (_DWORD *)sub_5269A0(this + 219, (int)this);
    result = sub_526A60(this + 219, (int)this);
    v8 = (_DWORD *)result;
    if ( v7 != (_DWORD *)result )
    {
      v9 = this + 16;
      do
      {
        result = (*(int (__thiscall **)(_DWORD, char *))(*(_DWORD *)*v7 + 188))(*v7, v9);
        ++v7;
      }
      while ( v7 != v8 );
    }
  }
  return result;
}
