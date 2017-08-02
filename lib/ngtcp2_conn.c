/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "ngtcp2_conn.h"

#include <string.h>
#include <assert.h>

#include "ngtcp2_upe.h"
#include "ngtcp2_ppe.h"
#include "ngtcp2_pkt.h"
#include "ngtcp2_macro.h"

/*
 * conn_local_stream returns nonzero if |stream_id| indicates that it
 * is the stream initiated by local endpoint.
 */
static int conn_local_stream(ngtcp2_conn *conn, uint32_t stream_id) {
  if (conn->server) {
    return stream_id % 2 == 0;
  }
  return stream_id % 2 != 0;
}

/*
 * id_from_stream_id translates |stream_id| to id space used by
 * ngtcp2_idtr.
 */
static uint64_t id_from_stream_id(uint32_t stream_id) {
  if (stream_id & 1) {
    return (stream_id - 1) / 2;
  }
  return (stream_id - 2) / 2;
}

static int conn_call_recv_pkt(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd) {
  int rv;

  if (!conn->callbacks.recv_pkt) {
    return 0;
  }

  rv = conn->callbacks.recv_pkt(conn, hd, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_recv_frame(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd,
                                const ngtcp2_frame *fr) {
  int rv;

  if (!conn->callbacks.recv_frame) {
    return 0;
  }

  rv = conn->callbacks.recv_frame(conn, hd, fr, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_send_pkt(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd) {
  int rv;

  if (!conn->callbacks.send_pkt) {
    return 0;
  }

  rv = conn->callbacks.send_pkt(conn, hd, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_send_frame(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd,
                                const ngtcp2_frame *fr) {
  int rv;

  if (!conn->callbacks.send_frame) {
    return 0;
  }

  rv = conn->callbacks.send_frame(conn, hd, fr, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_handshake_completed(ngtcp2_conn *conn) {
  int rv;

  if (!conn->callbacks.handshake_completed) {
    return 0;
  }

  rv = conn->callbacks.handshake_completed(conn, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_recv_stream_data(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                      uint8_t fin, const uint8_t *data,
                                      size_t datalen) {
  int rv;

  if (!conn->callbacks.recv_stream_data) {
    return 0;
  }

  rv = conn->callbacks.recv_stream_data(conn, strm->stream_id, fin, data,
                                        datalen, conn->user_data,
                                        strm->stream_user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_new(ngtcp2_conn **pconn, uint64_t conn_id, uint32_t version,
                    const ngtcp2_conn_callbacks *callbacks, void *user_data) {
  int rv;
  ngtcp2_mem *mem = ngtcp2_mem_default();

  *pconn = ngtcp2_mem_calloc(mem, 1, sizeof(ngtcp2_conn));
  if (*pconn == NULL) {
    rv = NGTCP2_ERR_NOMEM;
    goto fail_conn;
  }

  (*pconn)->strm0 = ngtcp2_mem_malloc(mem, sizeof(ngtcp2_strm));
  if ((*pconn)->strm0 == NULL) {
    rv = NGTCP2_ERR_NOMEM;
    goto fail_strm0_malloc;
  }
  /* TODO Initial max_stream_data for stream 0? */
  rv = ngtcp2_strm_init((*pconn)->strm0, 0, NGTCP2_STRM_FLAG_NONE,
                        NGTCP2_STRM0_MAX_STREAM_DATA,
                        NGTCP2_STRM0_MAX_STREAM_DATA, NULL, mem);
  if (rv != 0) {
    goto fail_strm0_init;
  }

  rv = ngtcp2_map_init(&(*pconn)->strms, mem);
  if (rv != 0) {
    goto fail_strms_init;
  }

  rv = ngtcp2_map_insert(&(*pconn)->strms, &(*pconn)->strm0->me);
  if (rv != 0) {
    goto fail_strms_insert;
  }

  rv = ngtcp2_idtr_init(&(*pconn)->local_idtr, mem);
  if (rv != 0) {
    goto fail_local_idtr_init;
  }

  rv = ngtcp2_idtr_init(&(*pconn)->remote_idtr, mem);
  if (rv != 0) {
    goto fail_remote_idtr_init;
  }

  ngtcp2_acktr_init(&(*pconn)->acktr);

  ngtcp2_rtb_init(&(*pconn)->rtb, mem);

  (*pconn)->callbacks = *callbacks;
  (*pconn)->conn_id = conn_id;
  (*pconn)->version = version;
  (*pconn)->mem = mem;
  (*pconn)->user_data = user_data;

  return 0;

fail_remote_idtr_init:
  ngtcp2_idtr_free(&(*pconn)->local_idtr);
fail_local_idtr_init:
fail_strms_insert:
  ngtcp2_map_free(&(*pconn)->strms);
fail_strms_init:
  ngtcp2_strm_free((*pconn)->strm0);
fail_strm0_init:
  ngtcp2_mem_free(mem, (*pconn)->strm0);
fail_strm0_malloc:
  ngtcp2_mem_free(mem, *pconn);
fail_conn:
  return rv;
}

int ngtcp2_conn_client_new(ngtcp2_conn **pconn, uint64_t conn_id,
                           uint32_t version,
                           const ngtcp2_conn_callbacks *callbacks,
                           const ngtcp2_settings *settings, void *user_data) {
  int rv;

  rv = conn_new(pconn, conn_id, version, callbacks, user_data);
  if (rv != 0) {
    return rv;
  }

  (*pconn)->state = NGTCP2_CS_CLIENT_INITIAL;
  (*pconn)->local_settings = *settings;
  (*pconn)->max_remote_stream_id = settings->max_stream_id;
  (*pconn)->unsent_max_rx_offset_high = (*pconn)->max_rx_offset_high =
      settings->max_data;

  /* TODO Since transport parameters are not required for interop now,
     just supply sensible default here.  Remove this when transport
     parameter gets mandatory. */
  (*pconn)->remote_settings.max_stream_data = 64 * 1024;
  (*pconn)->remote_settings.max_data = 64;
  (*pconn)->remote_settings.max_stream_id = 1;

  (*pconn)->max_tx_offset_high = (*pconn)->remote_settings.max_data;

  return 0;
}

int ngtcp2_conn_server_new(ngtcp2_conn **pconn, uint64_t conn_id,
                           uint32_t version,
                           const ngtcp2_conn_callbacks *callbacks,
                           const ngtcp2_settings *settings, void *user_data) {
  int rv;

  rv = conn_new(pconn, conn_id, version, callbacks, user_data);
  if (rv != 0) {
    return rv;
  }

  (*pconn)->state = NGTCP2_CS_SERVER_INITIAL;
  (*pconn)->server = 1;
  (*pconn)->local_settings = *settings;
  (*pconn)->max_remote_stream_id = settings->max_stream_id;
  (*pconn)->unsent_max_rx_offset_high = (*pconn)->max_rx_offset_high =
      settings->max_data;

  /* TODO Since transport parameters are not required for interop now,
     just supply sensible default here.  Remove this when transport
     parameter gets mandatory. */
  (*pconn)->remote_settings.max_stream_data = 64 * 1024;
  (*pconn)->remote_settings.max_data = 64;
  (*pconn)->remote_settings.max_stream_id = 0;

  (*pconn)->max_tx_offset_high = (*pconn)->remote_settings.max_data;

  return 0;
}

static void delete_acktr_entry(ngtcp2_acktr_entry *ent, ngtcp2_mem *mem) {
  ngtcp2_acktr_entry *next;

  for (; ent;) {
    next = ent->next;
    ngtcp2_acktr_entry_del(ent, mem);
    ent = next;
  }
}

static void delete_buffed_pkts(ngtcp2_pkt_chain *pc, ngtcp2_mem *mem) {
  ngtcp2_pkt_chain *next;

  for (; pc;) {
    next = pc->next;
    ngtcp2_pkt_chain_del(pc, mem);
    pc = next;
  }
}

static void delete_frq(ngtcp2_frame_chain *frc, ngtcp2_mem *mem) {
  ngtcp2_frame_chain *next;
  for (; frc;) {
    next = frc->next;
    ngtcp2_frame_chain_del(frc, mem);
    frc = next;
  }
}

static int delete_strms_each(ngtcp2_map_entry *ent, void *ptr) {
  ngtcp2_mem *mem = ptr;
  ngtcp2_strm *s = ngtcp2_struct_of(ent, ngtcp2_strm, me);

  ngtcp2_strm_free(s);
  ngtcp2_mem_free(mem, s);

  return 0;
}

void ngtcp2_conn_del(ngtcp2_conn *conn) {
  if (conn == NULL) {
    return;
  }

  delete_buffed_pkts(conn->buffed_rx_ppkts, conn->mem);

  delete_acktr_entry(conn->acktr.ent, conn->mem);
  ngtcp2_acktr_free(&conn->acktr);

  ngtcp2_crypto_km_del(conn->rx_ckm, conn->mem);
  ngtcp2_crypto_km_del(conn->tx_ckm, conn->mem);

  delete_frq(conn->frq, conn->mem);

  ngtcp2_rtb_free(&conn->rtb);

  ngtcp2_idtr_free(&conn->remote_idtr);
  ngtcp2_idtr_free(&conn->local_idtr);
  ngtcp2_map_each_free(&conn->strms, delete_strms_each, conn->mem);
  ngtcp2_map_free(&conn->strms);

  ngtcp2_mem_free(conn->mem, conn);
}

static void conn_set_next_ack_expiry(ngtcp2_conn *conn, ngtcp2_tstamp ts) {
  conn->next_ack_expiry = ts + NGTCP2_DELAYED_ACK_TIMEOUT;
}

static void conn_invalidate_next_ack_expiry(ngtcp2_conn *conn) {
  conn->next_ack_expiry = 0;
}

static int conn_create_ack_frame(ngtcp2_conn *conn, ngtcp2_ack *ack,
                                 ngtcp2_tstamp ts) {
  uint64_t first_pkt_num;
  ngtcp2_tstamp ack_delay;
  uint64_t last_pkt_num;
  ngtcp2_ack_blk *blk;
  int initial = 1;
  uint64_t gap;
  ngtcp2_acktr_entry *rpkt;

  rpkt = ngtcp2_acktr_get(&conn->acktr);
  if (rpkt == NULL) {
    /* TODO This might not be necessary if we don't forget ACK. */
    conn_invalidate_next_ack_expiry(conn);
    return 0;
  }

  first_pkt_num = last_pkt_num = rpkt->pkt_num;
  ack_delay = ts - rpkt->tstamp;

  ngtcp2_acktr_remove(&conn->acktr, rpkt);
  ngtcp2_acktr_entry_del(rpkt, conn->mem);

  ack->type = NGTCP2_FRAME_ACK;
  ack->num_ts = 0;
  ack->num_blks = 0;

  for (; (rpkt = ngtcp2_acktr_get(&conn->acktr));) {
    if (rpkt->pkt_num + 1 == last_pkt_num) {
      last_pkt_num = rpkt->pkt_num;
      ngtcp2_acktr_remove(&conn->acktr, rpkt);
      ngtcp2_acktr_entry_del(rpkt, conn->mem);
      continue;
    }

    if (initial) {
      initial = 0;
      ack->largest_ack = first_pkt_num;
      ack->ack_delay = (uint16_t)ack_delay;
      ack->first_ack_blklen = first_pkt_num - last_pkt_num;
    } else {
      blk = &ack->blks[ack->num_blks++];
      blk->gap = (uint8_t)gap;
      blk->blklen = first_pkt_num - last_pkt_num + 1;
    }

    gap = last_pkt_num - rpkt->pkt_num - 1;
    if (gap > 255) {
      /* TODO We need to encode next ack in the separate ACK frame or
         use the trick of 0 length ACK Block Length (not sure it is
         OK.  Anyway, this implementation will be rewritten soon, so
         we don't optimize this at the moment. */
      break;
    }

    first_pkt_num = last_pkt_num = rpkt->pkt_num;

    ngtcp2_acktr_remove(&conn->acktr, rpkt);
    ngtcp2_acktr_entry_del(rpkt, conn->mem);

    if (ack->num_blks == 255) {
      break;
    }
  }

  if (initial) {
    ack->largest_ack = first_pkt_num;
    ack->ack_delay = (uint16_t)ack_delay;
    ack->first_ack_blklen = first_pkt_num - last_pkt_num;
  } else if (first_pkt_num != last_pkt_num) {
    blk = &ack->blks[ack->num_blks++];
    blk->gap = (uint8_t)gap;
    blk->blklen = first_pkt_num - last_pkt_num + 1;
  }

  if (ngtcp2_acktr_get(&conn->acktr) == NULL) {
    conn_invalidate_next_ack_expiry(conn);
  }

  return 0;
}

/*
 * conn_retransmit_unprotected performs retransmission of lost
 * unprotected packet.
 */
static ssize_t conn_retransmit_unprotected(ngtcp2_conn *conn, uint8_t *dest,
                                           size_t destlen,
                                           ngtcp2_rtb_entry *ent,
                                           ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_upe upe;
  ngtcp2_pkt_hd hd = ent->hd;
  ngtcp2_frame_chain **pfrc, *frc;
  ngtcp2_rtb_entry *nent = NULL;
  ngtcp2_frame localfr;
  int pkt_empty = 1;
  int send_pkt_cb_called = 0;

  /* This is required because ent->hd may have old client version. */
  hd.version = conn->version;
  hd.conn_id = conn->conn_id;
  hd.pkt_num = conn->next_tx_pkt_num;

  ngtcp2_upe_init(&upe, dest, destlen);

  rv = ngtcp2_upe_encode_hd(&upe, &hd);
  if (rv != 0) {
    return rv;
  }

  /* TODO Don't include ACK in this unprotected packet in order not to
     ack protected packet here for now. */

  for (pfrc = &ent->frc; *pfrc;) {
    if ((*pfrc)->fr.type == NGTCP2_FRAME_MAX_STREAM_DATA) {
      if ((*pfrc)->fr.max_stream_data.max_stream_data <
          conn->strm0->unsent_max_rx_offset) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
    }
    rv = ngtcp2_upe_encode_frame(&upe, &(*pfrc)->fr);
    if (rv != 0) {
      if (rv == NGTCP2_ERR_NOBUF) {
        break;
      }
    }

    if (!send_pkt_cb_called) {
      rv = conn_call_send_pkt(conn, &hd);
      if (rv != 0) {
        return rv;
      }
      send_pkt_cb_called = 1;
    }

    rv = conn_call_send_frame(conn, &hd, &(*pfrc)->fr);
    if (rv != 0) {
      return rv;
    }

    pkt_empty = 0;
    pfrc = &(*pfrc)->next;
  }

  if (pkt_empty) {
    return rv;
  }

  if (*pfrc == NULL) {
    /* We have retransmit complete packet.  Update ent with new packet
       header, and push it into rbt again. */
    ent->hd = hd;
    /* TODO Should we change expiry time in 2nd try? */
    ent->expiry = ts + NGTCP2_INITIAL_EXPIRY;

    if (hd.type == NGTCP2_PKT_CLIENT_INITIAL) {
      localfr.type = NGTCP2_FRAME_PADDING;
      localfr.padding.len = ngtcp2_upe_padding(&upe);

      rv = conn_call_send_frame(conn, &hd, &localfr);
      if (rv != 0) {
        return rv;
      }
    }

    ++conn->next_tx_pkt_num;
    return (ssize_t)ngtcp2_upe_final(&upe, NULL);
  }

  /* We have partially retransmitted lost frames.  Create new
     ngtcp2_rtb_entry to track down the sent packet. */
  rv = ngtcp2_rtb_entry_new(&nent, &hd, NULL, ts + NGTCP2_INITIAL_EXPIRY,
                            conn->mem);
  if (rv != 0) {
    return rv;
  }

  nent->frc = ent->frc;
  ent->frc = *pfrc;
  *pfrc = NULL;

  rv = ngtcp2_rtb_add(&conn->rtb, nent);
  if (rv != 0) {
    ngtcp2_rtb_entry_del(nent, conn->mem);
    return rv;
  }

  ++conn->next_tx_pkt_num;

  return (ssize_t)ngtcp2_upe_final(&upe, NULL);
}

static ssize_t conn_retransmit_protected(ngtcp2_conn *conn, uint8_t *dest,
                                         size_t destlen, ngtcp2_rtb_entry *ent,
                                         ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd = ent->hd;
  ngtcp2_frame_chain **pfrc, *frc;
  ngtcp2_rtb_entry *nent = NULL;
  ngtcp2_frame localfr;
  int pkt_empty = 1;
  ssize_t nwrite;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_strm *strm;
  int send_pkt_cb_called = 0;
  int ack_expired = conn->next_ack_expiry && conn->next_ack_expiry <= ts;

  /* This is required because ent->hd may have old client version. */
  hd.version = conn->version;
  hd.conn_id = conn->conn_id;
  hd.pkt_num = conn->next_tx_pkt_num;

  ctx.ckm = conn->tx_ckm;
  ctx.aead_overhead = conn->aead_overhead;
  ctx.encrypt = conn->callbacks.encrypt;
  ctx.user_data = conn;

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx, conn->mem);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    return rv;
  }

  localfr.type = !NGTCP2_FRAME_ACK;
  if (ack_expired) {
    rv = conn_create_ack_frame(conn, &localfr.ack, ts);
    if (rv != 0) {
      return rv;
    }
    if (localfr.type == NGTCP2_FRAME_ACK) {
      rv = ngtcp2_ppe_encode_frame(&ppe, &localfr);
      if (rv != 0) {
        return rv;
      }

      if (!send_pkt_cb_called) {
        rv = conn_call_send_pkt(conn, &hd);
        if (rv != 0) {
          return rv;
        }
        send_pkt_cb_called = 1;
      }

      rv = conn_call_send_frame(conn, &hd, &localfr);
      if (rv != 0) {
        return rv;
      }

      pkt_empty = 0;
    }
  }

  for (pfrc = &ent->frc; *pfrc;) {
    switch ((*pfrc)->fr.type) {
    case NGTCP2_FRAME_MAX_STREAM_ID:
      if ((*pfrc)->fr.max_stream_id.max_stream_id <
          conn->max_remote_stream_id) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    case NGTCP2_FRAME_MAX_STREAM_DATA:
      strm =
          ngtcp2_conn_find_stream(conn, (*pfrc)->fr.max_stream_data.stream_id);
      if (strm == NULL ||
          (*pfrc)->fr.max_stream_data.max_stream_data < strm->max_rx_offset) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    case NGTCP2_FRAME_MAX_DATA:
      if ((*pfrc)->fr.max_data.max_data < conn->max_rx_offset_high) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    }
    rv = ngtcp2_ppe_encode_frame(&ppe, &(*pfrc)->fr);
    if (rv != 0) {
      if (rv == NGTCP2_ERR_NOBUF) {
        break;
      }
    }

    if (!send_pkt_cb_called) {
      rv = conn_call_send_pkt(conn, &hd);
      if (rv != 0) {
        return rv;
      }
      send_pkt_cb_called = 1;
    }

    rv = conn_call_send_frame(conn, &hd, &(*pfrc)->fr);
    if (rv != 0) {
      return rv;
    }

    pkt_empty = 0;
    pfrc = &(*pfrc)->next;
  }

  if (pkt_empty) {
    return rv;
  }

  if (*pfrc == NULL) {
    /* We have retransmit complete packet.  Update ent with new packet
       header, and push it into rbt again. */
    ent->hd = hd;
    /* TODO Should we change expiry time in 2nd try? */
    ent->expiry = ts + NGTCP2_INITIAL_EXPIRY;

    nwrite = ngtcp2_ppe_final(&ppe, NULL);
    if (nwrite < 0) {
      return nwrite;
    }

    ++conn->next_tx_pkt_num;

    return nwrite;
  }

  /* We have partially retransmitted lost frames.  Create new
     ngtcp2_rtb_entry to track down the sent packet. */
  rv = ngtcp2_rtb_entry_new(&nent, &hd, NULL, ts + NGTCP2_INITIAL_EXPIRY,
                            conn->mem);
  if (rv != 0) {
    return rv;
  }

  nent->frc = ent->frc;
  ent->frc = *pfrc;
  *pfrc = NULL;

  rv = ngtcp2_rtb_add(&conn->rtb, nent);
  if (rv != 0) {
    ngtcp2_rtb_entry_del(nent, conn->mem);
    return rv;
  }

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    return nwrite;
  }

  ++conn->next_tx_pkt_num;

  return nwrite;
}

static ssize_t conn_retransmit(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                               ngtcp2_tstamp ts) {
  ngtcp2_rtb_entry *ent;
  ssize_t nwrite;
  int rv;

  for (;;) {
    ent = ngtcp2_rtb_top(&conn->rtb);
    if (ent == NULL || ent->expiry > ts) {
      return 0;
    }
    ngtcp2_rtb_pop(&conn->rtb);

    if (ent->hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) {
      switch (ent->hd.type) {
      case NGTCP2_PKT_CLIENT_INITIAL:
      case NGTCP2_PKT_SERVER_CLEARTEXT:
      case NGTCP2_PKT_CLIENT_CLEARTEXT:
        nwrite = conn_retransmit_unprotected(conn, dest, destlen, ent, ts);
        break;
      default:
        /* TODO fix this */
        ngtcp2_rtb_entry_del(ent, conn->mem);
        return NGTCP2_ERR_INVALID_ARGUMENT;
      }
    } else {
      switch (ent->hd.type) {
      case NGTCP2_PKT_01:
      case NGTCP2_PKT_02:
      case NGTCP2_PKT_03:
        nwrite = conn_retransmit_protected(conn, dest, destlen, ent, ts);
        break;
      default:
        /* TODO fix this */
        ngtcp2_rtb_entry_del(ent, conn->mem);
        return NGTCP2_ERR_INVALID_ARGUMENT;
      }
    }

    if (nwrite <= 0) {
      if (nwrite == 0) {
        ngtcp2_rtb_entry_del(ent, conn->mem);
        continue;
      }
      if (nwrite == NGTCP2_ERR_NOBUF) {
        rv = ngtcp2_rtb_add(&conn->rtb, ent);
        if (rv != 0) {
          ngtcp2_rtb_entry_del(ent, conn->mem);
          assert(ngtcp2_err_fatal(rv));
          return rv;
        }
        return nwrite;
      }

      ngtcp2_rtb_entry_del(ent, conn->mem);
      return nwrite;
    }

    rv = ngtcp2_rtb_add(&conn->rtb, ent);
    if (rv != 0) {
      ngtcp2_rtb_entry_del(ent, conn->mem);
      assert(ngtcp2_err_fatal(rv));
      return rv;
    }

    return nwrite;
  }
}

static ssize_t conn_encode_handshake_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                         size_t destlen, uint8_t type,
                                         ngtcp2_buf *tx_buf, ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_upe upe;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame_chain *frc = NULL, **pfrc, *frc_head = NULL, *frc_next;
  ngtcp2_frame *fr, localfr;
  size_t nwrite;
  ngtcp2_rtb_entry *rtbent;
  int pkt_empty = 1;

  pfrc = &frc_head;

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_LONG_FORM, type, conn->conn_id,
                     conn->next_tx_pkt_num, conn->version);

  ngtcp2_upe_init(&upe, dest, destlen);

  rv = ngtcp2_upe_encode_hd(&upe, &hd);
  if (rv != 0) {
    return rv;
  }

  rv = conn_call_send_pkt(conn, &hd);
  if (rv != 0) {
    return rv;
  }

  /* Encode ACK here */
  if (type != NGTCP2_PKT_CLIENT_INITIAL) {
    localfr.type = !NGTCP2_FRAME_ACK;
    /* TODO Should we retransmit ACK frame? */
    rv = conn_create_ack_frame(conn, &localfr.ack, ts);
    if (rv != 0) {
      return rv;
    }
    if (localfr.type == NGTCP2_FRAME_ACK) {
      rv = ngtcp2_upe_encode_frame(&upe, &localfr);
      if (rv != 0) {
        return rv;
      }

      rv = conn_call_send_frame(conn, &hd, &localfr);
      if (rv != 0) {
        return rv;
      }

      pkt_empty = 0;
    }

    if (conn->strm0->max_rx_offset < conn->strm0->unsent_max_rx_offset) {
      rv = ngtcp2_frame_chain_new(&frc, conn->mem);
      if (rv != 0) {
        return rv;
      }

      *pfrc = frc;
      pfrc = &frc->next;

      frc->fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
      frc->fr.max_stream_data.stream_id = 0;
      frc->fr.max_stream_data.max_stream_data =
          conn->strm0->unsent_max_rx_offset;

      /* TODO If we get NGTCP2_ERR_NOBUF below, we lose
         MAX_STREAM_DATA update. */
      conn->strm0->max_rx_offset = conn->strm0->unsent_max_rx_offset;

      rv = ngtcp2_upe_encode_frame(&upe, &frc->fr);
      if (rv != 0) {
        goto fail;
      }

      rv = conn_call_send_frame(conn, &hd, &frc->fr);
      if (rv != 0) {
        goto fail;
      }

      pkt_empty = 0;
    }
  }

  if (ngtcp2_upe_left(&upe) < NGTCP2_STREAM_OVERHEAD + 1) {
    if (!pkt_empty) {
      ++conn->next_tx_pkt_num;
      return (ssize_t)ngtcp2_upe_final(&upe, NULL);
    }

    rv = NGTCP2_ERR_NOBUF;
    goto fail;
  }

  nwrite = ngtcp2_min(ngtcp2_buf_len(tx_buf),
                      ngtcp2_upe_left(&upe) - NGTCP2_STREAM_OVERHEAD);
  nwrite =
      ngtcp2_min(nwrite, conn->strm0->max_tx_offset - conn->strm0->tx_offset);

  if (nwrite > 0) {
    rv = ngtcp2_frame_chain_new(&frc, conn->mem);
    if (rv != 0) {
      goto fail;
    }

    *pfrc = frc;
    pfrc = &frc->next;

    fr = &frc->fr;

    /* TODO Make a function to create STREAM frame */
    fr->type = NGTCP2_FRAME_STREAM;
    fr->stream.flags = 0;
    fr->stream.fin = 0;
    fr->stream.stream_id = 0;
    fr->stream.offset = conn->strm0->tx_offset;
    fr->stream.datalen = nwrite;
    fr->stream.data = tx_buf->pos;

    rv = ngtcp2_upe_encode_frame(&upe, fr);
    if (rv != 0) {
      goto fail;
    }

    rv = conn_call_send_frame(conn, &hd, fr);
    if (rv != 0) {
      goto fail;
    }

    tx_buf->pos += nwrite;
    conn->strm0->tx_offset += nwrite;
  }

  if (type == NGTCP2_PKT_CLIENT_INITIAL) {
    localfr.type = NGTCP2_FRAME_PADDING;
    localfr.padding.len = ngtcp2_upe_padding(&upe);
    if (localfr.padding.len > 0) {
      rv = conn_call_send_frame(conn, &hd, &localfr);
      if (rv != 0) {
        goto fail;
      }
    }
  }

  ++conn->next_tx_pkt_num;

  if (frc_head) {
    rv = ngtcp2_rtb_entry_new(&rtbent, &hd, frc_head,
                              ts + NGTCP2_INITIAL_EXPIRY, conn->mem);
    if (rv != 0) {
      goto fail;
    }

    rv = ngtcp2_rtb_add(&conn->rtb, rtbent);
    if (rv != 0) {
      ngtcp2_rtb_entry_del(rtbent, conn->mem);
      return rv;
    }
  }

  return (ssize_t)ngtcp2_upe_final(&upe, NULL);

fail:
  for (frc = frc_head; frc;) {
    frc_next = frc->next;
    ngtcp2_frame_chain_del(frc, conn->mem);
    frc = frc_next;
  }
  return rv;
}

/*
 * conn_encode_unprotected_ack_if_any creates packet which only
 * includes ACK frame if any ack is required.
 */
static ssize_t conn_encode_unprotected_ack_if_any(ngtcp2_conn *conn,
                                                  uint8_t *dest, size_t destlen,
                                                  uint8_t type,
                                                  ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_upe upe;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame fr;

  if (ngtcp2_acktr_get(&conn->acktr) == NULL) {
    return 0;
  }

  rv = conn_create_ack_frame(conn, &fr.ack, ts);
  if (rv != 0) {
    return rv;
  }

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_LONG_FORM, type, conn->conn_id,
                     conn->next_tx_pkt_num, conn->version);

  ngtcp2_upe_init(&upe, dest, destlen);

  rv = ngtcp2_upe_encode_hd(&upe, &hd);
  if (rv != 0) {
    return rv;
  }

  rv = conn_call_send_pkt(conn, &hd);
  if (rv != 0) {
    return rv;
  }

  rv = ngtcp2_upe_encode_frame(&upe, &fr);
  if (rv != 0) {
    return rv;
  }

  rv = conn_call_send_frame(conn, &hd, &fr);
  if (rv != 0) {
    return rv;
  }

  ++conn->next_tx_pkt_num;

  return (ssize_t)ngtcp2_upe_final(&upe, NULL);
}

static ssize_t conn_send_client_initial(ngtcp2_conn *conn, uint8_t *dest,
                                        size_t destlen, ngtcp2_tstamp ts) {
  uint64_t pkt_num = 0;
  const uint8_t *payload;
  ssize_t payloadlen;
  ngtcp2_buf *tx_buf = &conn->strm0->tx_buf;

  payloadlen = conn->callbacks.send_client_initial(
      conn, NGTCP2_CONN_FLAG_NONE, &pkt_num, &payload, conn->user_data);

  if (payloadlen <= 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  ngtcp2_buf_init(tx_buf, (uint8_t *)payload, (size_t)payloadlen);
  tx_buf->last += payloadlen;

  conn->next_tx_pkt_num = pkt_num;

  return conn_encode_handshake_pkt(conn, dest, destlen,
                                   NGTCP2_PKT_CLIENT_INITIAL, tx_buf, ts);
}

static ssize_t conn_send_client_cleartext(ngtcp2_conn *conn, uint8_t *dest,
                                          size_t destlen, ngtcp2_tstamp ts) {
  const uint8_t *payload;
  ssize_t payloadlen;
  ngtcp2_buf *tx_buf = &conn->strm0->tx_buf;

  if (ngtcp2_buf_len(tx_buf) == 0) {
    payloadlen = conn->callbacks.send_client_cleartext(
        conn, NGTCP2_CONN_FLAG_NONE, &payload, conn->user_data);

    if (payloadlen < 0) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    if (payloadlen == 0) {
      return conn_encode_unprotected_ack_if_any(
          conn, dest, destlen, NGTCP2_PKT_CLIENT_CLEARTEXT, ts);
    }

    ngtcp2_buf_init(tx_buf, (uint8_t *)payload, (size_t)payloadlen);
    tx_buf->last += payloadlen;
  }

  return conn_encode_handshake_pkt(conn, dest, destlen,
                                   NGTCP2_PKT_CLIENT_CLEARTEXT, tx_buf, ts);
}

static ssize_t conn_send_server_cleartext(ngtcp2_conn *conn, uint8_t *dest,
                                          size_t destlen, int initial,
                                          ngtcp2_tstamp ts) {
  uint64_t pkt_num = 0;
  const uint8_t *payload;
  ssize_t payloadlen;
  ngtcp2_buf *tx_buf = &conn->strm0->tx_buf;

  if (ngtcp2_buf_len(tx_buf) == 0) {
    payloadlen = conn->callbacks.send_server_cleartext(
        conn, NGTCP2_CONN_FLAG_NONE, initial ? &pkt_num : NULL, &payload,
        conn->user_data);

    if (payloadlen < 0) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    if (payloadlen == 0) {
      if (initial) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
      }
      return conn_encode_unprotected_ack_if_any(
          conn, dest, destlen, NGTCP2_PKT_SERVER_CLEARTEXT, ts);
    }

    ngtcp2_buf_init(tx_buf, (uint8_t *)payload, (size_t)payloadlen);
    tx_buf->last += payloadlen;
  }

  if (initial) {
    conn->next_tx_pkt_num = pkt_num;
  }

  return conn_encode_handshake_pkt(conn, dest, destlen,
                                   NGTCP2_PKT_SERVER_CLEARTEXT, tx_buf, ts);
}

static int conn_should_send_max_stream_data(ngtcp2_conn *conn,
                                            ngtcp2_strm *strm) {
  return conn->local_settings.max_stream_data / 2 <
         (strm->unsent_max_rx_offset - strm->max_rx_offset);
}

static int conn_should_send_max_data(ngtcp2_conn *conn) {
  return conn->local_settings.max_data / 2 >=
         conn->max_rx_offset_high - conn->rx_offset_high;
}

static ssize_t conn_send_pkt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                             ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame ackfr;
  ssize_t nwrite;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_frame_chain **pfrc, *nfrc;
  ngtcp2_rtb_entry *ent;
  size_t left;
  ngtcp2_strm *strm, *strm_next;
  int send_pkt_cb_called = 0;
  int pkt_empty = 1;
  int ack_expired = conn->next_ack_expiry && conn->next_ack_expiry <= ts;

  ackfr.type = !NGTCP2_FRAME_ACK;
  if (ack_expired) {
    rv = conn_create_ack_frame(conn, &ackfr.ack, ts);
    if (rv != 0) {
      return rv;
    }

    if (ackfr.type == NGTCP2_FRAME_ACK) {
      pkt_empty = 0;
    }
  }

  if (conn->max_remote_stream_id > conn->local_settings.max_stream_id) {
    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      return rv;
    }
    nfrc->fr.type = NGTCP2_FRAME_MAX_STREAM_ID;
    nfrc->fr.max_stream_id.max_stream_id = conn->max_remote_stream_id;
    nfrc->next = conn->frq;
    conn->frq = nfrc;

    conn->local_settings.max_stream_id = conn->max_remote_stream_id;
    /* MAX_STREAM_ID frame could appear in the following for loop if
       buffer is too small. */

    pkt_empty = 0;
  }

  if ((!pkt_empty || conn->frq || conn_should_send_max_data(conn)) &&
      conn->unsent_max_rx_offset_high > conn->max_rx_offset_high) {
    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      return rv;
    }
    nfrc->fr.type = NGTCP2_FRAME_MAX_DATA;
    nfrc->fr.max_data.max_data = conn->unsent_max_rx_offset_high;
    nfrc->next = conn->frq;
    conn->frq = nfrc;

    conn->max_rx_offset_high = conn->unsent_max_rx_offset_high;

    pkt_empty = 0;
  }

  while (conn->fc_strms) {
    strm = conn->fc_strms;
    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      return rv;
    }
    nfrc->fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
    nfrc->fr.max_stream_data.stream_id = strm->stream_id;
    nfrc->fr.max_stream_data.max_stream_data = strm->unsent_max_rx_offset;
    nfrc->next = conn->frq;
    conn->frq = nfrc;

    strm->max_rx_offset = strm->unsent_max_rx_offset;

    strm_next = strm->fc_next;
    conn->fc_strms = strm_next;
    if (strm_next) {
      strm_next->fc_pprev = &conn->fc_strms;
    }
    strm->fc_next = NULL;
    strm->fc_pprev = NULL;
    strm = strm_next;

    pkt_empty = 0;
  }

  if (pkt_empty && conn->frq == NULL) {
    return 0;
  }

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_CONN_ID, NGTCP2_PKT_03, conn->conn_id,
                     conn->next_tx_pkt_num, conn->version);

  ctx.ckm = conn->tx_ckm;
  ctx.aead_overhead = conn->aead_overhead;
  ctx.encrypt = conn->callbacks.encrypt;
  ctx.user_data = conn;

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx, conn->mem);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    return rv;
  }

  if (ackfr.type == NGTCP2_FRAME_ACK) {
    rv = ngtcp2_ppe_encode_frame(&ppe, &ackfr);
    if (rv != 0) {
      return rv;
    }

    if (!send_pkt_cb_called) {
      rv = conn_call_send_pkt(conn, &hd);
      if (rv != 0) {
        return rv;
      }
      send_pkt_cb_called = 1;
    }

    rv = conn_call_send_frame(conn, &hd, &ackfr);
    if (rv != 0) {
      return rv;
    }
  }

  for (pfrc = &conn->frq; *pfrc; pfrc = &(*pfrc)->next) {
    if ((*pfrc)->fr.type == NGTCP2_FRAME_STREAM) {
      left = ngtcp2_ppe_left(&ppe);
      if (left <= NGTCP2_STREAM_OVERHEAD) {
        break;
      }
      left -= NGTCP2_STREAM_OVERHEAD;
      if (left < (*pfrc)->fr.stream.datalen) {
        if (left < 1024) {
          break;
        }
        rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
        if (rv != 0) {
          return rv;
        }
        nfrc->fr.type = NGTCP2_FRAME_STREAM;
        nfrc->fr.stream = (*pfrc)->fr.stream;
        nfrc->fr.stream.datalen = left;
        (*pfrc)->fr.stream.datalen -= left;
        (*pfrc)->fr.stream.data += left;
        (*pfrc)->fr.stream.offset += left;
        nfrc->next = *pfrc;
        *pfrc = nfrc;
      }
    }
    rv = ngtcp2_ppe_encode_frame(&ppe, &(*pfrc)->fr);
    if (rv != 0) {
      if (rv == NGTCP2_ERR_NOBUF) {
        break;
      }
      return rv;
    }

    if (!send_pkt_cb_called) {
      rv = conn_call_send_pkt(conn, &hd);
      if (rv != 0) {
        return rv;
      }
      send_pkt_cb_called = 1;
    }

    rv = conn_call_send_frame(conn, &hd, &(*pfrc)->fr);
    if (rv != 0) {
      return rv;
    }

    pkt_empty = 0;
  }

  if (pkt_empty) {
    return rv;
  }

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    return nwrite;
  }

  if (*pfrc != conn->frq) {
    rv = ngtcp2_rtb_entry_new(&ent, &hd, NULL, ts + NGTCP2_INITIAL_EXPIRY,
                              conn->mem);
    if (rv != 0) {
      return rv;
    }

    ent->frc = conn->frq;
    conn->frq = *pfrc;
    *pfrc = NULL;

    rv = ngtcp2_rtb_add(&conn->rtb, ent);
    if (rv != 0) {
      ngtcp2_rtb_entry_del(ent, conn->mem);
      return rv;
    }
  }

  ++conn->next_tx_pkt_num;

  return nwrite;
}

