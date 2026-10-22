cmake_minimum_required(VERSION 3.24)
project(ikaruga_recomp LANGUAGES C CXX)

# The tool. dream-recomp skips its own reference titles when it is not the top-level project.
add_subdirectory(dream-recomp)
add_subdirectory(game)
