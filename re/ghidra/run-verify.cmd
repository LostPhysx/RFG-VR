@echo off
rem Run after run-analyze.cmd has finished. Re-opens the analyzed project read-only and runs VerifyHookMap.
set JAVA_HOME=C:\repos\rfg-vr\tools\re\jdk-21.0.12.1+1
set PATH=%JAVA_HOME%\bin;%PATH%
set GHIDRA_HEADLESS_MAXMEM=8G
call C:\repos\rfg-vr\tools\re\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat C:\repos\rfg-vr\re\ghidra rfg -process rfg.exe -noanalysis -readOnly -scriptPath C:\repos\rfg-vr\re\ghidra_scripts -postScript VerifyHookMap.java > C:\repos\rfg-vr\re\ghidra\verify.log 2>&1