ssize_t ngtcp2_conn_send(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                         ngtcp2_tstamp ts) {
  ssize_t nwrite = 0;

  nwrite = conn_retransmit(conn, dest, destlen, ts);
  if (nwrite != 0) {
    return nwrite;
  }

  switch (conn->state) {
  case NGTCP2_CS_CLIENT_INITIAL:
    nwrite = conn_send_client_initial(conn, dest, destlen, ts);
    if (nwrite < 0) {
      break;
    }
    conn->state = NGTCP2_CS_CLIENT_WAIT_HANDSHAKE;
    break;
  case NGTCP2_CS_CLIENT_WAIT_HANDSHAKE:
    nwrite = conn_send_client_cleartext(conn, dest, destlen, ts);
    if (nwrite < 0) {
      break;
    }
    break;
  case NGTCP2_CS_CLIENT_HANDSHAKE_ALMOST_FINISHED:
    nwrite = conn_send_client_cleartext(conn, dest, destlen, ts);
    if (nwrite < 0) {
      break;
    }
    if (nwrite == 0) {
      conn->state = NGTCP2_CS_POST_HANDSHAKE;
    }
    break;
  case NGTCP2_CS_SERVER_INITIAL:
    nwrite = conn_send_server_cleartext(conn, dest, destlen, 1, ts);
    if (nwrite < 0) {
      break;
    }
    conn->state = NGTCP2_CS_SERVER_WAIT_HANDSHAKE;
    break;
  case NGTCP2_CS_SERVER_WAIT_HANDSHAKE:
    nwrite = conn_send_server_cleartext(conn, dest, destlen, 0, ts);
    if (nwrite < 0) {
      break;
    }
    break;
  case NGTCP2_CS_POST_HANDSHAKE:
  case NGTCP2_CS_CLOSE_WAIT:
    nwrite = conn_send_pkt(conn, dest, destlen, ts);
    if (nwrite < 0) {
      break;
    }
  }

  return nwrite;
}

