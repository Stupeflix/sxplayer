prefix=PREFIX
exec_prefix=${prefix}
includedir=${prefix}/include
libdir=${exec_prefix}/lib

Name: sfxmp
Description: Stupeflix Media Player library
Version: 1.0.0
Cflags: -I${includedir}
Libs: -L${libdir} -lsfxmp DEP_LIBS
