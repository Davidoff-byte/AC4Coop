// 0x681980  sub_681980  size=693
_QWORD *__thiscall sub_681980(unsigned int this, unsigned int a2, int a3, _DWORD *a4)
{
  int v5; // edx
  int v6; // ebx
  int v7; // eax
  int v8; // ecx
  int v9; // edi
  unsigned __int16 v10; // ax
  int v11; // ecx
  int v12; // eax
  int v14; // ecx
  int v15; // ecx
  int v16; // ecx
  _QWORD *result; // eax
  unsigned __int64 v18; // [esp+Ch] [ebp-2Ch] BYREF
  __int16 v19; // [esp+14h] [ebp-24h]
  char v20; // [esp+16h] [ebp-22h]
  int v21; // [esp+18h] [ebp-20h]
  int v22; // [esp+1Ch] [ebp-1Ch]
  int v23; // [esp+20h] [ebp-18h]
  int v24; // [esp+24h] [ebp-14h]
  int v25; // [esp+28h] [ebp-10h]
  int v26; // [esp+2Ch] [ebp-Ch]
  int v27; // [esp+30h] [ebp-8h]
  int v28; // [esp+34h] [ebp-4h]
  char v29; // [esp+4Bh] [ebp+13h]

  v5 = *(_DWORD *)(this + 264);
  v6 = (unsigned __int16)(a3 & *(_WORD *)(this + 16));
  v7 = *(_DWORD *)(a2 + 160);
  v27 = *(_DWORD *)(this + 260);
  v8 = *(_DWORD *)(this + 284);
  v26 = v5;
  sub_5D13B0((*(_QWORD *)(v8 + 80) >> 20) & 7, v7);
  v9 = *(_DWORD *)(a2 + 144);
  v10 = *(_WORD *)(this + 12);
  if ( (_WORD)v6 == 0xFFFF )
  {
    if ( (v10 & (unsigned __int16)((1 << *(_BYTE *)(*(_DWORD *)(*(_DWORD *)(this + 292) + 4) + 33)) - 1)) == (1 << *(_BYTE *)(*(_DWORD *)(*(_DWORD *)(this + 292) + 4) + 33)) - 1 )
      v28 = 0;
    else
      LOWORD(v28) = -1;
  }
  else
  {
    v28 = (unsigned __int16)(v6 & ~v10);
  }
  v11 = *(_DWORD *)(this + 276);
  if ( v11 )
    LOBYTE(v24) = sub_734460(v11);
  else
    LOBYTE(v24) = 0;
  v12 = sub_733FB0(*(_DWORD *)(this + 280));
  if ( v12 )
    LOBYTE(v25) = (*(_WORD *)(*(_DWORD *)(v12 + 680) + 44) & 0x1000) != 0;
  else
    LOBYTE(v25) = 0;
  if ( v27 )
  {
    if ( *(_DWORD *)(v27 + 144) != v9 )
      sub_69AD50(v27, v9);
  }
  else
  {
    v27 = sub_69D440(v6, v9, v24);
  }
  if ( v26 )
  {
    if ( *(_DWORD *)(v26 + 144) != v9 )
      sub_69AD50(v26, v9);
  }
  else
  {
    v26 = sub_69D440(v6, v9, v25);
  }
  switch ( *(_WORD *)(this + 252) & 7 )
  {
    case 1:
    case 6:
      (*(void (__thiscall **)(_DWORD, int, int, _DWORD *))(**(_DWORD **)(this + 276) + 28))(
        *(_DWORD *)(this + 276),
        v27,
        v6,
        a4);
      (*(void (__thiscall **)(_DWORD, int, int, _DWORD *))(**(_DWORD **)(this + 280) + 140))(
        *(_DWORD *)(this + 280),
        v26,
        v6,
        a4);
      break;
    case 2:
      if ( (_WORD)v28 )
      {
        v14 = *(_DWORD *)(this + 276);
        if ( v14 )
          (*(void (__thiscall **)(int, int, int, _DWORD *))(*(_DWORD *)v14 + 28))(v14, v27, v28, a4);
      }
      (*(void (__thiscall **)(_DWORD, int, int, _DWORD *))(**(_DWORD **)(this + 280) + 140))(
        *(_DWORD *)(this + 280),
        v26,
        v6,
        a4);
      break;
    case 3:
      (*(void (__thiscall **)(_DWORD, int, int, _DWORD *))(**(_DWORD **)(this + 276) + 28))(
        *(_DWORD *)(this + 276),
        v27,
        v6,
        a4);
      if ( (_WORD)v28 )
        goto LABEL_30;
      break;
    case 4:
      if ( (_WORD)v28 )
      {
        (*(void (__thiscall **)(_DWORD, int, int, _DWORD *))(**(_DWORD **)(this + 276) + 28))(
          *(_DWORD *)(this + 276),
          v27,
          v28,
          a4);
LABEL_30:
        (*(void (__thiscall **)(_DWORD, int, int, _DWORD *))(**(_DWORD **)(this + 280) + 140))(
          *(_DWORD *)(this + 280),
          v26,
          v28,
          a4);
      }
      break;
    default:
      break;
  }
  v15 = *(_DWORD *)(this + 276);
  v29 = 0;
  if ( v15 )
    v29 = (*(int (__thiscall **)(int))(*(_DWORD *)v15 + 200))(v15);
  v16 = *(_DWORD *)(this + 280);
  if ( v16 )
    v29 |= (*(int (__thiscall **)(int))(*(_DWORD *)v16 + 200))(v16);
  if ( v29 || *(_WORD *)(v27 + 174) || *(_WORD *)(v26 + 174) )
    sub_769330(a3);
  sub_522060(*(_DWORD *)(this + 284));
  v19 = a3;
  v22 = v26;
  v21 = v27;
  v18 = __PAIR64__(a2, this);
  v23 = 1;
  v20 = v24 & 1 | v20 & 0xF8 | (2 * (v25 & 1));
  result = sub_4AD640(a4, &v18);
  *(_WORD *)(this + 12) |= v6;
  return result;
}