static int conn_on_version_negotiation(ngtcp2_conn *conn,
                                       const ngtcp2_pkt_hd *hd,
                                       const uint8_t *pkt, size_t pktlen) {
  uint32_t sv[16];
  uint32_t *p;
  int rv;
  size_t nsv;

  if (pktlen % sizeof(uint32_t)) {
    return NGTCP2_ERR_PROTO;
  }

  if (!conn->callbacks.recv_version_negotiation) {
    return 0;
  }

  if (pktlen > sizeof(sv)) {
    p = ngtcp2_mem_malloc(conn->mem, pktlen);
    if (p == NULL) {
      return NGTCP2_ERR_NOMEM;
    }
  } else {
    p = sv;
  }

  nsv = ngtcp2_pkt_decode_version_negotiation(p, pkt, pktlen);

  rv = conn->callbacks.recv_version_negotiation(conn, hd, sv, nsv,
                                                conn->user_data);

  if (p != sv) {
    ngtcp2_mem_free(conn->mem, p);
  }

  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_recv_ack(ngtcp2_conn *conn, ngtcp2_ack *fr) {
  int rv;
  rv = ngtcp2_pkt_validate_ack(fr);
  if (rv != 0) {
    return rv;
  }
  return ngtcp2_rtb_recv_ack(&conn->rtb, fr, conn);
}

static int conn_recv_max_stream_data(ngtcp2_conn *conn,
                                     const ngtcp2_max_stream_data *fr) {
  ngtcp2_strm *strm;

  strm = ngtcp2_conn_find_stream(conn, fr->stream_id);
  if (strm == NULL) {
    return 0;
  }

  strm->max_tx_offset = ngtcp2_max(strm->max_tx_offset, fr->max_stream_data);

  return 0;
}

static void conn_recv_max_data(ngtcp2_conn *conn, const ngtcp2_max_data *fr) {
  conn->max_tx_offset_high = ngtcp2_max(conn->max_tx_offset_high, fr->max_data);
}

static int conn_buffer_protected_pkt(ngtcp2_conn *conn, const uint8_t *pkt,
                                     size_t pktlen, ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_pkt_chain **ppc = &conn->buffed_rx_ppkts;
  ngtcp2_pkt_chain *pc;
  size_t i;
  for (i = 0; *ppc && i < NGTCP2_MAX_NUM_BUFFED_RX_PPKTS;
       ppc = &(*ppc)->next, ++i)
    ;

  if (i == NGTCP2_MAX_NUM_BUFFED_RX_PPKTS) {
    return 0;
  }

  rv = ngtcp2_pkt_chain_new(&pc, pkt, pktlen, ts, conn->mem);
  if (rv != 0) {
    return rv;
  }

  *ppc = pc;

  return 0;
}

static int conn_recv_handshake_pkt(ngtcp2_conn *conn, const uint8_t *pkt,
                                   size_t pktlen, ngtcp2_tstamp ts) {
  ssize_t nread;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame fr;
  int rv;
  int require_ack = 0;
  uint64_t rx_offset;
  uint64_t fr_end_offset;

  if (!(pkt[0] & NGTCP2_HEADER_FORM_BIT)) {
    return conn_buffer_protected_pkt(conn, pkt, pktlen, ts);
  }

  nread = ngtcp2_pkt_decode_hd_long(&hd, pkt, pktlen);
  if (nread < 0) {
    return (int)nread;
  }

  pkt += nread;
  pktlen -= (size_t)nread;

  hd.pkt_num = ngtcp2_pkt_adjust_pkt_num(conn->max_rx_pkt_num, hd.pkt_num, 32);

  rv = conn_call_recv_pkt(conn, &hd);
  if (rv != 0) {
    return rv;
  }

  if (conn->version != hd.version) {
    return NGTCP2_ERR_PROTO;
  }

  /* TODO What happen if connection ID changes in mid handshake? */
  if (conn->server) {
    switch (hd.type) {
    case NGTCP2_PKT_CLIENT_INITIAL:
    case NGTCP2_PKT_CLIENT_CLEARTEXT:
      break;
    default:
      return NGTCP2_ERR_PROTO;
    }
  } else {
    conn->conn_id = hd.conn_id;

    switch (hd.type) {
    case NGTCP2_PKT_SERVER_CLEARTEXT:
      break;
    case NGTCP2_PKT_VERSION_NEGOTIATION:
      rv = conn_on_version_negotiation(conn, &hd, pkt, pktlen);
      if (rv != 0) {
        return rv;
      }
      return 0;
    default:
      return NGTCP2_ERR_PROTO;
    }
  }

  for (; pktlen;) {
    nread = ngtcp2_pkt_decode_frame(&fr, pkt, pktlen, conn->max_rx_pkt_num);
    if (nread < 0) {
      return (int)nread;
    }

    pkt += nread;
    pktlen -= (size_t)nread;

    rv = conn_call_recv_frame(conn, &hd, &fr);
    if (rv != 0) {
      return rv;
    }

    /* We don't ack packet which contains ACK and CONNECTION_CLOSE
       only. */
    /* TODO What about packet with PADDING frames only? */
    require_ack |=
        fr.type != NGTCP2_FRAME_ACK && fr.type != NGTCP2_FRAME_CONNECTION_CLOSE;

    switch (fr.type) {
    case NGTCP2_FRAME_ACK:
      rv = conn_recv_ack(conn, &fr.ack);
      if (rv != 0) {
        return rv;
      }
      continue;
    case NGTCP2_FRAME_MAX_STREAM_DATA:
      rv = conn_recv_max_stream_data(conn, &fr.max_stream_data);
      if (rv != 0) {
        return rv;
      }
      continue;
    }

    if (fr.type != NGTCP2_FRAME_STREAM || fr.stream.stream_id != 0 ||
        fr.stream.datalen == 0) {
      continue;
    }

    if (hd.type == NGTCP2_PKT_CLIENT_INITIAL && fr.stream.offset != 0) {
      return NGTCP2_ERR_PROTO;
    }

    rx_offset = ngtcp2_strm_rx_offset(conn->strm0);
    if (rx_offset >= fr.stream.offset + fr.stream.datalen) {
      continue;
    }

    fr_end_offset = fr.stream.offset + fr.stream.datalen;
    if (conn->strm0->max_rx_offset < fr_end_offset) {
      return NGTCP2_ERR_FLOW_CONTROL;
    }

    if (fr.stream.offset <= rx_offset) {
      size_t ncut = (rx_offset - fr.stream.offset);
      const uint8_t *data = fr.stream.data + ncut;
      size_t datalen = fr.stream.datalen - ncut;

      rx_offset += datalen;
      ngtcp2_rob_remove_prefix(&conn->strm0->rob, rx_offset);

      rv = conn->callbacks.recv_handshake_data(conn, data, datalen,
                                               conn->user_data);
      if (rv != 0) {
        return rv;
      }

      conn->strm0->unsent_max_rx_offset += datalen;

      rv =
          ngtcp2_conn_emit_pending_recv_handshake(conn, conn->strm0, rx_offset);
      if (rv != 0) {
        return rv;
      }
    } else {
      rv = ngtcp2_strm_recv_reordering(conn->strm0, &fr.stream);
      if (rv != 0) {
        return rv;
      }
    }
  }

  conn->max_rx_pkt_num = ngtcp2_max(conn->max_rx_pkt_num, hd.pkt_num);

  if (require_ack) {
    rv = ngtcp2_conn_sched_ack(conn, hd.pkt_num, ts);
    if (rv != 0) {
      return rv;
    }
  }

  return 0;
}

static ssize_t conn_decrypt_packet(ngtcp2_conn *conn, uint8_t *dest,
                                   size_t destlen, const uint8_t *pkt,
                                   size_t pktlen, const uint8_t *ad,
                                   size_t adlen, uint64_t pkt_num) {
  uint8_t nonce[64];
  ngtcp2_crypto_km *ckm = conn->rx_ckm;
  ssize_t nwrite;

  assert(sizeof(nonce) >= ckm->ivlen);

  ngtcp2_crypto_create_nonce(nonce, ckm->iv, ckm->ivlen, pkt_num);

  nwrite = conn->callbacks.decrypt(conn, dest, destlen, pkt, pktlen, ckm->key,
                                   ckm->keylen, nonce, ckm->ivlen, ad, adlen,
                                   conn->user_data);

  if (nwrite < 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return nwrite;
}

int ngtcp2_conn_init_stream(ngtcp2_conn *conn, ngtcp2_strm *strm,
                            uint32_t stream_id, void *stream_user_data) {
  int rv;

  rv = ngtcp2_strm_init(strm, stream_id, NGTCP2_STRM_FLAG_NONE,
                        conn->local_settings.max_stream_data,
                        conn->remote_settings.max_stream_data, stream_user_data,
                        conn->mem);
  if (rv != 0) {
    ngtcp2_mem_free(conn->mem, strm);
    return rv;
  }

  rv = ngtcp2_map_insert(&conn->strms, &strm->me);
  if (rv != 0) {
    assert(rv != NGTCP2_ERR_INVALID_ARGUMENT);

    ngtcp2_strm_free(strm);
    ngtcp2_mem_free(conn->mem, strm);
    return rv;
  }

  return 0;
}

static int conn_emit_pending_stream_data(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                         uint64_t rx_offset) {
  size_t datalen;
  const uint8_t *data;
  int rv;

  for (;;) {
    datalen = ngtcp2_rob_data_at(&strm->rob, &data, rx_offset);
    if (datalen == 0) {
      assert(rx_offset == ngtcp2_strm_rx_offset(strm));
      return 0;
    }

    rx_offset += datalen;

    rv = conn_call_recv_stream_data(conn, strm,
                                    (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) &&
                                        rx_offset == strm->last_rx_offset,
                                    data, datalen);
    if (rv != 0) {
      return rv;
    }

    ngtcp2_rob_pop(&strm->rob, rx_offset - datalen, datalen);
  }
}

/*
 * conn_max_data_violated returns nonzero if receiving |datalen|
 * violates connection flow control on local endpoint.
 */
static int conn_max_data_violated(ngtcp2_conn *conn, size_t datalen) {
  uint64_t left_high = conn->max_rx_offset_high - conn->rx_offset_high;
  uint64_t low = conn->rx_offset_low + datalen;
  uint64_t from_low = low / 1024;

  if (left_high == from_low) {
    return (low & 0x3ff) > 0;
  }

  return left_high < from_low;
}

void ngtcp2_increment_offset(uint64_t *offset_high, uint32_t *offset_low,
                             size_t datalen) {
  uint64_t datalen_high = datalen / 1024;
  uint32_t datalen_low = datalen & 0x3ff;

  if (*offset_high > UINT64_MAX - datalen_high) {
    *offset_high = UINT64_MAX;
    *offset_low = 0x3ff;
    return;
  }

  *offset_high += datalen_high;
  *offset_low += datalen_low;

  if (*offset_low <= 0x3ff) {
    return;
  }

  if (*offset_high == UINT64_MAX) {
    *offset_low = 0x3ff;
    return;
  }

  *offset_low &= 0x3ff;
  ++*offset_high;
}

static int conn_recv_stream(ngtcp2_conn *conn, const ngtcp2_stream *fr) {
  int rv;
  ngtcp2_strm *strm;
  uint64_t rx_offset, fr_end_offset;
  int local_stream;

  /* TODO What to do if we get data for stream 0? */
  if (fr->stream_id == 0) {
    return 0;
  }

  local_stream = conn_local_stream(conn, fr->stream_id);

  if (!local_stream && conn->local_settings.max_stream_id < fr->stream_id) {
    return NGTCP2_ERR_PROTO;
  }

  if (UINT64_MAX - fr->datalen < fr->offset) {
    return NGTCP2_ERR_PROTO;
  }

  strm = ngtcp2_conn_find_stream(conn, fr->stream_id);
  if (strm == NULL) {
    if (local_stream) {
      rv = ngtcp2_idtr_is_open(&conn->local_idtr,
                               id_from_stream_id(fr->stream_id));
    } else {
      rv = ngtcp2_idtr_open(&conn->remote_idtr,
                            id_from_stream_id(fr->stream_id));
    }
    if (rv != 0) {
      if (rv == NGTCP2_ERR_STREAM_IN_USE) {
        /* TODO The stream has been closed.  This should be responded
           with RST_STREAM, or simply ignored. */
        return 0;
      }
      return rv;
    }

    strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
    if (strm == NULL) {
      return NGTCP2_ERR_NOMEM;
    }
    /* TODO Perhaps, call new_stream callback? */
    rv = ngtcp2_conn_init_stream(conn, strm, fr->stream_id, NULL);
    if (rv != 0) {
      return rv;
    }
  }

  fr_end_offset = fr->offset + fr->datalen;

  if (strm->max_rx_offset < fr_end_offset) {
    return NGTCP2_ERR_FLOW_CONTROL;
  }

  if (strm->last_rx_offset < fr_end_offset) {
    size_t datalen = fr_end_offset - strm->last_rx_offset;

    if (conn_max_data_violated(conn, datalen)) {
      return NGTCP2_ERR_FLOW_CONTROL;
    }

    ngtcp2_increment_offset(&conn->rx_offset_high, &conn->rx_offset_low,
                            datalen);
  }

  strm->last_rx_offset = ngtcp2_max(strm->last_rx_offset, fr_end_offset);

  if (fr->fin) {
    ngtcp2_strm_shutdown(strm, NGTCP2_STRM_FLAG_SHUT_RD);

    if (strm->last_rx_offset != fr_end_offset) {
      return NGTCP2_ERR_PROTO;
    }

    rx_offset = ngtcp2_strm_rx_offset(strm);
    if (fr_end_offset == rx_offset) {
      rv = conn_call_recv_stream_data(conn, strm, 1, NULL, 0);
      if (rv != 0) {
        return rv;
      }
      return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm);
    }
  } else {
    rx_offset = ngtcp2_strm_rx_offset(strm);
    if (fr_end_offset <= rx_offset) {
      return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm);
    }
  }

  if (fr->offset <= rx_offset) {
    size_t ncut = rx_offset - fr->offset;
    const uint8_t *data = fr->data + ncut;
    size_t datalen = fr->datalen - ncut;

    rx_offset += datalen;
    ngtcp2_rob_remove_prefix(&strm->rob, rx_offset);

    rv = conn_call_recv_stream_data(conn, strm, fr->fin, data, datalen);
    if (rv != 0) {
      return rv;
    }

    rv = conn_emit_pending_stream_data(conn, strm, rx_offset);
    if (rv != 0) {
      return rv;
    }
  } else {
    rv = ngtcp2_strm_recv_reordering(strm, fr);
    if (rv != 0) {
      return rv;
    }
  }
  return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm);
}

