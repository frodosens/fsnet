include(ExternalProject)
find_package(Perl REQUIRED)
find_program(FSNET_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
set(FSNET_OPENSSL_TARGET "" CACHE STRING "Explicit OpenSSL Configure target (normally detected from the CMake target)")
set(_fsnet_crypto_target "${FSNET_OPENSSL_TARGET}")
set(_fsnet_crypto_flags no-shared no-tests no-module no-legacy)
if(MINGW)
  if(NOT _fsnet_crypto_target)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_fsnet_crypto_target mingw64)
    else()
      set(_fsnet_crypto_target mingw)
    endif()
  endif()
elseif(MSVC)
  message(FATAL_ERROR "The Windows fsnet SDK currently uses MinGW-w64; configure with the documented MSYS2 toolchain")
elseif(APPLE)
  if(NOT _fsnet_crypto_target)
    if(CMAKE_OSX_ARCHITECTURES)
      list(LENGTH CMAKE_OSX_ARCHITECTURES _fsnet_arch_count)
      if(NOT _fsnet_arch_count EQUAL 1)
        message(FATAL_ERROR "Build each macOS fsnet SDK architecture separately")
      endif()
      set(_fsnet_arch "${CMAKE_OSX_ARCHITECTURES}")
    else()
      set(_fsnet_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(_fsnet_arch STREQUAL "arm64" OR _fsnet_arch STREQUAL "aarch64")
      set(_fsnet_crypto_target darwin64-arm64)
    elseif(_fsnet_arch STREQUAL "x86_64" OR _fsnet_arch STREQUAL "AMD64")
      set(_fsnet_crypto_target darwin64-x86_64)
    else()
      message(FATAL_ERROR "Unsupported macOS fsnet SDK architecture: ${_fsnet_arch}")
    endif()
  endif()
  list(APPEND _fsnet_crypto_flags -fPIC)
else()
  list(APPEND _fsnet_crypto_flags -fPIC)
endif()
set(FSNET_CRYPTO_ROOT "${CMAKE_BINARY_DIR}/openssl-install")
file(MAKE_DIRECTORY "${FSNET_CRYPTO_ROOT}/include")
ExternalProject_Add(fsnet-openssl
  URL https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/openssl-3.5.9
  URL_HASH SHA256=796831ec533bf41998eaaee9d632d921b8a69f57394fa4086ff01a941113d26d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  CONFIGURE_COMMAND ${PERL_EXECUTABLE} <SOURCE_DIR>/Configure
    ${_fsnet_crypto_target} --prefix=${FSNET_CRYPTO_ROOT} --libdir=lib ${_fsnet_crypto_flags}
  BUILD_COMMAND ${FSNET_MAKE_EXECUTABLE} -j4 build_libs
  INSTALL_COMMAND ${FSNET_MAKE_EXECUTABLE} install_dev
  BUILD_BYPRODUCTS ${FSNET_CRYPTO_ROOT}/lib/libssl.a ${FSNET_CRYPTO_ROOT}/lib/libcrypto.a
  LOG_DOWNLOAD ON LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON)
add_library(fsnet-crypto STATIC IMPORTED GLOBAL)
set_target_properties(fsnet-crypto PROPERTIES IMPORTED_LOCATION "${FSNET_CRYPTO_ROOT}/lib/libcrypto.a"
  INTERFACE_INCLUDE_DIRECTORIES "${FSNET_CRYPTO_ROOT}/include"
  INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}")
add_dependencies(fsnet-crypto fsnet-openssl)
if(WIN32)
  set_property(TARGET fsnet-crypto APPEND PROPERTY INTERFACE_LINK_LIBRARIES ws2_32 crypt32 advapi32 bcrypt user32)
endif()
add_library(fsnet-ssl STATIC IMPORTED GLOBAL)
set_target_properties(fsnet-ssl PROPERTIES IMPORTED_LOCATION "${FSNET_CRYPTO_ROOT}/lib/libssl.a"
  INTERFACE_INCLUDE_DIRECTORIES "${FSNET_CRYPTO_ROOT}/include"
  INTERFACE_LINK_LIBRARIES fsnet-crypto)
add_dependencies(fsnet-ssl fsnet-openssl)
