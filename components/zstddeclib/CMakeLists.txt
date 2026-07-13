# Single-file Zstandard decompressor (decompress-only), vendored from
# https://github.com/facebook/zstd/tree/dev/build/single_file_libs
#
# zstddeclib.c was generated with:
#   python combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c
# from an unmodified checkout of the zstd repository. See LICENSE/COPYING for
# the original project's dual BSD/GPLv2 licensing.

idf_component_register(
    SRCS
        "zstddeclib.c"
    INCLUDE_DIRS
        "include"
)

# The amalgamated file is third-party code we don't want to lint/warn on.
idf_component_get_property(lib ${COMPONENT_NAME} COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -w)