static int conn_recv_pkt(ngtcp2_conn *conn, uint8_t *pkt, size_t pktlen,
                         ngtcp2_tstamp ts) {
  ngtcp2_pkt_hd hd;
  size_t pkt_num_bits;
  int encrypted = 0;
  int rv = 0;
  const uint8_t *hdpkt = pkt;
  ssize_t nread, nwrite;
  ngtcp2_frame fr;
  int require_ack = 0;

  if (pkt[0] & NGTCP2_HEADER_FORM_BIT) {
    nread = ngtcp2_pkt_decode_hd_long(&hd, pkt, pktlen);
  } else {
    nread = ngtcp2_pkt_decode_hd_short(&hd, pkt, pktlen);
  }
  if (nread < 0) {
    return (int)nread;
  }

  pkt += nread;
  pktlen -= (size_t)nread;

  if (hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) {
    pkt_num_bits = 32;
    switch (hd.type) {
    case NGTCP2_PKT_1RTT_PROTECTED_K0:
      encrypted = 1;
      break;
    case NGTCP2_PKT_SERVER_CLEARTEXT:
      if (!conn->server) {
        /* TODO Client may keep sending old connection ID */
        conn->conn_id = hd.conn_id;
      }
      break;
    }
  } else {
    switch (hd.type) {
    case NGTCP2_PKT_01:
      pkt_num_bits = 8;
      break;
    case NGTCP2_PKT_02:
      pkt_num_bits = 16;
      break;
    case NGTCP2_PKT_03:
      pkt_num_bits = 32;
      break;
    default:
      assert(0);
    }
    if (!(hd.flags & NGTCP2_PKT_FLAG_KEY_PHASE)) {
      encrypted = 1;
    }
  }

  hd.pkt_num =
      ngtcp2_pkt_adjust_pkt_num(conn->max_rx_pkt_num, hd.pkt_num, pkt_num_bits);

  rv = conn_call_recv_pkt(conn, &hd);
  if (rv != 0) {
    return rv;
  }

  if (encrypted) {
    nwrite = conn_decrypt_packet(conn, pkt, pktlen, pkt, pktlen, hdpkt,
                                 (size_t)nread, hd.pkt_num);
    if (nwrite < 0) {
      return (int)nwrite;
    }
    pktlen = (size_t)nwrite;
  }

  for (; pktlen;) {
    nread = ngtcp2_pkt_decode_frame(&fr, pkt, pktlen, conn->max_rx_pkt_num);
    if (nread < 0) {
      return (int)nread;
    }

    pkt += nread;
    pktlen -= (size_t)nread;

    rv = conn_call_recv_frame(conn, &hd, &fr);
    if (rv != 0) {
      return rv;
    }

    /* We don't ack packet which contains ACK and CONNECTION_CLOSE
       only. */
    /* TODO What about packet with PADDING frames only? */
    require_ack |=
        fr.type != NGTCP2_FRAME_ACK && fr.type != NGTCP2_FRAME_CONNECTION_CLOSE;

    switch (fr.type) {
    case NGTCP2_FRAME_ACK:
      rv = conn_recv_ack(conn, &fr.ack);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_STREAM:
      rv = conn_recv_stream(conn, &fr.stream);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_MAX_STREAM_DATA:
      rv = conn_recv_max_stream_data(conn, &fr.max_stream_data);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_MAX_DATA:
      conn_recv_max_data(conn, &fr.max_data);
      break;
    }
  }

  conn->max_rx_pkt_num = ngtcp2_max(conn->max_rx_pkt_num, hd.pkt_num);

  if (require_ack) {
    rv = ngtcp2_conn_sched_ack(conn, hd.pkt_num, ts);
    if (rv != 0) {
      return rv;
    }
  }

  return rv;
}

