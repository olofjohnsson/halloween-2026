# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/olof-johnsson/Documents/repos/halloween/candy-dispenser")
  file(MAKE_DIRECTORY "/home/olof-johnsson/Documents/repos/halloween/candy-dispenser")
endif()
file(MAKE_DIRECTORY
  "/home/olof-johnsson/Documents/repos/halloween/candy-dispenser/build/candy-dispenser"
  "/home/olof-johnsson/Documents/repos/halloween/candy-dispenser/build/_sysbuild/sysbuild/images/candy-dispenser-prefix"
  "/home/olof-johnsson/Documents/repos/halloween/candy-dispenser/build/_sysbuild/sysbuild/images/candy-dispenser-prefix/tmp"
  "/home/olof-johnsson/Documents/repos/halloween/candy-dispenser/build/_sysbuild/sysbuild/images/candy-dispenser-prefix/src/candy-dispenser-stamp"
  "/home/olof-johnsson/Documents/repos/halloween/candy-dispenser/build/_sysbuild/sysbuild/images/candy-dispenser-prefix/src"
  "/home/olof-johnsson/Documents/repos/halloween/candy-dispenser/build/_sysbuild/sysbuild/images/candy-dispenser-prefix/src/candy-dispenser-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/olof-johnsson/Documents/repos/halloween/candy-dispenser/build/_sysbuild/sysbuild/images/candy-dispenser-prefix/src/candy-dispenser-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/olof-johnsson/Documents/repos/halloween/candy-dispenser/build/_sysbuild/sysbuild/images/candy-dispenser-prefix/src/candy-dispenser-stamp${cfgdir}") # cfgdir has leading slash
endif()
