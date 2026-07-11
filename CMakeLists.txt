# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.

project(paper_color)
  fatfs_create_spiflash_image(storage main/apps/local_photo_slideshow/images FLASH_IN_PROJECT)
