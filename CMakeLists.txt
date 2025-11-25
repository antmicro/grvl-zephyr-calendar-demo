cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(grvl-demo)

target_sources(app PRIVATE src/main.cpp)

if(CONFIG_SDCARD_IMAGE)
  find_program(MKE2FS NAMES mke2fs mkfs.ext2 REQUIRED)
  find_program(TRUNCATE NAMES truncate REQUIRED)

  set(SDCARD_GENSCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/scripts/sdcard.sh)
  set(SDCARD_IMG_NAME zephyr.sdcard.img)
  set(SDCARD_IMG_FILE ${APPLICATION_BINARY_DIR}/zephyr/${SDCARD_IMG_NAME})

  file(GLOB_RECURSE ROMFS_FILES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/romfs/*")

  add_custom_command(
    OUTPUT ${SDCARD_IMG_FILE}
    COMMAND ${SDCARD_GENSCRIPT} ${CONFIG_SDCARD_IMAGE_SIZE}
            ${CMAKE_CURRENT_SOURCE_DIR}/romfs ${APPLICATION_BINARY_DIR}
    DEPENDS ${SDCARD_GENSCRIPT} ${ROMFS_FILES}
    COMMENT "Generating SD card image ${SDCARD_IMG_NAME}")

  add_custom_target(sdcard DEPENDS ${SDCARD_IMG_FILE})

  add_dependencies(app sdcard)
endif() # CONFIG_SDCARD_IMAGE
