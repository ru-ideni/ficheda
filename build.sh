#!/bin/bash

cmake -C ./bin/cmake_install.cmake -S ./ -B ./bin

cmake --build ./bin --target ficheda
