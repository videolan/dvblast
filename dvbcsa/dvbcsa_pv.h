/*

    This file is part of libdvbcsa.

    libdvbcsa is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published
    by the Free Software Foundation; either version 2 of the License,
    or (at your option) any later version.

    libdvbcsa is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libdvbcsa; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
    02111-1307 USA

    (c) 2006-2008 Alexandre Becoulet <alexandre.becoulet@free.fr>

*/

#ifndef DVBCSA_PV_H_
# define DVBCSA_PV_H_

#include "dvbcsa_config.h"

#if STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# if HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif
#if HAVE_STRING_H
# if !STDC_HEADERS && HAVE_MEMORY_H
#  include <memory.h>
# endif
# include <string.h>
#endif
#if HAVE_STRINGS_H
# include <strings.h>
#endif
#if HAVE_INTTYPES_H
# include <inttypes.h>
#else
# if HAVE_STDINT_H
#  include <stdint.h>
# endif
#endif

#if !defined(DVBCSA_DEBUG) && defined(__GNUC__)
#define DVBCSA_INLINE __attribute__ ((always_inline))
#else
#define DVBCSA_INLINE
#endif

/** types used for type-punning */
#ifdef HAVE_ATTRIBUTE_MAY_ALIAS
typedef uint8_t  __attribute__((may_alias)) dvbcsa_u8_aliasing_t;
typedef uint16_t __attribute__((may_alias)) dvbcsa_u16_aliasing_t;
typedef uint32_t __attribute__((may_alias)) dvbcsa_u32_aliasing_t;
typedef uint64_t __attribute__((may_alias)) dvbcsa_u64_aliasing_t;
#else
typedef uint8_t  dvbcsa_u8_aliasing_t;
typedef uint16_t dvbcsa_u16_aliasing_t;
typedef uint32_t dvbcsa_u32_aliasing_t;
typedef uint64_t dvbcsa_u64_aliasing_t;
#endif

void worddump (const char *str, const void *data, size_t len, size_t ws);

#define DVBCSA_DATA_SIZE        8
#define DVBCSA_KEYSBUFF_SIZE    56
#define DVBCSA_CWBITS_SIZE      64

typedef uint8_t                 dvbcsa_block_t[DVBCSA_DATA_SIZE];
typedef uint8_t                 dvbcsa_keys_t[DVBCSA_KEYSBUFF_SIZE];

struct dvbcsa_key_s
{
  dvbcsa_cw_t           cw;
  dvbcsa_cw_t           cws;    /* nibble swapped CW */
  dvbcsa_keys_t         sch;
};

extern const uint8_t dvbcsa_block_sbox[256];

void dvbcsa_block_decrypt (const dvbcsa_keys_t key, const dvbcsa_block_t in, dvbcsa_block_t out);
void dvbcsa_block_encrypt (const dvbcsa_keys_t key, const dvbcsa_block_t in, dvbcsa_block_t out);

void dvbcsa_stream_xor (const dvbcsa_cw_t cw, const dvbcsa_block_t iv,
                        uint8_t *stream, unsigned int len);

void dvbcsa_key_schedule_block(const dvbcsa_cw_t cw, uint8_t * kk);

DVBCSA_INLINE static inline void
dvbcsa_xor_64 (uint8_t *b, const uint8_t *a)
{
  uint64_t ia, ib;
  memcpy(&ia, a, 8);
  memcpy(&ib, b, 8);
  ib ^= ia;
  memcpy(b, &ib, 8);
}

DVBCSA_INLINE static inline uint32_t
dvbcsa_load_le32(const uint8_t *p)
{
#if defined(DVBCSA_ENDIAN_LITTLE)
  uint32_t i;
  memcpy(&i, p, 4);
  return i;
#else
  return ((uint32_t)p[3] << 24) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[1] << 8 ) |
          (uint32_t)p[0];
#endif
}

DVBCSA_INLINE static inline uint64_t
dvbcsa_load_le64(const uint8_t *p)
{
#if defined(DVBCSA_ENDIAN_LITTLE)
  uint64_t i;
  memcpy(&i, p, 8);
  return i;
#else
  return (uint64_t)( ((uint64_t)p[7] << 56) |
                     ((uint64_t)p[6] << 48) |
                     ((uint64_t)p[5] << 40) |
                     ((uint64_t)p[4] << 32) |
                     ((uint64_t)p[3] << 24) |
                     ((uint64_t)p[2] << 16) |
                     ((uint64_t)p[1] << 8 ) |
                      (uint64_t)p[0]
                     );
#endif
}

DVBCSA_INLINE static inline void
dvbcsa_store_le64(uint8_t *p, const uint64_t w)
{
#if defined(DVBCSA_ENDIAN_LITTLE)
  memcpy(p, &w, 8);
#else
  p[7] = (w >> 56);
  p[6] = (w >> 48);
  p[5] = (w >> 40);
  p[4] = (w >> 32);
  p[3] = (w >> 24);
  p[2] = (w >> 16);
  p[1] = (w >> 8);
  p[0] = (w);
#endif
}


#endif

