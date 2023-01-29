#!/bin/bash

set -euo pipefail

if (( $# != 1 )); then
	echo Script requires one argument - the clang version to be installed
	exit 1
fi

if ! which $CC >/dev/null 2>&1; then
	case $DISTRO in
		"ubuntu-22.04") distro_name=jammy;;
		"ubuntu-20.04") distro_name=focal;;
		*)
		echo "Unknown distribution $DISTRO"
		exit 1
	esac
	case $1 in
		"14" | "15") llvm_version=$1;;
		*)
		echo "Unknown llvm version $1"
		exit 1
	esac

	sources="deb [trusted=yes] http://apt.llvm.org/$distro_name/ llvm-toolchain-$distro_name-$llvm_version main"

	echo "clang-$llvm_version missed in the image, installing from llvm"
	echo "$sources" | sudo tee -a /etc/apt/sources.list
	sudo apt-get update
	sudo apt-get install -y --no-install-recommends clang-$llvm_version
fi

