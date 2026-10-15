# kwai: gcc or clang on Linux, macOS and MinGW (Windows). For Visual Studio,
# open the folder (CMakeLists.txt) or run build.bat in a Developer prompt.

CC     ?= cc
CFLAGS ?= -Wall -Wextra -O2

ifeq ($(OS),Windows_NT)
EXE  = kwai.exe
LIBS = -lsetupapi -lcfgmgr32
else
EXE  = kwai
LIBS = -pthread
endif

# `make test` runs kwai against LiteForth's desktop lf over ptys (Linux, macOS)
LF     ?= ../liteforth/bin/lf
BLOCKS ?= ../liteforth/bin/lfblocks.fb4

.PHONY: all test clean

all: $(EXE)

$(EXE): kwai.c
	$(CC) $(CFLAGS) -o $@ kwai.c $(LIBS)

test: $(EXE)
	python3 test/test_kwai.py ./$(EXE) $(LF) $(BLOCKS)

clean:
	rm -f kwai kwai.exe
