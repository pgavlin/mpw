MPW = ../build/bin/mpw

MrC = $(MPW) MrC
PPCLink = $(MPW) PPCLink

COPTS = -w off

SHARED_LIBS = "{SharedLibraries}"StdCLib "{SharedLibraries}"InterfaceLib
PPC_LIBS = "{PPCLibraries}"StdCRuntime.o "{PPCLibraries}"PPCCRuntime.o "{PPCLibraries}"PPCToolLibs.o

all: Hello IoctlProbe FileRead

FileRead: FileRead.c.o
	rm -f $@
	$(PPCLink) $< $(SHARED_LIBS) $(PPC_LIBS) -t MPST -c 'MPS ' -o $@

IoctlProbe: IoctlProbe.c.o
	rm -f $@
	$(PPCLink) $< $(SHARED_LIBS) $(PPC_LIBS) -t MPST -c 'MPS ' -o $@

Hello: Hello.c.o
	rm -f $@
	$(PPCLink) $< $(SHARED_LIBS) $(PPC_LIBS) -t MPST -c 'MPS ' -o $@

sha256.o: sha256.c
	rm -f $@
	$(MrC) -p -w off -opt speed -d TARGET_CPU=68000 sha256.c -o sha256.o

%.c.o: %.c
	$(MrC) $< $(COPTS) -o $@

clean:
	rm -f Hello Hello.c.o IoctlProbe IoctlProbe.c.o

.PHONY: all clean
