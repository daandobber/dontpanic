idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"main.c"
		"storage.c"
		"wiki.c"
		"wiki_zim.c"
		"html_text.c"
		"screens.c"
		"ui_theme.c"
		"hosted_reset.c"
	PRIV_REQUIRES
		esp_lcd
		fatfs
		sdmmc
		json
		nvs_flash
		bt
		badge-bsp
		zstddeclib
		${extra_requires}
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
# wiki.c builds SD card paths from directory entry names (dirent d_name,
# unbounded per POSIX) into fixed-size buffers; snprintf truncates safely,
# but GCC can't prove that statically.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format-truncation" APPEND)

# ZIM files can be larger than 2 GiB. Use 64-bit stdio offsets for fseeko/ftello
# so FAT32 files up to its 4 GiB per-file limit can be read.
target_compile_definitions(${COMPONENT_LIB} PRIVATE _FILE_OFFSET_BITS=64)
