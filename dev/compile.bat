@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /O1 /MT /std:c++17 /EHsc /GR- /utf-8 /W3 /DUNICODE /D_UNICODE /Fobuild\ /Fe..\release\1.4.0\LLcl.exe src\*.cpp /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF user32.lib comdlg32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib comctl32.lib advapi32.lib shlwapi.lib uuid.lib wininet.lib ws2_32.lib
