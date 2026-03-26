# StdCLib __initialize Annotated Flow

Traced using `pef_inspect` on StdCLib PPC container.

## Entry: __initialize (code offset 0xF41C)

```
*(RTOC+0x1C08) = 0              ; clear some global
r31 = *(RTOC-0x18)              ; → &__C_phase
*__C_phase = 1                   ; phase = 1

call shared_init_helper (0xFD40)
  if (result != 0) return result

call _RTInit(0, 0, 0, 0, 0)     ; code 0xEE98
  if (result == -2821) return    ; gestaltUndefSelectorErr = OK

call save_emulator_state (0xF3D0)
  r31 = *(RTOC-0x14) → 12-byte struct
  *(r31+0) = GetZone()          ; save current zone
  *(r31+4) = GetEmulatorRegister(13)  ; save 68K A5 register
  *(r31+8) = 0

*__C_phase = 4                   ; phase = 4
return 0
```

## shared_init_helper (code offset 0xFD40)

```
r31 = *(RTOC-4)                  ; → &_IntEnv (StdCLib's copy of the MPGM info block)
r30 = RTOC+0x1BCC               ; → MPGM cache variable

call CFM_connection_init (0xEBEC)
  args: (stack_buf, &StandAlone, &_IntEnv+2, &_IntEnv+6, &_IntEnv+10)
  if (result != 0) return -2821

r9 = *(r30)                     ; MPGM header ptr (set by CFM_connection_init)
if (r9 == 0) goto standalone_exit
r9 = *(r9+4)                    ; → info block
if (r9 == 0) return -2821

compare _IntEnv magic ('SH') with info magic
if (mismatch) return -2821

*** KEY: *(info+0x24) = &_IntEnv   ; StdCLib writes exit chain pointer!
*** KEY: *(_IntEnv+0x24) = info    ; _IntEnv gets pointer back to info block

r6 = *(info+0x20)               ; device table from MPGM
if (r6 == 0) goto standalone_exit

allocate 0x78 (120) bytes       ; NewPtr for device table copy
*(_IntEnv+0x20) = new_dev_table ; _IntEnv gets its own device table
copy 24 bytes (first device entry) from info device table to _IntEnv device table
copy 24 bytes (second device entry)
... (copies the FSYS, ECON, SYST entries)

... (continues with IO table copy, more init)
```

## CFM_connection_init (code offset 0xEBEC)

```
r20 = RTOC+0x1BD0               ; → StandAlone flag
r27 = *(RTOC-0x38)              ; → pointer to MPGM cache var at RTOC+0x1BCC

call NGetTrapAddress(0xA1AD)    ; check Gestalt trap exists
store result

r12 = *(0x0316)                 ; READ MPGM HEADER FROM LOW MEMORY
*(r27) = r12                    ; cache it (so RTOC+0x1BCC now has MPGM ptr)

if (r12 == 0):
  StandAlone = 1; skip to standalone path

; Validate MPGM structure:
r5 = *(r12)                     ; first word of MPGM header
compare r5 to 'MPGM' (0x4D50474D)
if (mismatch) StandAlone = 1

r8 = *(r12+4)                   ; info block pointer
if (r8 == 0) StandAlone = 1

r4 = *(r8+0)                    ; first halfword of info = 'SH' magic
if (r4 == 0) StandAlone = 1

if all checks pass:
  StandAlone = 0                ; we're under MPW shell

if StandAlone:
  *r27 = 0                     ; clear MPGM cache
  store argc=1 to &_IntEnv+2
  return

; Non-standalone path:
; Read argc/argv/envp from info block into _IntEnv
*(r28) = *(info+0x02)          ; argc → _IntEnv+2
*(r29) = *(info+0x06)          ; argv → _IntEnv+6
*(r31) = *(info+0x0A)          ; envp → _IntEnv+10

; Walk startup entry list at info+0x28
r31 = *(info+0x28)
if (r31 == 0) skip
check for 'strt' tag, then walk entries looking for 'getv', 'setv', 'syst'

; Store StandAlone to caller's pointer
return 0
```

## Key Findings

### 1. Exit chain is set up BY StdCLib, not by us

StdCLib's shared_init_helper writes `&_IntEnv` into `info+0x24`:
```
*(info+0x24) = &_IntEnv
```
We do NOT need to set this up in Phase 6. StdCLib handles it.

### 2. Device table is copied from MPGM

StdCLib reads `info+0x20` (device table pointer), allocates a copy, and copies all entries. So we just need to ensure the MPGM device table (`MPW::Init()` at mpw.cpp) has PPC-callable TVectors for the ECON device handlers instead of 68K F-trap addresses.

### 3. IO table is also copied

Similarly, StdCLib copies the IO table from `info+0x1C`. The cookies and device pointers must be correct in the MPGM IO table before StdCLib init runs.

### 4. StandAlone detection works

The 0x0316 → 'MPGM' → info → 'SH' chain is validated correctly. Our `MPW::Init()` sets this up and StdCLib finds it.

### 5. Startup entry list (info+0x28) is optional

If NULL, it's skipped. Not needed for Hello World.

### 6. __C_phase progression

- Before init: 0
- After start: 1
- After _RTInit: 4
- The tool's __start checks __C_phase to decide whether to call _BreakPoint

### 7. GetEmulatorRegister(13) saves A5

The init saves GetZone() and GetEmulatorRegister(13) for later restoration. Register 13 = A5 (68K application globals pointer). Returning 0 is fine.
