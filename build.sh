#!/bin/bash

# Получаем папку, в которой располагается скрипт
PROJECT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BUILD_DIR="$PROJECT_DIR/build_linux"

cd "$PROJECT_DIR"

# git fetch origin
# git reset --hard origin/main

rm -rf "$BUILD_DIR"
mkdir "$BUILD_DIR"
cd "$BUILD_DIR"

# Project generation
CC=gcc CXX=g++ cmake ..

# Project compilation
make

if [ $? -eq 0 ]; then
	echo "Success!"
else
	echo "Fail!"
	exit 1
fi
