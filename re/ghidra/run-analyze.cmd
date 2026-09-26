set JAVA_HOME=C:\repos\rfg-vr\tools\re\jdk-21.0.12.1+1
set PATH=%JAVA_HOME%\bin;%PATH%
set GHIDRA_HEADLESS_MAXMEM=8G
call C:\repos\rfg-vr\tools\re\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat C:\repos\rfg-vr\re\ghidra rfg -import C:\repos\rfg-vr\game\rfg.exe -overwrite -analysisTimeoutPerFile 7200 > C:\repos\rfg-vr\re\ghidra\analyze.log 2>&1
