@echo off
rem Usage: run-find-disp.cmd <out.txt> <hexdisp>...
set JAVA_HOME=C:\repos\rfg-vr\tools\re\jdk-21.0.12.1+1
set PATH=%JAVA_HOME%\bin;%PATH%
set GHIDRA_HEADLESS_MAXMEM=8G
call C:\repos\rfg-vr\tools\re\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat C:\repos\rfg-vr\re\ghidra rfg -process rfg.exe -noanalysis -readOnly -scriptPath C:\repos\rfg-vr\re\ghidra_scripts -postScript FindDisplacement.java %* > C:\repos\rfg-vr\re\ghidra\find-disp.log 2>&1
