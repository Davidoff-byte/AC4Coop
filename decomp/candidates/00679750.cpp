// 0x679750  sub_679750  size=303
int __thiscall sub_679750(int this, int a2)
{
  int v3; // edi
  unsigned __int16 v4; // ax
  unsigned __int16 v5; // cx
  int result; // eax
  int v7; // ecx

  v3 = (unsigned __int16)(a2 & *(_WORD *)(this + 18));
  v4 = *(_WORD *)(this + 14);
  if ( (_WORD)v3 != 0xFFFF )
  {
    result = (unsigned __int16)(v3 & ~v4);
    goto LABEL_6;
  }
  v5 = (1 << *(_BYTE *)(*(_DWORD *)(*(_DWORD *)(this + 292) + 4) + 33)) - 1;
  if ( (v4 & v5) == v5 )
  {
    result = 0;
LABEL_6:
    LOWORD(a2) = result;
    goto LABEL_7;
  }
  LOWORD(a2) = -1;
  result = a2;
LABEL_7:
  switch ( *(_WORD *)(this + 252) & 7 )
  {
    case 1:
    case 6:
      if ( *(float *)(this + 8) < 0.5 )
        (*(void (__thiscall **)(_DWORD, int))(**(_DWORD **)(this + 276) + 40))(*(_DWORD *)(this + 276), v3);
      goto LABEL_10;
    case 2:
      if ( *(float *)(this + 8) < 0.5 )
      {
        if ( (_WORD)a2 )
        {
          v7 = *(_DWORD *)(this + 276);
          if ( v7 )
            (*(void (__thiscall **)(int, int))(*(_DWORD *)v7 + 40))(v7, result);
        }
      }
LABEL_10:
      result = (*(int (__thiscall **)(_DWORD, int))(**(_DWORD **)(this + 280) + 152))(*(_DWORD *)(this + 280), v3);
      *(_WORD *)(this + 14) |= v3;
      return result;
    case 3:
      if ( *(float *)(this + 8) < 0.5 )
        result = (*(int (__thiscall **)(_DWORD, int))(**(_DWORD **)(this + 276) + 40))(*(_DWORD *)(this + 276), v3);
      goto LABEL_21;
    case 4:
    case 5:
      if ( *(float *)(this + 8) < 0.5 && (_WORD)a2 )
        result = (*(int (__thiscall **)(_DWORD, int))(**(_DWORD **)(this + 276) + 40))(*(_DWORD *)(this + 276), result);
      goto LABEL_21;
    default:
LABEL_21:
      *(_WORD *)(this + 14) |= v3;
      return result;
  }
}
