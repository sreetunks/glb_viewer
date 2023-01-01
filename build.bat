@echo off
cls
if not exist "build" mkdir build
pushd build
set CompilerOptions=/nologo /cgthreads8 /GS- /Gs9999999 /J /TC /std:c11
set CompilerOptions=%CompilerOptions% /Od
set CompilerOptions=%CompilerOptions% /I"..\include" /DUNICODE
set CompilerOptions=%CompilerOptions% /Zi /Fd:"glb_viewer.pdb" /Zl /c
set InputFiles="..\source\main.c"
set InputFiles=%InputFiles% "..\source\utils.c"
set InputFiles=%InputFiles% "..\source\jsmn.c"
set CompilerOptions=%CompilerOptions% %InputFiles%
cl %CompilerOptions%
set LinkerOptions=/NOLOGO /MACHINE:X64
set LinkerOptions=%LinkerOptions% /DEBUG:FULL /PDB:"glb_viewer.pdb"
set LinkerOptions=%LinkerOptions% /ENTRY:main /NODEFAULTLIB
set LinkerOptions=%LinkerOptions% /SUBSYSTEM:WINDOWS /OUT:"glb_viewer.exe"
link %LinkerOptions% "kernel32.lib" "user32.lib" "shell32.lib" "*.obj"
popd