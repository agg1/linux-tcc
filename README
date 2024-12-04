Linux-2.4 can be compiled with tinycc compiler:
- AoT with contained Kbuild.sh
- JiT with https://bellard.org/tcc/tccboot.html

Kbuild.sh contains further comments.
linux-2.4.37 folder contains kernel sources with all necessary patches applied
from patches/kernel folder.

A recent tinycc compiler is required, tested with mob branch commit
085e029f08c9b0b57632703df565efdbe2cd0c7f november 2024.

Currently it is recommended to execute Kbuild.sh on a native x86 build host
instead of using tcc as cross-compiler accross different architectures, because
unknown side-effects could not be confirmed which occured when residing on an
aarch32 buildhost for testing compilation towards x86 target.

See https://lists.nongnu.org/archive/html/tinycc-devel for related discussions:
https://lists.nongnu.org/archive/html/tinycc-devel/2024-11/msg00008.html
https://lists.nongnu.org/archive/html/tinycc-devel/2024-11/msg00019.html
https://lists.nongnu.org/archive/html/tinycc-devel/2024-11/msg00020.html

With the local test setup the patched linux-2.4.37.11 sources compiled, linked
and too booted successfully with nosmp option, which it is by default currently.

With plans for a complete i486-tcc-linux-musl.iso distribution this project
will be incorporated into system integration gradually.

