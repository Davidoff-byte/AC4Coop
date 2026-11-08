// 0x1A90920  sub_1A90920  size=123
int __usercall sub_1A90920@<eax>(_DWORD *a1@<ecx>, int a2@<ebp>)
{
  int v3; // esi
  _DWORD v4[19]; // [esp-40h] [ebp-4Ch] BYREF
  _UNKNOWN *retaddr; // [esp+Ch] [ebp+0h]

  v4[16] = a2;
  v4[17] = retaddr;
  if ( !*a1 )
    return 3;
  v3 = 3;
  sub_A00CB0(*a1, dword_4E371E4, 0);
  if ( (unsigned __int8)sub_9F6260(v4) )
  {
    do
      v3 = ((int (__thiscall *)(_DWORD))v4[12])(v4[10]);
    while ( v3 == 2 && (unsigned __int8)sub_9F6260(v4) );
  }
  sub_A01070(v4);
  return v3;
}
