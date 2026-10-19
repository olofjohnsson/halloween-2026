# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/olof-johnsson/Documents/repos/halloween-2026/stations")
  file(MAKE_DIRECTORY "/home/olof-johnsson/Documents/repos/halloween-2026/stations")
endif()
file(MAKE_DIRECTORY
  "/home/olof-johnsson/Documents/repos/halloween-2026/stations/build_1/stations"
  "/home/olof-johnsson/Documents/repos/halloween-2026/stations/build_1/_sysbuild/sysbuild/images/stations-prefix"
  "/home/olof-johnsson/Documents/repos/halloween-2026/stations/build_1/_sysbuild/sysbuild/images/stations-prefix/tmp"
  "/home/olof-johnsson/Documents/repos/halloween-2026/stations/build_1/_sysbuild/sysbuild/images/stations-prefix/src/stations-stamp"
  "/home/olof-johnsson/Documents/repos/halloween-2026/stations/build_1/_sysbuild/sysbuild/images/stations-prefix/src"
  "/home/olof-johnsson/Documents/repos/halloween-2026/stations/build_1/_sysbuild/sysbuild/images/stations-prefix/src/stations-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/olof-johnsson/Documents/repos/halloween-2026/stations/build_1/_sysbuild/sysbuild/images/stations-prefix/src/stations-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/olof-johnsson/Documents/repos/halloween-2026/stations/build_1/_sysbuild/sysbuild/images/stations-prefix/src/stations-stamp${cfgdir}") # cfgdir has leading slash
endif()
