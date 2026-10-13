@echo off
REM Build Swiss inside Docker on Windows, no Git Bash required.
REM Usage: docker\build.bat [make target]   (default: full release build)
REM Examples:
REM   docker\build.bat        -^> swiss_r*\ release package for every boot medium
REM   docker\build.bat dev    -^> quick dev build (cube\swiss\swiss.dol)
pushd %~dp0..

where docker >nul 2>nul
if errorlevel 1 (
	echo docker not found: install Docker Desktop ^(https://www.docker.com/products/docker-desktop^)
	goto fail
)

docker info >nul 2>nul
if errorlevel 1 (
	echo Docker daemon isn't answering, trying to start Docker Desktop...
	if exist "%ProgramFiles%\Docker\Docker\Docker Desktop.exe" start "" "%ProgramFiles%\Docker\Docker\Docker Desktop.exe"
	set /a tries=60
	:waitloop
		docker info >nul 2>nul
		if not errorlevel 1 goto ready
		timeout /t 2 /nobreak >nul
		set /a tries-=1
		if %tries% gtr 0 goto waitloop
	echo Docker still isn't up; start Docker Desktop and run this again.
	goto fail
)

:ready
docker build -t swiss-gc-build docker
if errorlevel 1 goto fail
docker run --rm -v "%CD%:/swiss" swiss-gc-build sh -c "make %*"
if errorlevel 1 goto fail

popd
exit /b 0

:fail
popd
exit /b 1
