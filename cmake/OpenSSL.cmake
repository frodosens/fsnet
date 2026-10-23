include(ExternalProject)
find_package(Perl REQUIRED)
find_program(FSNET_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
set(FSNET_CRYPTO_ROOT "${CMAKE_BINARY_DIR}/openssl-install")
file(MAKE_DIRECTORY "${FSNET_CRYPTO_ROOT}/include")
ExternalProject_Add(fsnet-openssl
  URL https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/openssl-3.5.9
  URL_HASH SHA256=796831ec533bf41998eaaee9d632d921b8a69f57394fa4086ff01a941113d26d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  CONFIGURE_COMMAND ${PERL_EXECUTABLE} <SOURCE_DIR>/Configure
    --prefix=${FSNET_CRYPTO_ROOT} --libdir=lib no-shared no-tests no-module no-legacy -fPIC
  BUILD_COMMAND ${FSNET_MAKE_EXECUTABLE} -j4 build_libs
  INSTALL_COMMAND ${FSNET_MAKE_EXECUTABLE} install_dev
  BUILD_BYPRODUCTS ${FSNET_CRYPTO_ROOT}/lib/libssl.a ${FSNET_CRYPTO_ROOT}/lib/libcrypto.a
  LOG_DOWNLOAD ON LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON)
add_library(fsnet-crypto STATIC IMPORTED GLOBAL)
set_target_properties(fsnet-crypto PROPERTIES IMPORTED_LOCATION "${FSNET_CRYPTO_ROOT}/lib/libcrypto.a"
  INTERFACE_INCLUDE_DIRECTORIES "${FSNET_CRYPTO_ROOT}/include"
  INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}")
add_dependencies(fsnet-crypto fsnet-openssl)
add_library(fsnet-ssl STATIC IMPORTED GLOBAL)
set_target_properties(fsnet-ssl PROPERTIES IMPORTED_LOCATION "${FSNET_CRYPTO_ROOT}/lib/libssl.a"
  INTERFACE_INCLUDE_DIRECTORIES "${FSNET_CRYPTO_ROOT}/include"
  INTERFACE_LINK_LIBRARIES fsnet-crypto)
add_dependencies(fsnet-ssl fsnet-openssl)
