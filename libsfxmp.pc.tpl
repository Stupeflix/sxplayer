prefix=PREFIX
exec_prefix=${prefix}
includedir=${prefix}/include
libdir=${exec_prefix}/lib

Name: sfxmp
Description: Stupeflix Media Player library
Version: 5.3.0
Cflags: -I${includedir}
Libs: -L${libdir} -lsfxmp DEP_LIBS
Libs.private: DEP_PRIVATE_LIBS
