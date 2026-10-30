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

    Parallel bitslice implementation based on FFdecsa,
     Copyright (C) 2003-2004 fatih89r

    (c) 2006-2008 Alexandre Becoulet <alexandre.becoulet@free.fr>

*/

#ifndef LIBDVBCSA_H_
#define LIBDVBCSA_H_

#ifdef __cplusplus
extern "C" {
#endif

/* csa control word */
typedef unsigned char           dvbcsa_cw_t[8];

/***********************************************************************
        Single packet CSA implemetation API
 */

/* single packet implementation key context */
typedef struct dvbcsa_key_s     dvbcsa_key_t;

/** allocate a new csa key context */
struct dvbcsa_key_s * dvbcsa_key_alloc(void);

/** free a csa key context */

void dvbcsa_key_free(struct dvbcsa_key_s *key);

/** setup a csa key context to use the given control word */

void dvbcsa_key_set (const dvbcsa_cw_t cw, struct dvbcsa_key_s *key);

/** decrypt a packet payload */

void dvbcsa_decrypt (const struct dvbcsa_key_s *key,
                     unsigned char *data, unsigned int len);

/** encrypt a packet payload */

void dvbcsa_encrypt (const struct dvbcsa_key_s *key,
                     unsigned char *data, unsigned int len);



/***********************************************************************
        Parallel bitslice CSA implemetation API
 */

/** maximum allowed length of a packet */
#define DVBCSA_BS_MAX_PACKET_LEN 184

/** packets batch structure, describe each data packet payload to process */
struct dvbcsa_bs_batch_s
{
  unsigned char         *data;  /* pointer to payload */
  unsigned int          len;    /* payload bytes lenght */
};

/** parallel bitslice implementation key context */
typedef struct dvbcsa_bs_key_s  dvbcsa_bs_key_t;

/** allocate a new csa bitslice key context */

struct dvbcsa_bs_key_s * dvbcsa_bs_key_alloc(void);

/** free a csa bitslice key context */

void dvbcsa_bs_key_free(struct dvbcsa_bs_key_s *key);

/** setup a csa bitslice key context to use the given control word */

void dvbcsa_bs_key_set(const dvbcsa_cw_t cw, struct dvbcsa_bs_key_s *key);

/** get maximum number of packet per batch */

unsigned int dvbcsa_bs_batch_size(void);

/** decrypt a packet batch. batch is an array of struct
    dvbcsa_bs_batch_s with an extra NULL data termination
    entry. maxlen is the maximum data bytes lenght to process, must be
    a multiple of 8, should be 184 for TS packets. */

void dvbcsa_bs_decrypt(const struct dvbcsa_bs_key_s *key,
                       const struct dvbcsa_bs_batch_s *pcks,
                       unsigned int maxlen);

/** encrypt a packet batch. batch is an array of struct
    dvbcsa_bs_batch_s with an extra NULL data termination
    entry. maxlen is the maximum data bytes lenght to process, must be
    a multiple of 8, should be 184 for TS packets. */

void dvbcsa_bs_encrypt(const struct dvbcsa_bs_key_s *key,
                       const struct dvbcsa_bs_batch_s *pcks,
                       unsigned int maxlen);

#ifdef __cplusplus
}
#endif

#endif

