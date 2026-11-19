#pragma once
#include <stddef.h>
#define MBEDTLS_MD_SHA256 1
typedef struct {int type;} mbedtls_md_info_t;
const mbedtls_md_info_t *mbedtls_md_info_from_type(int);
int mbedtls_md(const mbedtls_md_info_t *,const unsigned char *,size_t,unsigned char *);
int mbedtls_md_hmac(const mbedtls_md_info_t *,const unsigned char *,size_t,const unsigned char *,size_t,unsigned char *);