static int conn_process_buffered_protected_pkt(ngtcp2_conn *conn,
                                               ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_pkt_chain *pc = conn->buffed_rx_ppkts, *next;

  for (; pc; pc = pc->next) {
    rv = conn_recv_pkt(conn, pc->pkt, pc->pktlen, ts);
    if (rv != 0) {
      return rv;
    }
  }

  for (pc = conn->buffed_rx_ppkts; pc;) {
    next = pc->next;
    ngtcp2_pkt_chain_del(pc, conn->mem);
    pc = next;
  }

  conn->buffed_rx_ppkts = NULL;

  return 0;
}

int ngtcp2_conn_recv(ngtcp2_conn *conn, uint8_t *pkt, size_t pktlen,
                     ngtcp2_tstamp ts) {
  int rv = 0;

  if (pktlen == 0) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  if (pkt[0] & NGTCP2_HEADER_FORM_BIT) {
    switch (pkt[0] & NGTCP2_LONG_TYPE_MASK) {
    case NGTCP2_PKT_CLIENT_INITIAL:
    case NGTCP2_PKT_SERVER_STATELESS_RETRY:
    case NGTCP2_PKT_SERVER_CLEARTEXT:
    case NGTCP2_PKT_CLIENT_CLEARTEXT:
    case NGTCP2_PKT_PUBLIC_RESET:
      if (ngtcp2_pkt_verify(pkt, pktlen) != 0) {
        return NGTCP2_ERR_BAD_PKT_HASH;
      }
      pktlen -= NGTCP2_PKT_MDLEN;
      break;
    }
  }

  switch (conn->state) {
  case NGTCP2_CS_CLIENT_WAIT_HANDSHAKE:
    rv = conn_recv_handshake_pkt(conn, pkt, pktlen, ts);
    if (rv < 0) {
      break;
    }
    if (conn->handshake_completed) {
      rv = conn_call_handshake_completed(conn);
      if (rv != 0) {
        return rv;
      }
      conn->state = NGTCP2_CS_CLIENT_HANDSHAKE_ALMOST_FINISHED;

      rv = conn_process_buffered_protected_pkt(conn, ts);
      if (rv != 0) {
        return rv;
      }
    }
    break;
  case NGTCP2_CS_SERVER_INITIAL:
  case NGTCP2_CS_SERVER_WAIT_HANDSHAKE:
    rv = conn_recv_handshake_pkt(conn, pkt, pktlen, ts);
    if (rv < 0) {
      break;
    }
    if (conn->handshake_completed) {
      rv = conn_call_handshake_completed(conn);
      if (rv != 0) {
        return rv;
      }
      conn->state = NGTCP2_CS_POST_HANDSHAKE;

      rv = conn_process_buffered_protected_pkt(conn, ts);
      if (rv != 0) {
        return rv;
      }
    }
    break;
  case NGTCP2_CS_POST_HANDSHAKE:
  case NGTCP2_CS_CLOSE_WAIT:
    rv = conn_recv_pkt(conn, pkt, pktlen, ts);
    if (rv < 0) {
      break;
    }
    break;
  }

  return rv;
}

