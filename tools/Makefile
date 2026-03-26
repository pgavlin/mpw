MPW = ../build/bin/mpw

MrC = $(MPW) MrC
PPCLink = $(MPW) PPCLink

COPTS = -w off

SHARED_LIBS = "{SharedLibraries}"StdCLib "{SharedLibraries}"InterfaceLib
PPC_LIBS = "{PPCLibraries}"StdCRuntime.o "{PPCLibraries}"PPCCRuntime.o "{PPCLibraries}"PPCToolLibs.o

all: Hello IoctlProbe

IoctlProbe: IoctlProbe.c.o
	rm -f $@
	$(PPCLink) $< $(SHARED_LIBS) $(PPC_LIBS) -t MPST -c 'MPS ' -o $@

Hello: Hello.c.o
	rm -f $@
	$(PPCLink) $< $(SHARED_LIBS) $(PPC_LIBS) -t MPST -c 'MPS ' -o $@

%.c.o: %.c
	$(MrC) $< $(COPTS) -o $@

clean:
	rm -f Hello Hello.c.o IoctlProbe IoctlProbe.c.o

.PHONY: all clean
