find_package(PkgConfig)

if (PREFER_SYSTEM_LIBRARIES)
	if (PREFER_PKGCONFIG)
		pkg_search_module(CURL libcurl)
	endif(PREFER_PKGCONFIG)
	if (NOT CURL_FOUND)
    	find_package(CURL REQUIRED)
	endif(NOT CURL_FOUND)
endif(PREFER_SYSTEM_LIBRARIES)
if (NOT CURL_FOUND)
    message(STATUS "Using built-in curl library")
    set(CURL_INCLUDE_DIRS "${EXTDIR}/curl/include")
    set(CURL_LIBRARY_DIRS "${EXTDIR}/curl/lib/.libs")
    set(CURL_LIBRARIES "curl")
endif(NOT CURL_FOUND)

if (PREFER_SYSTEM_LIBRARIES)
    pkg_search_module(OPENSSL openssl)
    pkg_search_module(CRYPTO libcrypto)
endif (PREFER_SYSTEM_LIBRARIES)

if (NOT OPENSSL_FOUND)
    set(OPENSSL_INCLUDE_DIRS "${EXTDIR}/openssl-1.0.2o/include")
    set(OPENSSL_LIBRARY_DIRS "${EXTDIR}/openssl-1.0.2o/")
    set(OPENSSL_LIBRARIES "ssl")
    set(CRYPTO_LIBRARIES "crypto")