int ngtcp2_conn_emit_pending_recv_handshake(ngtcp2_conn *conn,
                                            ngtcp2_strm *strm,
                                            uint64_t rx_offset) {
  size_t datalen;
  const uint8_t *data;
  int rv;

  for (;;) {
    datalen = ngtcp2_rob_data_at(&strm->rob, &data, rx_offset);
    if (datalen == 0) {
      assert(rx_offset == ngtcp2_strm_rx_offset(strm));
      return 0;
    }

    rx_offset += datalen;

    rv = conn->callbacks.recv_handshake_data(conn, data, datalen,
                                             conn->user_data);
    if (rv != 0) {
      return rv;
    }

    strm->unsent_max_rx_offset += datalen;

    ngtcp2_rob_pop(&strm->rob, rx_offset - datalen, datalen);
  }
}

void ngtcp2_conn_handshake_completed(ngtcp2_conn *conn) {
  conn->handshake_completed = 1;
}

int ngtcp2_conn_sched_ack(ngtcp2_conn *conn, uint64_t pkt_num,
                          ngtcp2_tstamp ts) {
  ngtcp2_acktr_entry *rpkt;
  int rv;

  rv = ngtcp2_acktr_entry_new(&rpkt, pkt_num, ts, conn->mem);
  if (rv != 0) {
    return rv;
  }

  rv = ngtcp2_acktr_add(&conn->acktr, rpkt);
  if (rv != 0) {
    ngtcp2_acktr_entry_del(rpkt, conn->mem);
    return rv;
  }

  if (conn->next_ack_expiry == 0) {
    conn_set_next_ack_expiry(conn, ts);
  }

  return 0;
}

