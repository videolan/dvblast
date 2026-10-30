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

#include "dvbcsa.h"
#include "dvbcsa_pv.h"

void dvbcsa_decrypt (const struct dvbcsa_key_s *key, uint8_t *data, unsigned int len)
{
  unsigned int  alen = len & (unsigned)~0x7;
  unsigned int  i;

  if (len < 8)
    return;

#ifndef DVBCSA_DISABLE_STREAM
  dvbcsa_stream_xor(key->cws, data, data + 8, len - 8);
#endif

#ifndef DVBCSA_DISABLE_BLOCK
  dvbcsa_block_decrypt(key->sch, data, data);

  for (i = 8; i < alen; i += 8)
    {
      dvbcsa_xor_64(data + i - 8, data + i);
      dvbcsa_block_decrypt(key->sch, data + i, data + i);
    }
#endif
}

void dvbcsa_encrypt (const struct dvbcsa_key_s *key, uint8_t *data, unsigned int len)
{
  unsigned int  alen = len & (unsigned)~0x7;
  int       i;

  if (len < 8)
    return;

#ifndef DVBCSA_DISABLE_BLOCK
  dvbcsa_block_encrypt(key->sch, data + alen - 8, data + alen - 8);

  for (i = alen - 16; i >= 0; i -= 8)
    {
      dvbcsa_xor_64(data + i, data + i + 8);
      dvbcsa_block_encrypt(key->sch, data + i, data + i);
    }
#endif

#ifndef DVBCSA_DISABLE_STREAM
  dvbcsa_stream_xor(key->cws, data, data + 8, len - 8);
#endif
}

struct dvbcsa_key_s * dvbcsa_key_alloc(void)
{
  return (struct dvbcsa_key_s *)malloc(sizeof (struct dvbcsa_key_s));
}

void dvbcsa_key_free(struct dvbcsa_key_s *key)
{
  free(key);
}

