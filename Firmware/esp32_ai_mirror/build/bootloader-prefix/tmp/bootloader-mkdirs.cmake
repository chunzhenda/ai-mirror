# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "E:/file/ESP-IDF/components/bootloader/subproject"
  "D:/UGit/ai-mirror/Firmware/esp32_ai_mirror/build/bootloader"
  "D:/UGit/ai-mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix"
  "D:/UGit/ai-mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/tmp"
  "D:/UGit/ai-mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/src/bootloader-stamp"
  "D:/UGit/ai-mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/src"
  "D:/UGit/ai-mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/UGit/ai-mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/UGit/ai-mirror/Firmware/esp32_ai_mirror/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
