# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "E:/ESP-IDF/master/v5.1/esp-idf/components/bootloader/subproject"
  "D:/my_project/ai_mirror/Firmware/esp32_ai_mirror/build/bootloader"
  "D:/my_project/ai_mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix"
  "D:/my_project/ai_mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/tmp"
  "D:/my_project/ai_mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/src/bootloader-stamp"
  "D:/my_project/ai_mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/src"
  "D:/my_project/ai_mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/my_project/ai_mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/my_project/ai_mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
