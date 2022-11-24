#ifndef SHA1_H_
#define SHA1_H_

/*
   SHA-1 in C
   By Steve Reid <steve@edmweb.com>
   100% Public Domain
 */

/* for uint32_t */
#include <stdint.h>

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buffer[64];
} SHA1_CTX;

#if defined(__cplusplus)||defined(c_plusplus)
extern "C"
{
#endif

void SHA1Transform(
    uint32_t state[5],
    const unsigned char buffer[64]
    );

void SHA1Init(
    SHA1_CTX * context
    );

void SHA1Update(
    SHA1_CTX * context,
    const unsigned char *data,
    uint32_t len
    );

void SHA1Final(
    unsigned char digest[20],
    SHA1_CTX * context
    );

#if defined(__cplusplus)||defined(c_plusplus)
}  /* end extern "C" */
#endif

#endif // SHA1_H_
