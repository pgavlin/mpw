MPW Emulator
------------

by Kelvin W Sherlock, _et alia_

Please check the [wiki](https://github.com/ksherlock/mpw/wiki/_pages) for useful information.

Please check the [releases](https://github.com/ksherlock/mpw/releases) for compiled binaries.


## System compatibility

macOS 10.8+ and Linux are supported. Both case-sensitive and case-insensitive filesystems (HFS+, APFS, ext4) work correctly — the emulator performs case-insensitive path resolution at the application level. On Linux, resource forks and Finder Info are stored as AppleDouble sidecar files.

## License

CPU emulation for both 68K and PowerPC uses [Unicorn Engine](https://www.unicorn-engine.org/) (LGPL v2).
Disassembly uses [Capstone](https://www.capstone-engine.org/) (BSD).

The memory allocator (NewHandle/NewPointer) code is from [mempoolite](https://github.com/jeftyneg/mempoolite),
which is a fork of the SQLite zero-alloc allocator by Jefty Negapatan and D. Richard Hipp.  It, as is SQLite,
is in the public domain.

All other code is BSD 2-Clause.

## Building

First initialize and fetch submodules:

    git submodule init
    git submodule update

Compiling requires cmake, ragel (version 6), lemon, and a recent version of clang++ with
c++11 support. Also requires libedit for the debugger.

    mkdir build
    cd build
    cmake ..
    make

This will generate `bin/mpw` (emulator), `bin/disasm` (disassembler), and `bin/mpw-lsp` (language server).

## Installation

Certain configuration and execution files are generally useful.  They are
stored in an mpw directory, which may be located:

    $MPW (shell variable)
    ~/mpw/ (your home directory)
    /usr/local/share/mpw/
    /usr/share/mpw/

The layout is reminiscent of actual MPW installations.

    mpw/Environment.text
    mpw/Tools/...
    mpw/Interfaces/...
    mpw/Libraries/...
    mpw/Help/...

## Environment file

The Environment.text file is new; it contains MPW environment variables (many
of them set the library and include file locations).  The format is fairly 
simple.

    # this is a comment
    
    #this sets a variable
    name = value
    
    # this sets a variable if it is undefined.
    name ?= value
    
    # values may refer to other variables
    Libraries=$MPW:Libraries:Libraries:
    Libraries=${MPW}:Libraries:Libraries:
    



## Usage

`mpw [mpw flags] command-name [command arguments]`

### Profiling

Use `--profile` to generate execution traces in callgrind format, viewable in KCachegrind/QCachegrind:

    mpw --profile Echo hello              # writes callgrind.out.<pid>
    mpw --profile --profile-output=trace.cg Echo hello   # custom filename

The trace captures function-level and instruction-level cycle data with a call graph.
Function names are resolved from MacsBug debug symbols and Toolbox trap names.

### LSP Server

`mpw-lsp` is a Language Server Protocol server for MPW development tools. It translates
MPW compiler error output into standard LSP diagnostics, providing IDE integration for
classic Mac development. See the `lsp/` directory for details.

### Shell Aliases

you may also create shell aliases:

`alias AsmIIgs='mpw AsmIIgs'`

or create a shell script (in `/usr/local/bin`, etc)

`/usr/local/bin/AsmIIgs`:

    #!/bin/sh
    
    exec mpw AsmIIgs "$@"


mpw uses the MPW `$Commands` variable to find the command, similar to `$PATH` on Unix.  If the `$Commands` variable
is not set, mpw looks in the current directory and then in the `$MPW:Tools:` directory.

