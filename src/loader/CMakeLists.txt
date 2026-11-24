cmake_minimum_required(VERSION 3.16)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
project(deathsmiles C)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -std=gnu17 -mthumb -ffixed-r9 -Wall -Wno-unused-function")
add_executable(deathsmiles src/main.c src/loader.c src/imports.c src/log.c src/debugScreen.c src/objc.c src/foundation.c src/collections.c src/uikit.c src/gl.c src/threads.c src/files.c src/audio.c src/md5.c src/misc.c src/cg.c src/input.c src/archive.c src/font.c src/storekit.c src/convert.c)
target_link_libraries(deathsmiles vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub SceKernelDmacMgr_stub SceGxm_stub SceAppMgr_stub SceCommonDialog_stub SceSysmodule_stub stdc++ kubridge_stub SceCtrl_stub SceDisplay_stub SceSysmem_stub SceIofilemgr_stub SceAudio_stub SceTouch_stub ScePower_stub pthread m)
vita_create_self(eboot.bin deathsmiles UNSAFE)
vita_create_vpk(Deathsmiles.vpk DSMS00001 eboot.bin VERSION 01.00 NAME "Deathsmiles"
  FILE sce_sys/icon0.png sce_sys/icon0.png FILE sce_sys/pic0.png sce_sys/pic0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE res/LiberationSans-Bold.ttf res/LiberationSans-Bold.ttf FILE res/ipag.ttf res/ipag.ttf
  FILE sce_sys/manual/001.png sce_sys/manual/001.png
  FILE sce_sys/manual/002.png sce_sys/manual/002.png
  FILE sce_sys/manual/003.png sce_sys/manual/003.png
  FILE sce_sys/manual/004.png sce_sys/manual/004.png
  FILE sce_sys/manual/005.png sce_sys/manual/005.png
  FILE sce_sys/manual/006.png sce_sys/manual/006.png
  FILE sce_sys/manual/007.png sce_sys/manual/007.png
  FILE sce_sys/manual/008.png sce_sys/manual/008.png
  FILE sce_sys/manual/009.png sce_sys/manual/009.png
  FILE sce_sys/manual/010.png sce_sys/manual/010.png
  FILE sce_sys/manual/011.png sce_sys/manual/011.png
  FILE sce_sys/manual/012.png sce_sys/manual/012.png
  FILE sce_sys/manual/013.png sce_sys/manual/013.png
  FILE sce_sys/manual/014.png sce_sys/manual/014.png
  FILE sce_sys/manual/015.png sce_sys/manual/015.png
  FILE sce_sys/manual/016.png sce_sys/manual/016.png
  FILE sce_sys/manual/017.png sce_sys/manual/017.png
  FILE sce_sys/manual/018.png sce_sys/manual/018.png
  FILE sce_sys/manual/019.png sce_sys/manual/019.png
  FILE sce_sys/manual/020.png sce_sys/manual/020.png
  FILE sce_sys/manual/021.png sce_sys/manual/021.png
  FILE sce_sys/manual/022.png sce_sys/manual/022.png
  FILE sce_sys/manual/023.png sce_sys/manual/023.png
  FILE sce_sys/manual/024.png sce_sys/manual/024.png
  FILE sce_sys/manual/025.png sce_sys/manual/025.png
  FILE sce_sys/manual/026.png sce_sys/manual/026.png
  FILE sce_sys/manual/027.png sce_sys/manual/027.png
  FILE bezels/bezel_windia.png bezels/bezel_windia.png
  FILE bezels/bezel_casper.png bezels/bezel_casper.png
  FILE bezels/bezel_follett.png bezels/bezel_follett.png
  FILE bezels/bezel_rosa.png bezels/bezel_rosa.png
  FILE bezels/bezel_tiara.png bezels/bezel_tiara.png
  FILE bezels/bezel_menu.png bezels/bezel_menu.png
  FILE res/LICENSE-Liberation.txt res/LICENSE-Liberation.txt FILE res/LICENSE-IPAfont.txt res/LICENSE-IPAfont.txt)
