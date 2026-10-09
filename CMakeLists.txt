cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One component lock file per target: the P4 and S3 dependency sets differ (esp_hosted etc. are
# P4-only), so a shared dependencies.lock would be re-solved and rewritten by every build of the
# other target.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
set(PROJECT_VER "2.4.1")
project(wyze-esp-bridge)
