#!/bin/sh
# Build Swiss inside Docker, using the same image as the CI. Works the same on
# Linux, macOS and Windows (Git Bash / MSYS2; Windows users can instead run
# docker\build.bat from cmd.exe or PowerShell).
#
# Usage: docker/build.sh [make target]    (default: full release build)
# Examples:
#   docker/build.sh        -> swiss_r*/ release package for every boot medium
#   docker/build.sh dev    -> quick dev build (cube/swiss/swiss.dol)
set -eu
cd "$(dirname "$0")/.."

if ! command -v docker >/dev/null 2>&1; then
	echo >&2 "docker not found: install Docker Desktop (https://www.docker.com/products/docker-desktop)"
	exit 1
fi

# Docker Desktop on Windows/macOS runs docker inside a VM that has to be
# started first; on Linux ask systemd for the daemon
if ! docker info >/dev/null 2>&1; then
	echo "Docker daemon isn't answering, trying to start it..." >&2
	case "$(uname -s)" in
		Darwin)
			open -a Docker 2>/dev/null || true ;;
		MINGW*|MSYS*|CYGWIN*)
			for exe in "$PROGRAMFILES/Docker/Docker/Docker Desktop.exe" "$LOCALAPPDATA/Docker/Docker/Docker Desktop.exe"; do
				[ -f "$exe" ] && { start "" "$exe" || true; break; }
			done ;;
		Linux)
			sudo systemctl start docker 2>/dev/null || true ;;
	esac
	printf "waiting for Docker"
	for _ in $(seq 60); do docker info >/dev/null 2>&1 && break; printf "."; sleep 2; done
	printf "\n"
	docker info >/dev/null 2>&1 || { echo >&2 "Docker still isn't up; start Docker Desktop and run this again."; exit 1; }
fi

# Hand docker a path it understands. Git Bash/MSYS would otherwise rewrite
# POSIX-looking arguments (like the /swiss mount point) into Windows paths,
# and docker wants a drive-letter path for the source mount there.
case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*)
		export MSYS_NO_PATHCONV=1
		export MSYS2_ARG_CONV_EXCL="*"
		SRCDIR=$(pwd -W) ;;
	*)
		SRCDIR=$PWD ;;
esac

# Only Linux leaves container-owned files behind; Docker Desktop on
# Windows/macOS maps ownership back to the invoking user by itself
FIXUP="true"
[ "$(uname -s)" = Linux ] && FIXUP="chown -R $(id -u):$(id -g) /swiss"

docker build -t swiss-gc-build docker
docker run --rm -v "$SRCDIR:/swiss" swiss-gc-build sh -c "make $* && $FIXUP"
