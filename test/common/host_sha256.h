#pragma once
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
using mbedtls_sha256_context = CC_SHA256_CTX;
inline int mbedtls_sha256_starts(mbedtls_sha256_context *c, int) { return !CC_SHA256_Init(c); }
inline int mbedtls_sha256_update(mbedtls_sha256_context *c, const void *p, size_t n) { return !CC_SHA256_Update(c, p, n); }
inline int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char *out) { return !CC_SHA256_Final(out, c); }
#else
#include <openssl/sha.h>
using mbedtls_sha256_context = SHA256_CTX;
inline int mbedtls_sha256_starts(mbedtls_sha256_context *c, int) { return !SHA256_Init(c); }
inline int mbedtls_sha256_update(mbedtls_sha256_context *c, const void *p, size_t n) { return !SHA256_Update(c, p, n); }
inline int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char *out) { return !SHA256_Final(out, c); }
#endif
inline void mbedtls_sha256_init(mbedtls_sha256_context *) {}
inline void mbedtls_sha256_free(mbedtls_sha256_context *) {}
