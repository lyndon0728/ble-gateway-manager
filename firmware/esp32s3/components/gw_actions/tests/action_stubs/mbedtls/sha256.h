#pragma once
#include <stddef.h>
#include "monocypher-ed25519.h"
typedef struct {crypto_sha512_ctx ctx;} mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context *);
void mbedtls_sha256_free(mbedtls_sha256_context *);
int mbedtls_sha256_starts(mbedtls_sha256_context *,int);
int mbedtls_sha256_update(mbedtls_sha256_context *,const unsigned char *,size_t);
int mbedtls_sha256_finish(mbedtls_sha256_context *,unsigned char[32]);
int mbedtls_sha256(const unsigned char *,size_t,unsigned char[32],int);