int ngtcp2_accept(ngtcp2_pkt_hd *dest, const uint8_t *pkt, size_t pktlen) {
  ssize_t nread;
  ngtcp2_pkt_hd hd, *p;

  if (dest) {
    p = dest;
  } else {
    p = &hd;
  }

  if (pktlen == 0 || (pkt[0] & NGTCP2_HEADER_FORM_BIT) == 0) {
    return -1;
  }

  nread = ngtcp2_pkt_decode_hd_long(p, pkt, pktlen);
  if (nread < 0) {
    return -1;
  }

  if (p->type != NGTCP2_PKT_CLIENT_INITIAL) {
    return -1;
  }

  if (p->version != NGTCP2_PROTO_VERSION) {
    return 1;
  }

  return 0;
}

void ngtcp2_conn_set_aead_overhead(ngtcp2_conn *conn, size_t aead_overhead) {
  conn->aead_overhead = aead_overhead;
}

int ngtcp2_conn_update_tx_keys(ngtcp2_conn *conn, const uint8_t *key,
                               size_t keylen, const uint8_t *iv, size_t ivlen) {
  if (conn->tx_ckm) {
    return NGTCP2_ERR_INVALID_STATE;
  }

  return ngtcp2_crypto_km_new(&conn->tx_ckm, key, keylen, iv, ivlen, conn->mem);
}

int ngtcp2_conn_update_rx_keys(ngtcp2_conn *conn, const uint8_t *key,
                               size_t keylen, const uint8_t *iv, size_t ivlen) {
  if (conn->rx_ckm) {
    return NGTCP2_ERR_INVALID_STATE;
  }

  return ngtcp2_crypto_km_new(&conn->rx_ckm, key, keylen, iv, ivlen, conn->mem);
}

ngtcp2_tstamp ngtcp2_conn_earliest_expiry(ngtcp2_conn *conn) {
  ngtcp2_rtb_entry *ent = ngtcp2_rtb_top(&conn->rtb);

  if (ent == NULL) {
    return conn->next_ack_expiry;
  }

  if (conn->next_ack_expiry > 0) {
    return ngtcp2_min(conn->next_ack_expiry, ent->expiry);
  } else {
    return ent->expiry;
  }
}

