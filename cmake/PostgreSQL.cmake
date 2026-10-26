option(FSNET_POSTGRES_TEST_SERVER "Build a private PostgreSQL server for integration tests" OFF)
find_program(FSNET_BISON_EXECUTABLE bison REQUIRED)
find_program(FSNET_FLEX_EXECUTABLE flex REQUIRED)
find_program(FSNET_M4_EXECUTABLE m4 REQUIRED)
set(pg_env ${CMAKE_COMMAND} -E env "M4=${FSNET_M4_EXECUTABLE}")
set(FSNET_POSTGRES_ROOT "${CMAKE_BINARY_DIR}/postgres-install")
file(MAKE_DIRECTORY "${FSNET_POSTGRES_ROOT}/include")
if(FSNET_POSTGRES_TEST_SERVER)
  set(pg_build ${pg_env} ${FSNET_MAKE_EXECUTABLE} -C src/backend generated-headers COMMAND ${pg_env} ${FSNET_MAKE_EXECUTABLE} -j4)
  set(pg_install ${FSNET_MAKE_EXECUTABLE} install)
else()
  set(pg_build ${pg_env} ${FSNET_MAKE_EXECUTABLE} -C src/backend generated-headers COMMAND ${pg_env} ${FSNET_MAKE_EXECUTABLE} -j4 -C src/interfaces/libpq)
  set(pg_install ${FSNET_MAKE_EXECUTABLE} -C src/interfaces/libpq install
    COMMAND ${FSNET_MAKE_EXECUTABLE} -C src/common install
    COMMAND ${FSNET_MAKE_EXECUTABLE} -C src/port install)
endif()
ExternalProject_Add(fsnet-postgres
  URL https://codeload.github.com/postgres/postgres/tar.gz/refs/tags/REL_17_11
  URL_HASH SHA256=8b222af249d957ecec23608fe848d2dce03246121ac9b67bafb081226ea3c535
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
    "CFLAGS=-O2 -fPIC" "CPPFLAGS=-I${FSNET_CRYPTO_ROOT}/include" "LDFLAGS=-L${FSNET_CRYPTO_ROOT}/lib -Wl,-rpath,${FSNET_CRYPTO_ROOT}/lib" "LIBS=-ldl -lpthread"
    <SOURCE_DIR>/configure --prefix=${FSNET_POSTGRES_ROOT} --with-ssl=openssl --without-readline --without-zlib --without-icu
    --disable-rpath --without-gssapi --without-ldap --without-lz4 --without-zstd --without-libxml --without-libxslt --without-pam
  BUILD_COMMAND ${pg_build}
  INSTALL_COMMAND ${pg_install}
  BUILD_BYPRODUCTS ${FSNET_POSTGRES_ROOT}/lib/libpq.a ${FSNET_POSTGRES_ROOT}/lib/libpgcommon_shlib.a ${FSNET_POSTGRES_ROOT}/lib/libpgport_shlib.a
  DEPENDS fsnet-openssl
  LOG_DOWNLOAD ON LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON)
foreach(component pq pgcommon pgport)
  set(pg_archive ${component})
  if(NOT component STREQUAL "pq")
    set(pg_archive ${component}_shlib)
  endif()
  add_library(fsnet-${component} STATIC IMPORTED GLOBAL)
  set_target_properties(fsnet-${component} PROPERTIES IMPORTED_LOCATION "${FSNET_POSTGRES_ROOT}/lib/lib${pg_archive}.a"
    INTERFACE_INCLUDE_DIRECTORIES "${FSNET_POSTGRES_ROOT}/include")
  add_dependencies(fsnet-${component} fsnet-postgres)
endforeach()
set_target_properties(fsnet-pq PROPERTIES INTERFACE_LINK_LIBRARIES "fsnet-pgcommon;fsnet-pgport;fsnet-ssl;m")
