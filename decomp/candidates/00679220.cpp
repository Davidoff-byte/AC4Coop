// 0x679220  sub_679220  size=84
bool __thiscall sub_679220(void *this)
{
  float v2; // xmm1_4

  if ( (*((_WORD *)this + 126) & 7) == 0 )
    return 1;
  if ( (*((_WORD *)this + 126) & 7) == 5 && (*((_BYTE *)this + 21) & 4) == 0 )
    return 0;
  if ( (*((_BYTE *)this + 21) & 2) != 0 )
    return 1;
  v2 = *((float *)this + 1);
  return *(float *)this > v2 || fabs(*(float *)this - v2) <= 0.00050000002;
}
