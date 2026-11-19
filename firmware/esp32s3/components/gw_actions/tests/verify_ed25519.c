/* Host-only verifier: protocol is public key[32] || signature[64] || message.
 * Test driver signs independently using Python cryptography/OpenSSL. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "monocypher-ed25519.h"
int main(int argc,char **argv){
    if(argc!=2)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    uint8_t *bytes=malloc(65632);if(!bytes){fclose(f);return 2;}
    size_t n=fread(bytes,1,65632,f);
    int extra=fgetc(f);fclose(f);if(n<96||extra!=EOF){free(bytes);return 2;}
    int rc=crypto_ed25519_check(bytes+32,bytes,bytes+96,n-96);free(bytes);return rc?1:0;
}
