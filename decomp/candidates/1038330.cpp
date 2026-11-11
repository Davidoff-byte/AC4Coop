// 0x1038330  sub_1038330  size=338 (0x152)
//
// HAND-DERIVED from disassembly, NOT decompiler output: Hex-Rays rejects this function. This file is
// therefore evidence-of-contract, not a reconstruction, and cannot be byte-matched. The contract is
// what the two call sites in sub_1913100 rely on, and what the probe uses:
//
//   void* sub_1038330(void* entity, char mode);       // __cdecl, 2 args (push 0; push eax; add esp,8)
//
//   entity == NULL                      -> returns NULL                    (0x103833a..0x1038344)
//   mode != 0                           -> fills a container by calling entity->vtbl[0x68]
//   mode == 0                           -> fills it via sub_79B5E0(entity) + sub_525590
//   then iterates that container, accepting entries whose [entry+0x0C] -> vtbl[3] -> [obj+0x64]
//   passes the sub_101A430 validity test, and returns the accepted object (or NULL).
//
// NOT established here: what class the accepted object is. That is why the probe resolves the
// runtime vtable id before anything is written into the mod.
//
// Disassembly anchors (evidence file decomp/evidence/1038330.txt):
//   1038337  mov esi,[ebp+arg_0]        ; entity
//   103834f  cmp [ebp+arg_4],0          ; mode
//   1038355  mov eax,[esi] / [eax+68h]  ; mode!=0 path: entity vtable slot +0x68
//   1038368  call sub_79B5E0            ; mode==0 path
//   1038370  call sub_525590
//   103837c  shr ecx,10h / lea eax,[ebx+ecx*4]   ; container walk
//
// Consumer (verified in the same session, sub_1913100 case 3, 0x1913173..0x1913190):
//   obj = sub_1038330(entity, 0);
//   float v = ((float (__thiscall*)(void*)) (*(void***)obj)[0x1B4/4])(obj);   // vtable slot 0x1B4
// The sibling case 4 reads 0x1B4 and 0x1B8 (GetHealth / GetMaxHealth).

typedef struct Entity Entity;

void* sub_1038330(Entity* entity, char mode)
{
    (void)entity;
    (void)mode;
    return 0;   // placeholder: this file records the observed contract, not an implementation
}
