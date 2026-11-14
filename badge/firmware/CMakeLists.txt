cmake_minimum_required(VERSION 3.16)

# 板级支持包（bsp 组件）取自 FoloToy/ai-passport 子模块，只用它的 components/，不用它的 main/。
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/third_party/ai-passport/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(claude-chan-badge)
