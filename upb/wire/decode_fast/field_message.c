// Protocol Buffers - Google's data interchange format
// Copyright 2025 Google LLC.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "upb/mem/arena.h"
#include "upb/message/message.h"
#include "upb/mini_table/message.h"
#include "upb/wire/decode.h"
#include "upb/wire/decode_fast/cardinality.h"
#include "upb/wire/decode_fast/combinations.h"
#include "upb/wire/decode_fast/dispatch.h"
#include "upb/wire/decode_fast/field_parsers.h"
#include "upb/wire/eps_copy_input_stream.h"
#include "upb/wire/internal/decoder.h"

// Must be last.
#include "upb/port/def.inc"

UPB_INLINE
upb_Message* decode_newmsg_ceil(upb_Decoder* d, const upb_MiniTable* m,
                                upb_DecodeFast_Type type) {
  size_t size = m->UPB_PRIVATE(size);
  char* msg_data;
  size_t msg_ceil_bytes = upb_DecodeFast_MessageCeilingBytes(type);
  if (UPB_LIKELY(type != kUpb_DecodeFast_MessageBig &&
                 UPB_PRIVATE(_upb_ArenaHas)(&d->arena) >= msg_ceil_bytes)) {
    UPB_ASSERT(size <= (size_t)msg_ceil_bytes);
    msg_data = d->arena.UPB_PRIVATE(ptr);
    // TODO: fix for Xsan
    d->arena.UPB_PRIVATE(ptr) += size;
    memset(msg_data, 0, msg_ceil_bytes);
  } else {
    msg_data = (char*)upb_Arena_Malloc(&d->arena, size);
    memset(msg_data, 0, size);
  }
  return (upb_Message*)msg_data;
}

typedef struct {
  intptr_t table;
  upb_Message* msg;
} fastdecode_submsgdata;

UPB_FORCEINLINE
const char* fastdecode_tosubmsg(upb_EpsCopyInputStream* e, const char* ptr,
                                void* ctx) {
  upb_Decoder* d = (upb_Decoder*)e;
  fastdecode_submsgdata* submsg = ctx;
  ptr = fastdecode_dispatch(d, ptr, submsg->msg, submsg->table, 0, 0);
  UPB_ASSUME(ptr != NULL);
  return ptr;
}

#define FASTDECODE_SUBMSG(d, ptr, msg, table, hasbits, data, type, card,  \
                          tagsize)                                        \
  int tagbytes = upb_DecodeFast_TagSizeBytes(tagsize);                    \
                                                                          \
  if (UPB_UNLIKELY(!fastdecode_checktag(data, tagbytes))) {               \
    RETURN_GENERIC("submessage field tag mismatch\n");                    \
  }                                                                       \
                                                                          \
  if (--d->depth == 0) {                                                  \
    _upb_FastDecoder_ErrorJmp(d, kUpb_DecodeStatus_MaxDepthExceeded);     \
  }                                                                       \
                                                                          \
  upb_Message** dst;                                                      \
  uint32_t submsg_idx = (data >> 16) & 0xff;                              \
  const upb_MiniTable* tablep = decode_totablep(table);                   \
  const upb_MiniTable* subtablep =                                        \
      UPB_PRIVATE(_upb_MiniTable_GetSubTableByIndex)(tablep, submsg_idx); \
  fastdecode_submsgdata submsg = {decode_totable(subtablep)};             \
  fastdecode_arr farr;                                                    \
                                                                          \
  if (subtablep->UPB_PRIVATE(table_mask) == (uint8_t)-1) {                \
    d->depth++;                                                           \
    RETURN_GENERIC("submessage doesn't have fast tables.");               \
  }                                                                       \
                                                                          \
  dst = fastdecode_getfield(d, ptr, msg, &data, &hasbits, &farr,          \
                            sizeof(upb_Message*), card);                  \
                                                                          \
  if (card == kUpb_DecodeFast_Scalar) {                                   \
    upb_DecodeFast_SetHasbits(msg, hasbits);                              \
    hasbits = 0;                                                          \
  }                                                                       \
                                                                          \
  again:                                                                  \
  if (card == kUpb_DecodeFast_Repeated) {                                 \
    dst = fastdecode_resizearr(d, dst, &farr, sizeof(upb_Message*));      \
  }                                                                       \
                                                                          \
  submsg.msg = *dst;                                                      \
                                                                          \
  if (card == kUpb_DecodeFast_Repeated || UPB_LIKELY(!submsg.msg)) {      \
    *dst = submsg.msg = decode_newmsg_ceil(d, subtablep, type);           \
  }                                                                       \
                                                                          \
  ptr += tagbytes;                                                        \
  ptr = fastdecode_delimited(d, ptr, fastdecode_tosubmsg, &submsg);       \
                                                                          \
  if (UPB_UNLIKELY(ptr == NULL || d->end_group != DECODE_NOGROUP)) {      \
    _upb_FastDecoder_ErrorJmp(d, kUpb_DecodeStatus_Malformed);            \
  }                                                                       \
                                                                          \
  if (card == kUpb_DecodeFast_Repeated) {                                 \
    fastdecode_nextret ret = fastdecode_nextrepeated(                     \
        d, dst, &ptr, &farr, data, tagbytes, sizeof(upb_Message*));       \
    switch (ret.next) {                                                   \
      case FD_NEXT_SAMEFIELD:                                             \
        dst = ret.dst;                                                    \
        goto again;                                                       \
      case FD_NEXT_OTHERFIELD:                                            \
        d->depth++;                                                       \
        data = ret.tag;                                                   \
        UPB_MUSTTAIL return _upb_FastDecoder_TagDispatch(UPB_PARSE_ARGS); \
      case FD_NEXT_ATLIMIT:                                               \
        d->depth++;                                                       \
        return ptr;                                                       \
    }                                                                     \
  }                                                                       \
                                                                          \
  d->depth++;                                                             \
  UPB_MUSTTAIL return fastdecode_dispatch(UPB_PARSE_ARGS);

#define F(type, card, tagsize)                                                 \
  const char* UPB_DECODEFAST_FUNCNAME(type, card, tagsize)(UPB_PARSE_PARAMS) { \
    FASTDECODE_SUBMSG(d, ptr, msg, table, hasbits, data,                       \
                      kUpb_DecodeFast_##type, kUpb_DecodeFast_##card,          \
                      kUpb_DecodeFast_##tagsize);                              \
  }

UPB_DECODEFAST_CARDINALITIES(UPB_DECODEFAST_TAGSIZES, F, Message64)
UPB_DECODEFAST_CARDINALITIES(UPB_DECODEFAST_TAGSIZES, F, Message128)
UPB_DECODEFAST_CARDINALITIES(UPB_DECODEFAST_TAGSIZES, F, Message192)
UPB_DECODEFAST_CARDINALITIES(UPB_DECODEFAST_TAGSIZES, F, Message256)
UPB_DECODEFAST_CARDINALITIES(UPB_DECODEFAST_TAGSIZES, F, MessageBig)

#undef F
#undef FASTDECODE_SUBMSG
