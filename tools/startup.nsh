@echo -off
echo [PINIT] HV+Drain dispatcher v3

# Drain mode: if drain.flag is present on any boot FS, run HypeDrain
# from that FS and remove the flag. Loop completes naturally; control
# falls through into the normal HV+Windows path below afterward.
for %d in fs0 fs1 fs2 fs3 fs4 fs5 fs6 fs7
  if exist %d:\drain.flag then
    echo [PINIT] drain.flag on %d -- running HypeDrain
    %d:\HypeDrain.efi
    rm %d:\drain.flag
  endif
endfor

echo [PINIT] Searching for PlatformInit.efi...
for %d in fs0 fs1 fs2 fs3 fs4 fs5 fs6 fs7
  if exist %d:\EFI\Boot\PlatformInit.efi then
    echo [PINIT] Found driver on %d
    load %d:\EFI\Boot\PlatformInit.efi
    goto findwin
  endif
endfor
echo [PINIT] WARNING: PlatformInit.efi not found - booting without HV
:findwin
echo [PINIT] Searching for Windows Boot Manager...
for %w in fs0 fs1 fs2 fs3 fs4 fs5 fs6 fs7
  if exist %w:\EFI\Microsoft\Boot\bootmgfw.efi then
    echo [PINIT] Found Windows on %w
    %w:\EFI\Microsoft\Boot\bootmgfw.efi
  endif
endfor
echo [PINIT] ERROR: bootmgfw.efi not found
pause
