cmake_minimum_required(VERSION 3.16)

# 板级支持包（bsp 组件）取自 FoloToy/ai-passport 子模块，只用它的 components/，不用它的 main/。
set(CC_UPSTREAM "${CMAKE_CURRENT_LIST_DIR}/third_party/ai-passport")
if(NOT EXISTS "${CC_UPSTREAM}/components/bsp/CMakeLists.txt")
    message(FATAL_ERROR "缺少子模块 third_party/ai-passport，请在仓库根目录执行：git submodule update --init")
endif()
set(EXTRA_COMPONENT_DIRS "${CC_UPSTREAM}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(claude-chan-badge)
