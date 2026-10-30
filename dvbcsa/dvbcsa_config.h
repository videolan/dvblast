#ifndef DVBCSA_CONFIG_H_
#define DVBCSA_CONFIG_H_

#define STDC_HEADERS 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_ATTRIBUTE_MAY_ALIAS 1

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define DVBCSA_ENDIAN_BIG 1
#else
#define DVBCSA_ENDIAN_LITTLE 1
#endif

#endif
