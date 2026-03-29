CC      = x86_64-w64-mingw32-gcc
WINDRES = x86_64-w64-mingw32-windres
VERSION = $(shell git describe --tags --dirty 2>/dev/null || echo dev)
CFLAGS  = -O2 -Wall -Wextra -mwindows -municode -DVERSION_STRING=L\"$(VERSION)\"
LDFLAGS = -lshell32 -luser32 -lkernel32 -ladvapi32 -lwininet
TARGET  = MonitorSwitcher.exe
SRC     = MonitorSwitcher.c
RC      = MonitorSwitcher.rc
RES_OBJ = MonitorSwitcher_res.o

$(TARGET): $(SRC) $(RES_OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(RES_OBJ) $(LDFLAGS)

$(RES_OBJ): $(RC) MonitorSwitcher.ico MonitorSwitcher.manifest
	$(WINDRES) $(RC) -o $@

clean:
	rm -f $(TARGET) $(RES_OBJ)

.PHONY: clean