int ngtcp2_pkt_chain_new(ngtcp2_pkt_chain **ppc, const uint8_t *pkt,
                         size_t pktlen, ngtcp2_tstamp ts, ngtcp2_mem *mem) {
  *ppc = ngtcp2_mem_malloc(mem, sizeof(ngtcp2_pkt_chain) + pktlen);
  if (*ppc == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  (*ppc)->next = NULL;
  (*ppc)->pkt = (uint8_t *)(*ppc) + sizeof(ngtcp2_pkt_chain);
  (*ppc)->pktlen = pktlen;
  (*ppc)->ts = ts;

  memcpy((*ppc)->pkt, pkt, pktlen);

  return 0;
}

void ngtcp2_pkt_chain_del(ngtcp2_pkt_chain *pc, ngtcp2_mem *mem) {
  ngtcp2_mem_free(mem, pc);
}

static void
settings_copy_from_transport_params(ngtcp2_settings *dest,
                                    const ngtcp2_transport_params *src) {
  dest->max_stream_data = src->initial_max_stream_data;
  dest->max_data = src->initial_max_data;
  dest->max_stream_id = src->initial_max_stream_id;
  dest->idle_timeout = src->idle_timeout;
  dest->omit_connection_id = src->omit_connection_id;
  dest->max_packet_size = src->max_packet_size;
}

static void transport_params_copy_from_settings(ngtcp2_transport_params *dest,
                                                const ngtcp2_settings *src) {
  dest->initial_max_stream_data = src->max_stream_data;
  dest->initial_max_data = (uint32_t)src->max_data;
  dest->initial_max_stream_id = src->max_stream_id;
  dest->idle_timeout = src->idle_timeout;
  dest->omit_connection_id = src->omit_connection_id;
  dest->max_packet_size = src->max_packet_size;
}

int ngtcp2_conn_set_remote_transport_params(
    ngtcp2_conn *conn, uint8_t exttype, const ngtcp2_transport_params *params) {
  switch (exttype) {
  case NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO:
    if (!conn->server) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
    /* TODO More extensive validation is required */
    if (conn->server && params->v.ch.negotiated_version != conn->version) {
      return NGTCP2_ERR_PROTO;
    }
    break;
  case NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS:
  case NGTCP2_TRANSPORT_PARAMS_TYPE_NEW_SESSION_TICKET:
    if (conn->server) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
    break;
  default:
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  settings_copy_from_transport_params(&conn->remote_settings, params);

  conn->max_tx_offset_high = conn->remote_settings.max_data;

  /* TODO Should we check that conn->max_remote_stream_id is larger
     than conn->remote_settings.max_stream_id here?  What happens for
     0-RTT stream? */

  return 0;
}

int ngtcp2_conn_get_local_transport_params(ngtcp2_conn *conn,
                                           ngtcp2_transport_params *params,
                                           uint8_t exttype) {
  switch (exttype) {
  case NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO:
    if (conn->server) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
    /* TODO Fix this; not sure how to handle them correctly */
    params->v.ch.initial_version = conn->version;
    params->v.ch.negotiated_version = conn->version;
    break;
  case NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS:
    if (!conn->server) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
    /* TODO Fix this; not sure how to handle them correctly */
    params->v.ee.len = 1;
    params->v.ee.supported_versions[0] = conn->version;
    break;
  case NGTCP2_TRANSPORT_PARAMS_TYPE_NEW_SESSION_TICKET:
    if (!conn->server) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
    break;
  default:
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }
  transport_params_copy_from_settings(params, &conn->local_settings);
  return 0;
}

int ngtcp2_conn_open_stream(ngtcp2_conn *conn, uint32_t stream_id,
                            void *stream_user_data) {
  int rv;
  ngtcp2_strm *strm;

  if (!conn_local_stream(conn, stream_id)) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  if (stream_id > conn->remote_settings.max_stream_id) {
    return NGTCP2_ERR_STREAM_ID_BLOCKED;
  }

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm != NULL) {
    return NGTCP2_ERR_STREAM_IN_USE;
  }

  rv = ngtcp2_idtr_open(&conn->local_idtr, id_from_stream_id(stream_id));
  if (rv != 0) {
    return rv;
  }

  strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
  if (strm == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  rv = ngtcp2_conn_init_stream(conn, strm, stream_id, stream_user_data);
  if (rv != 0) {
    return rv;
  }

  return 0;
}

ngtcp2_strm *ngtcp2_conn_find_stream(ngtcp2_conn *conn, uint32_t stream_id) {
  ngtcp2_map_entry *me;

  me = ngtcp2_map_find(&conn->strms, stream_id);
  if (me == NULL) {
    return NULL;
  }

  return ngtcp2_struct_of(me, ngtcp2_strm, me);
}

ssize_t ngtcp2_conn_write_stream(ngtcp2_conn *conn, uint8_t *dest,
                                 size_t destlen, size_t *pdatalen,
                                 uint32_t stream_id, uint8_t fin,
                                 const uint8_t *data, size_t datalen,
                                 ngtcp2_tstamp ts) {
  ngtcp2_strm *strm;
  ngtcp2_frame_chain *frc;
  ngtcp2_pkt_hd hd;
  ngtcp2_ppe ppe;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_rtb_entry *ent;
  int rv;
  size_t ndatalen, left;
  ssize_t nwrite;

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_CONN_ID, NGTCP2_PKT_03, conn->conn_id,
                     conn->next_tx_pkt_num, conn->version);

  ctx.ckm = conn->tx_ckm;
  ctx.aead_overhead = conn->aead_overhead;
  ctx.encrypt = conn->callbacks.encrypt;
  ctx.user_data = conn;

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx, conn->mem);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    return rv;
  }

  left = ngtcp2_ppe_left(&ppe);
  if (left <= NGTCP2_STREAM_OVERHEAD) {
    return NGTCP2_ERR_NOBUF;
  }

  left -= NGTCP2_STREAM_OVERHEAD;

  /* TODO Take into account flow control credit here */
  ndatalen = ngtcp2_min(datalen, left);
  ndatalen = ngtcp2_min(ndatalen, strm->max_tx_offset - strm->tx_offset);
  if (conn->max_tx_offset_high - conn->tx_offset_high <=
      (ndatalen + conn->tx_offset_low) / 1024) {
    ndatalen =
        ngtcp2_min(ndatalen,
                   (conn->max_tx_offset_high - conn->tx_offset_high) * 1024 -
                       conn->tx_offset_low);
  }

  if (datalen > 0 && ndatalen == 0) {
    return NGTCP2_ERR_STREAM_DATA_BLOCKED;
  }

  rv = conn_call_send_pkt(conn, &hd);
  if (rv != 0) {
    return rv;
  }

  rv = ngtcp2_frame_chain_new(&frc, conn->mem);
  if (rv != 0) {
    return rv;
  }

  frc->fr.type = NGTCP2_FRAME_STREAM;
  frc->fr.stream.flags = 0;
  frc->fr.stream.fin = fin && ndatalen == datalen;
  frc->fr.stream.stream_id = stream_id;
  frc->fr.stream.offset = strm->tx_offset;
  frc->fr.stream.datalen = ndatalen;
  frc->fr.stream.data = data;

  rv = ngtcp2_ppe_encode_frame(&ppe, &frc->fr);
  if (rv != 0) {
    ngtcp2_frame_chain_del(frc, conn->mem);
    return rv;
  }

  rv = conn_call_send_frame(conn, &hd, &frc->fr);
  if (rv != 0) {
    ngtcp2_frame_chain_del(frc, conn->mem);
    return rv;
  }

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    ngtcp2_frame_chain_del(frc, conn->mem);
    return nwrite;
  }

  rv = ngtcp2_rtb_entry_new(&ent, &hd, frc, ts + NGTCP2_INITIAL_EXPIRY,
                            conn->mem);
  if (rv != 0) {
    ngtcp2_frame_chain_del(frc, conn->mem);
    return rv;
  }

  rv = ngtcp2_rtb_add(&conn->rtb, ent);
  if (rv != 0) {
    ngtcp2_rtb_entry_del(ent, conn->mem);
    return rv;
  }

  strm->tx_offset += ndatalen;
  ngtcp2_increment_offset(&conn->tx_offset_high, &conn->tx_offset_low,
                          ndatalen);
  ++conn->next_tx_pkt_num;

  if (pdatalen) {
    *pdatalen = ndatalen;
  }

  if (fin) {
    ngtcp2_strm_shutdown(strm, NGTCP2_STRM_FLAG_SHUT_WR);
    rv = ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm);
    if (rv != 0) {
      return rv;
    }
  }

  return nwrite;
}

int ngtcp2_conn_close_stream(ngtcp2_conn *conn, ngtcp2_strm *strm) {
  int rv;

  rv = ngtcp2_map_remove(&conn->strms, strm->me.key);
  if (rv != 0) {
    return rv;
  }

  if (!conn_local_stream(conn, strm->stream_id) &&
      conn->max_remote_stream_id <= UINT32_MAX - 2) {
    conn->max_remote_stream_id += 2;
  }

  if (strm->fc_pprev) {
    *strm->fc_pprev = strm->fc_next;
    if (strm->fc_next) {
      strm->fc_next->fc_pprev = strm->fc_pprev;
    }
  }

  ngtcp2_strm_free(strm);
  ngtcp2_mem_free(conn->mem, strm);

  return 0;
}

int ngtcp2_conn_close_stream_if_shut_rdwr(ngtcp2_conn *conn,
                                          ngtcp2_strm *strm) {
  if ((strm->flags & NGTCP2_STRM_FLAG_SHUT_RDWR) ==
          NGTCP2_STRM_FLAG_SHUT_RDWR &&
      ngtcp2_rob_first_gap_offset(&strm->rob) == strm->last_rx_offset &&
      ngtcp2_gaptr_first_gap_offset(&strm->acked_tx_offset) ==
          strm->tx_offset) {
    return ngtcp2_conn_close_stream(conn, strm);
  }
  return 0;
}

int ngtcp2_conn_extend_max_stream_offset(ngtcp2_conn *conn, uint32_t stream_id,
                                         size_t datalen) {
  ngtcp2_strm *strm;

  if (stream_id == 0) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  if (strm->unsent_max_rx_offset <= UINT64_MAX - datalen) {
    strm->unsent_max_rx_offset += datalen;
  }

  if (!strm->fc_pprev && conn_should_send_max_stream_data(conn, strm)) {
    strm->fc_pprev = &conn->fc_strms;
    if (conn->fc_strms) {
      strm->fc_next = conn->fc_strms;
      conn->fc_strms->fc_pprev = &strm->fc_next;
    }
    conn->fc_strms = strm;
  }

  return 0;
}

void ngtcp2_conn_extend_max_offset(ngtcp2_conn *conn, size_t datalen) {
  ngtcp2_increment_offset(&conn->unsent_max_rx_offset_high,
                          &conn->unsent_max_rx_offset_low, datalen);
}
