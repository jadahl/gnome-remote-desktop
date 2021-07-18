/*
 * Copyright (C) 2021 Pascal Nowack
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "config.h"

#include "grd-rdp-graphics-pipeline.h"

#include <winpr/sysinfo.h>

#include "grd-rdp-frame-info.h"
#include "grd-rdp-gfx-surface.h"
#include "grd-rdp-surface.h"
#include "grd-session-rdp.h"

#define MAX_TRACKED_ENC_FRAMES 1000

typedef struct _GfxSurfaceContext
{
  GrdRdpGfxSurface *gfx_surface;
  uint64_t ref_count;
} GfxSurfaceContext;

typedef struct _GfxFrameInfo
{
  GrdRdpFrameInfo frame_info;
  uint32_t surface_serial;
} GfxFrameInfo;

struct _GrdRdpGraphicsPipeline
{
  GObject parent;

  RdpgfxServerContext *rdpgfx_context;
  HANDLE stop_event;
  gboolean channel_opened;
  gboolean initialized;
  uint32_t initial_version;

  GrdSessionRdp *session_rdp;
  wStream *encode_stream;
  RFX_CONTEXT *rfx_context;

  GSource *protocol_reset_source;
  GMutex caps_mutex;
  RDPGFX_CAPSET *cap_sets;
  uint16_t n_cap_sets;

  GMutex gfx_mutex;
  GHashTable *surface_table;
  GHashTable *codec_context_table;

  /* Unacknowledged Frames ADM element */
  GHashTable *frame_serial_table;

  GHashTable *serial_surface_table;
  gboolean frame_acks_suspended;

  GQueue *encoded_frames;
  uint32_t total_frames_encoded;

  uint32_t next_frame_id;
  uint16_t next_surface_id;
  uint32_t next_serial;
};

G_DEFINE_TYPE (GrdRdpGraphicsPipeline, grd_rdp_graphics_pipeline, G_TYPE_OBJECT);

void
grd_rdp_graphics_pipeline_create_surface (GrdRdpGraphicsPipeline *graphics_pipeline,
                                          GrdRdpGfxSurface       *gfx_surface)
{
  RdpgfxServerContext *rdpgfx_context = graphics_pipeline->rdpgfx_context;
  RDPGFX_CREATE_SURFACE_PDU create_surface = {0};
  GrdRdpSurface *rdp_surface = grd_rdp_gfx_surface_get_rdp_surface (gfx_surface);
  uint16_t surface_id = grd_rdp_gfx_surface_get_surface_id (gfx_surface);
  uint32_t surface_serial = grd_rdp_gfx_surface_get_serial (gfx_surface);
  GfxSurfaceContext *surface_context;

  surface_context = g_malloc0 (sizeof (GfxSurfaceContext));

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  g_hash_table_insert (graphics_pipeline->surface_table,
                       GUINT_TO_POINTER (surface_id), gfx_surface);

  surface_context->gfx_surface = gfx_surface;
  g_hash_table_insert (graphics_pipeline->serial_surface_table,
                       GUINT_TO_POINTER (surface_serial), surface_context);
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);

  create_surface.surfaceId = surface_id;
  create_surface.width = rdp_surface->width;
  create_surface.height = rdp_surface->height;
  create_surface.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;

  rdpgfx_context->CreateSurface (rdpgfx_context, &create_surface);
}

void
grd_rdp_graphics_pipeline_delete_surface (GrdRdpGraphicsPipeline *graphics_pipeline,
                                          GrdRdpGfxSurface       *gfx_surface)
{
  RdpgfxServerContext *rdpgfx_context = graphics_pipeline->rdpgfx_context;
  RDPGFX_DELETE_ENCODING_CONTEXT_PDU delete_encoding_context = {0};
  RDPGFX_DELETE_SURFACE_PDU delete_surface = {0};
  gboolean needs_encoding_context_deletion = FALSE;
  GfxSurfaceContext *surface_context;
  uint16_t surface_id;
  uint32_t codec_context_id;
  uint32_t surface_serial;

  if (!graphics_pipeline->channel_opened)
    return;

  surface_id = grd_rdp_gfx_surface_get_surface_id (gfx_surface);
  codec_context_id = grd_rdp_gfx_surface_get_codec_context_id (gfx_surface);
  surface_serial = grd_rdp_gfx_surface_get_serial (gfx_surface);

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  if (!g_hash_table_lookup_extended (graphics_pipeline->serial_surface_table,
                                     GUINT_TO_POINTER (surface_serial),
                                     NULL, (gpointer *) &surface_context))
    g_assert_not_reached ();

  surface_context->gfx_surface = NULL;
  if (surface_context->ref_count == 0)
    {
      g_hash_table_remove (graphics_pipeline->serial_surface_table,
                           GUINT_TO_POINTER (surface_serial));
    }

  if (g_hash_table_steal_extended (graphics_pipeline->codec_context_table,
                                   GUINT_TO_POINTER (codec_context_id),
                                   NULL, NULL))
    needs_encoding_context_deletion = TRUE;

  g_hash_table_remove (graphics_pipeline->surface_table,
                       GUINT_TO_POINTER (surface_id));
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);

  if (needs_encoding_context_deletion)
    {
      delete_encoding_context.surfaceId = surface_id;
      delete_encoding_context.codecContextId = codec_context_id;

      rdpgfx_context->DeleteEncodingContext (rdpgfx_context,
                                             &delete_encoding_context);
    }

  delete_surface.surfaceId = surface_id;

  rdpgfx_context->DeleteSurface (rdpgfx_context, &delete_surface);
}

void
grd_rdp_graphics_pipeline_reset_graphics (GrdRdpGraphicsPipeline *graphics_pipeline,
                                          uint32_t                width,
                                          uint32_t                height,
                                          MONITOR_DEF            *monitors,
                                          uint32_t                n_monitors)
{
  RdpgfxServerContext *rdpgfx_context = graphics_pipeline->rdpgfx_context;
  RDPGFX_RESET_GRAPHICS_PDU reset_graphics = {0};
  GList *surfaces;
  GList *l;

  g_debug ("[RDP.RDPGFX] Resetting graphics");

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  surfaces = g_hash_table_get_values (graphics_pipeline->surface_table);

  g_hash_table_steal_all (graphics_pipeline->surface_table);
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);

  for (l = surfaces; l; l = l->next)
    {
      GrdRdpGfxSurface *gfx_surface = l->data;
      GrdRdpSurface *rdp_surface;

      rdp_surface = grd_rdp_gfx_surface_get_rdp_surface (gfx_surface);
      g_clear_object (&rdp_surface->gfx_surface);
    }
  g_list_free (surfaces);

  /*
   * width and height refer here to the size of the Graphics Output Buffer
   * ADM (Abstract Data Model) element
   */
  reset_graphics.width = width;
  reset_graphics.height = height;
  reset_graphics.monitorCount = n_monitors;
  reset_graphics.monitorDefArray = monitors;

  rdpgfx_context->ResetGraphics (rdpgfx_context, &reset_graphics);
}

void
grd_rdp_graphics_pipeline_notify_new_round_trip_time (GrdRdpGraphicsPipeline *graphics_pipeline,
                                                      uint64_t                round_trip_time_us)
{
  GrdRdpGfxSurface *gfx_surface;
  GHashTableIter iter;

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  g_hash_table_iter_init (&iter, graphics_pipeline->surface_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &gfx_surface))
    grd_rdp_gfx_surface_notify_new_round_trip_time (gfx_surface, round_trip_time_us);
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);
}

static uint32_t
get_next_free_frame_id (GrdRdpGraphicsPipeline *graphics_pipeline)
{
  uint32_t frame_id = graphics_pipeline->next_frame_id;

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  while (g_hash_table_contains (graphics_pipeline->frame_serial_table,
                                GUINT_TO_POINTER (frame_id)))
    ++frame_id;
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);

  graphics_pipeline->next_frame_id = frame_id + 1;

  return frame_id;
}

static void
surface_serial_ref (GrdRdpGraphicsPipeline *graphics_pipeline,
                    uint32_t                surface_serial)
{
  GfxSurfaceContext *surface_context;

  if (!g_hash_table_lookup_extended (graphics_pipeline->serial_surface_table,
                                     GUINT_TO_POINTER (surface_serial),
                                     NULL, (gpointer *) &surface_context))
    g_assert_not_reached ();

  ++surface_context->ref_count;
}

static void
surface_serial_unref (GrdRdpGraphicsPipeline *graphics_pipeline,
                      uint32_t                surface_serial)
{
  GfxSurfaceContext *surface_context;

  if (!g_hash_table_lookup_extended (graphics_pipeline->serial_surface_table,
                                     GUINT_TO_POINTER (surface_serial),
                                     NULL, (gpointer *) &surface_context))
    g_assert_not_reached ();

  g_assert (surface_context->ref_count > 0);
  --surface_context->ref_count;

  if (!surface_context->gfx_surface && surface_context->ref_count == 0)
    {
      g_hash_table_remove (graphics_pipeline->serial_surface_table,
                           GUINT_TO_POINTER (surface_serial));
    }
}

static void
gfx_frame_info_free (GrdRdpGraphicsPipeline *graphics_pipeline,
                     GfxFrameInfo           *gfx_frame_info)
{
  uint32_t surface_serial = gfx_frame_info->surface_serial;

  g_hash_table_remove (graphics_pipeline->frame_serial_table,
                       GUINT_TO_POINTER (gfx_frame_info->frame_info.frame_id));
  surface_serial_unref (graphics_pipeline, surface_serial);

  g_free (gfx_frame_info);
}

static void
reduce_tracked_frame_infos (GrdRdpGraphicsPipeline *graphics_pipeline,
                            uint32_t                max_tracked_frames)
{
  while (g_queue_peek_head (graphics_pipeline->encoded_frames) &&
         g_queue_get_length (graphics_pipeline->encoded_frames) > max_tracked_frames)
    {
      gfx_frame_info_free (graphics_pipeline,
                           g_queue_pop_head (graphics_pipeline->encoded_frames));
    }
}

static void
enqueue_tracked_frame_info (GrdRdpGraphicsPipeline *graphics_pipeline,
                            uint32_t                surface_serial,
                            uint32_t                frame_id,
                            int64_t                 enc_time_us)
{
  GfxFrameInfo *gfx_frame_info;

  g_assert (MAX_TRACKED_ENC_FRAMES > 1);
  reduce_tracked_frame_infos (graphics_pipeline, MAX_TRACKED_ENC_FRAMES - 1);

  gfx_frame_info = g_malloc0 (sizeof (GfxFrameInfo));
  gfx_frame_info->frame_info.frame_id = frame_id;
  gfx_frame_info->frame_info.enc_time_us = enc_time_us;
  gfx_frame_info->surface_serial = surface_serial;

  g_queue_push_tail (graphics_pipeline->encoded_frames, gfx_frame_info);
}

static gboolean
rfx_progressive_write_message (RFX_MESSAGE *rfx_message,
                               wStream     *s,
                               gboolean     needs_progressive_header)
{
  uint32_t block_len;
  uint32_t *qv;
  RFX_TILE *rfx_tile;
  uint32_t tiles_data_size;
  uint16_t i;

  if (needs_progressive_header)
    {
      /* RFX_PROGRESSIVE_SYNC */
      block_len = 12;
      if (!Stream_EnsureRemainingCapacity (s, block_len))
        return FALSE;

      Stream_Write_UINT16 (s, 0xCCC0);     /* blockType */
      Stream_Write_UINT32 (s, block_len);  /* blockLen */
      Stream_Write_UINT32 (s, 0xCACCACCA); /* magic */
      Stream_Write_UINT16 (s, 0x0100);     /* version */

      /* RFX_PROGRESSIVE_CONTEXT */
      block_len = 10;
      if (!Stream_EnsureRemainingCapacity (s, block_len))
        return FALSE;

      Stream_Write_UINT16 (s, 0xCCC3);    /* blockType */
      Stream_Write_UINT32 (s, block_len); /* blockLen */
      Stream_Write_UINT8 (s, 0);          /* ctxId */
      Stream_Write_UINT16 (s, 0x0040);    /* tileSize */
      Stream_Write_UINT8 (s, 0);          /* flags */
    }

  /* RFX_PROGRESSIVE_FRAME_BEGIN */
  block_len = 12;
  if (!Stream_EnsureRemainingCapacity (s, block_len))
    return FALSE;

  Stream_Write_UINT16 (s, 0xCCC1);                /* blockType */
  Stream_Write_UINT32 (s, block_len);             /* blockLen */
  Stream_Write_UINT32 (s, rfx_message->frameIdx); /* frameIndex */
  Stream_Write_UINT16 (s, 1);                     /* regionCount */

  /* RFX_PROGRESSIVE_REGION */
  block_len = 18;
  block_len += rfx_message->numRects * 8;
  block_len += rfx_message->numQuant * 5;
  tiles_data_size = rfx_message->numTiles * 22;

  for (i = 0; i < rfx_message->numTiles; i++)
    {
      rfx_tile = rfx_message->tiles[i];
      tiles_data_size += rfx_tile->YLen + rfx_tile->CbLen + rfx_tile->CrLen;
    }

  block_len += tiles_data_size;
  if (!Stream_EnsureRemainingCapacity (s, block_len))
    return FALSE;

  Stream_Write_UINT16 (s, 0xCCC4);                /* blockType */
  Stream_Write_UINT32 (s, block_len);             /* blockLen */
  Stream_Write_UINT8 (s, 0x40);                   /* tileSize */
  Stream_Write_UINT16 (s, rfx_message->numRects); /* numRects */
  Stream_Write_UINT8 (s, rfx_message->numQuant);  /* numQuant */
  Stream_Write_UINT8 (s, 0);                      /* numProgQuant */
  Stream_Write_UINT8 (s, 0);                      /* flags */
  Stream_Write_UINT16 (s, rfx_message->numTiles); /* numTiles */
  Stream_Write_UINT32 (s, tiles_data_size);       /* tilesDataSize */

  for (i = 0; i < rfx_message->numRects; i++)
    {
      /* TS_RFX_RECT */
      Stream_Write_UINT16 (s, rfx_message->rects[i].x);      /* x */
      Stream_Write_UINT16 (s, rfx_message->rects[i].y);      /* y */
      Stream_Write_UINT16 (s, rfx_message->rects[i].width);  /* width */
      Stream_Write_UINT16 (s, rfx_message->rects[i].height); /* height */
    }

  /*
   * The RFX_COMPONENT_CODEC_QUANT structure differs from the
   * TS_RFX_CODEC_QUANT ([MS-RDPRFX] section 2.2.2.1.5) structure with respect
   * to the order of the bands.
   *             0    1    2    3    4    5    6    7    8    9
   * RDPRFX:   LL3, LH3, HL3, HH3, LH2, HL2, HH2, LH1, HL1, HH1
   * RDPEGFX:  LL3, HL3, LH3, HH3, HL2, LH2, HH2, HL1, LH1, HH1
   */
  for (i = 0, qv = rfx_message->quantVals; i < rfx_message->numQuant; ++i, qv += 10)
    {
      /* RFX_COMPONENT_CODEC_QUANT */
      Stream_Write_UINT8 (s, qv[0] + (qv[2] << 4)); /* LL3, HL3 */
      Stream_Write_UINT8 (s, qv[1] + (qv[3] << 4)); /* LH3, HH3 */
      Stream_Write_UINT8 (s, qv[5] + (qv[4] << 4)); /* HL2, LH2 */
      Stream_Write_UINT8 (s, qv[6] + (qv[8] << 4)); /* HH2, HL1 */
      Stream_Write_UINT8 (s, qv[7] + (qv[9] << 4)); /* LH1, HH1 */
    }

  for (i = 0; i < rfx_message->numTiles; ++i)
    {
      /* RFX_PROGRESSIVE_TILE_SIMPLE */
      rfx_tile = rfx_message->tiles[i];
      block_len = 22 + rfx_tile->YLen + rfx_tile->CbLen + rfx_tile->CrLen;
      Stream_Write_UINT16 (s, 0xCCC5);                     /* blockType */
      Stream_Write_UINT32 (s, block_len);                  /* blockLen */
      Stream_Write_UINT8 (s, rfx_tile->quantIdxY);         /* quantIdxY */
      Stream_Write_UINT8 (s, rfx_tile->quantIdxCb);        /* quantIdxCb */
      Stream_Write_UINT8 (s, rfx_tile->quantIdxCr);        /* quantIdxCr */
      Stream_Write_UINT16 (s, rfx_tile->xIdx);             /* xIdx */
      Stream_Write_UINT16 (s, rfx_tile->yIdx);             /* yIdx */
      Stream_Write_UINT8 (s, 0);                           /* flags */
      Stream_Write_UINT16 (s, rfx_tile->YLen);             /* YLen */
      Stream_Write_UINT16 (s, rfx_tile->CbLen);            /* CbLen */
      Stream_Write_UINT16 (s, rfx_tile->CrLen);            /* CrLen */
      Stream_Write_UINT16 (s, 0);                          /* tailLen */
      Stream_Write (s, rfx_tile->YData, rfx_tile->YLen);   /* YData */
      Stream_Write (s, rfx_tile->CbData, rfx_tile->CbLen); /* CbData */
      Stream_Write (s, rfx_tile->CrData, rfx_tile->CrLen); /* CrData */
    }

  /* RFX_PROGRESSIVE_FRAME_END */
  block_len = 6;
  if (!Stream_EnsureRemainingCapacity (s, block_len))
    return FALSE;

  Stream_Write_UINT16 (s, 0xCCC2);    /* blockType */
  Stream_Write_UINT32 (s, block_len); /* blockLen */

  return TRUE;
}

static void
refresh_gfx_surface_rfx_progressive (GrdRdpGraphicsPipeline *graphics_pipeline,
                                     GrdRdpSurface          *rdp_surface,
                                     cairo_region_t         *region,
                                     uint8_t                *src_data)
{
  RdpgfxServerContext *rdpgfx_context = graphics_pipeline->rdpgfx_context;
  GrdSessionRdp *session_rdp = graphics_pipeline->session_rdp;
  GrdRdpGfxSurface *gfx_surface = rdp_surface->gfx_surface;
  uint32_t src_stride = grd_session_rdp_get_stride_for_width (session_rdp,
                                                              rdp_surface->width);
  RDPGFX_SURFACE_COMMAND cmd = {0};
  RDPGFX_START_FRAME_PDU cmd_start = {0};
  RDPGFX_END_FRAME_PDU cmd_end = {0};
  gboolean needs_progressive_header = FALSE;
  cairo_rectangle_int_t cairo_rect;
  RFX_RECT *rfx_rects, *rfx_rect;
  int n_rects;
  RFX_MESSAGE *rfx_message;
  SYSTEMTIME system_time;
  uint32_t codec_context_id;
  uint32_t surface_serial;
  int64_t enc_ack_time_us;
  int i;

  graphics_pipeline->rfx_context->mode = RLGR1;
  if (!rdp_surface->valid)
    {
      rfx_context_reset (graphics_pipeline->rfx_context,
                         rdp_surface->width, rdp_surface->height);
      rdp_surface->valid = TRUE;
    }

  codec_context_id = grd_rdp_gfx_surface_get_codec_context_id (gfx_surface);
  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  if (!g_hash_table_contains (graphics_pipeline->codec_context_table,
                              GUINT_TO_POINTER (codec_context_id)))
    needs_progressive_header = TRUE;
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);

  n_rects = cairo_region_num_rectangles (region);
  rfx_rects = g_malloc0 (n_rects * sizeof (RFX_RECT));
  for (i = 0; i < n_rects; ++i)
    {
      cairo_region_get_rectangle (region, i, &cairo_rect);

      rfx_rect = &rfx_rects[i];
      rfx_rect->x = cairo_rect.x;
      rfx_rect->y = cairo_rect.y;
      rfx_rect->width = cairo_rect.width;
      rfx_rect->height = cairo_rect.height;
    }

  rfx_message = rfx_encode_message (graphics_pipeline->rfx_context,
                                    rfx_rects,
                                    n_rects,
                                    src_data,
                                    rdp_surface->width,
                                    rdp_surface->height,
                                    src_stride);
  g_free (rfx_rects);

  GetSystemTime (&system_time);
  cmd_start.timestamp = system_time.wHour << 22 |
                        system_time.wMinute << 16 |
                        system_time.wSecond << 10 |
                        system_time.wMilliseconds;
  cmd_start.frameId = get_next_free_frame_id (graphics_pipeline);
  cmd_end.frameId = cmd_start.frameId;

  cmd.surfaceId = grd_rdp_gfx_surface_get_surface_id (gfx_surface);
  cmd.codecId = RDPGFX_CODECID_CAPROGRESSIVE;
  cmd.contextId = codec_context_id;
  cmd.format = PIXEL_FORMAT_BGRX32;

  Stream_SetPosition (graphics_pipeline->encode_stream, 0);
  if (!rfx_progressive_write_message (rfx_message,
                                      graphics_pipeline->encode_stream,
                                      needs_progressive_header))
    {
      g_warning ("[RDP.RDPGFX] rfx_progressive_write_message() failed");
      rfx_message_free (graphics_pipeline->rfx_context, rfx_message);
      return;
    }
  rfx_message_free (graphics_pipeline->rfx_context, rfx_message);

  cmd.length = Stream_GetPosition (graphics_pipeline->encode_stream);
  cmd.data = Stream_Buffer (graphics_pipeline->encode_stream);

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  if (needs_progressive_header)
    {
      g_hash_table_insert (graphics_pipeline->codec_context_table,
                           GUINT_TO_POINTER (codec_context_id), gfx_surface);
    }

  enc_ack_time_us = g_get_monotonic_time ();
  grd_rdp_gfx_surface_unack_frame (gfx_surface, cmd_start.frameId,
                                   enc_ack_time_us);

  surface_serial = grd_rdp_gfx_surface_get_serial (gfx_surface);
  g_hash_table_insert (graphics_pipeline->frame_serial_table,
                       GUINT_TO_POINTER (cmd_start.frameId),
                       GUINT_TO_POINTER (surface_serial));
  surface_serial_ref (graphics_pipeline, surface_serial);
  ++graphics_pipeline->total_frames_encoded;

  if (graphics_pipeline->frame_acks_suspended)
    {
      grd_rdp_gfx_surface_ack_frame (gfx_surface, cmd_start.frameId,
                                     enc_ack_time_us);
      enqueue_tracked_frame_info (graphics_pipeline, surface_serial,
                                  cmd_start.frameId, enc_ack_time_us);
    }
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);

  rdpgfx_context->SurfaceFrameCommand (rdpgfx_context, &cmd,
                                       &cmd_start, &cmd_end);
}

static uint16_t
get_next_free_surface_id (GrdRdpGraphicsPipeline *graphics_pipeline)
{
  uint16_t surface_id = graphics_pipeline->next_surface_id;

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  while (g_hash_table_contains (graphics_pipeline->surface_table,
                                GUINT_TO_POINTER (surface_id)))
    ++surface_id;
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);

  graphics_pipeline->next_surface_id = surface_id + 1;

  return surface_id;
}

static uint32_t
get_next_free_serial (GrdRdpGraphicsPipeline *graphics_pipeline)
{
  uint32_t serial = graphics_pipeline->next_serial;

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  while (g_hash_table_contains (graphics_pipeline->serial_surface_table,
                                GUINT_TO_POINTER (serial)))
    ++serial;
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);

  graphics_pipeline->next_serial = serial + 1;

  return serial;
}

static void
map_surface_to_output (GrdRdpGraphicsPipeline *graphics_pipeline,
                       GrdRdpGfxSurface       *gfx_surface)
{
  RdpgfxServerContext *rdpgfx_context = graphics_pipeline->rdpgfx_context;
  RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map_surface_to_output = {0};
  GrdRdpSurface *rdp_surface = grd_rdp_gfx_surface_get_rdp_surface (gfx_surface);
  uint16_t surface_id = grd_rdp_gfx_surface_get_surface_id (gfx_surface);

  map_surface_to_output.surfaceId = surface_id;
  map_surface_to_output.outputOriginX = rdp_surface->output_origin_x;
  map_surface_to_output.outputOriginY = rdp_surface->output_origin_y;

  rdpgfx_context->MapSurfaceToOutput (rdpgfx_context, &map_surface_to_output);
}

void
grd_rdp_graphics_pipeline_refresh_gfx (GrdRdpGraphicsPipeline *graphics_pipeline,
                                       GrdRdpSurface          *rdp_surface,
                                       cairo_region_t         *region,
                                       uint8_t                *src_data)
{
  GrdSessionRdp *session_rdp = graphics_pipeline->session_rdp;

  if (!rdp_surface->gfx_surface)
    rdp_surface->valid = FALSE;
  if (!rdp_surface->valid)
    g_clear_object (&rdp_surface->gfx_surface);
  if (!rdp_surface->gfx_surface)
    {
      rdp_surface->gfx_surface = grd_rdp_gfx_surface_new (
        graphics_pipeline, session_rdp, rdp_surface,
        get_next_free_surface_id (graphics_pipeline),
        get_next_free_serial (graphics_pipeline));
      map_surface_to_output (graphics_pipeline, rdp_surface->gfx_surface);
    }

  refresh_gfx_surface_rfx_progressive (graphics_pipeline, rdp_surface,
                                       region, src_data);
}

static uint32_t cap_list[] =
{
  RDPGFX_CAPVERSION_106,
  RDPGFX_CAPVERSION_105,
  RDPGFX_CAPVERSION_104,
  RDPGFX_CAPVERSION_103,
  RDPGFX_CAPVERSION_102,
  RDPGFX_CAPVERSION_101,
  RDPGFX_CAPVERSION_10,
  RDPGFX_CAPVERSION_81,
  RDPGFX_CAPVERSION_8,
};

static gboolean
cap_sets_contains_supported_version (RDPGFX_CAPSET *cap_sets,
                                     uint16_t       n_cap_sets)
{
  size_t i;
  uint16_t j;

  for (i = 0; i < G_N_ELEMENTS (cap_list); ++i)
    {
      for (j = 0; j < n_cap_sets; ++j)
        {
          if (cap_sets[j].version == cap_list[i])
            return TRUE;
        }
    }

  g_warning ("[RDP.RDPGFX] Client did not advertise any supported "
             "capability set");

  return FALSE;
}

static uint32_t
rdpgfx_caps_advertise (RdpgfxServerContext             *rdpgfx_context,
                       const RDPGFX_CAPS_ADVERTISE_PDU *caps_advertise)
{
  GrdRdpGraphicsPipeline *graphics_pipeline = rdpgfx_context->custom;
  GrdSessionRdp *session_rdp = graphics_pipeline->session_rdp;

  g_debug ("[RDP.RDPGFX] Received a CapsAdvertise PDU");

  if (graphics_pipeline->initialized &&
      graphics_pipeline->initial_version < RDPGFX_CAPVERSION_103)
    {
      g_warning ("[RDP.RDPGFX] Protocol violation: Received an illegal "
                 "CapsAdvertise PDU (RDPGFX: initialized, initial "
                 "version < 103)");
      grd_session_rdp_notify_error (graphics_pipeline->session_rdp,
                                    ERRINFO_GRAPHICS_SUBSYSTEM_FAILED);

      return CHANNEL_RC_ALREADY_INITIALIZED;
    }

  if (!cap_sets_contains_supported_version (caps_advertise->capsSets,
                                            caps_advertise->capsSetCount))
    {
      g_warning ("[RDP.RDPGFX] CapsAdvertise PDU does NOT contain any supported "
                 "capability sets");
      grd_session_rdp_notify_error (graphics_pipeline->session_rdp,
                                    ERRINFO_GRAPHICS_SUBSYSTEM_FAILED);

      return CHANNEL_RC_UNSUPPORTED_VERSION;
    }

  g_mutex_lock (&graphics_pipeline->caps_mutex);
  g_clear_pointer (&graphics_pipeline->cap_sets, g_free);

  graphics_pipeline->n_cap_sets = caps_advertise->capsSetCount;
  graphics_pipeline->cap_sets = g_memdup2 (caps_advertise->capsSets,
                                           graphics_pipeline->n_cap_sets *
                                           sizeof (RDPGFX_CAPSET));
  g_mutex_unlock (&graphics_pipeline->caps_mutex);

  grd_session_rdp_notify_graphics_pipeline_reset (session_rdp);
  g_source_set_ready_time (graphics_pipeline->protocol_reset_source, 0);

  return CHANNEL_RC_OK;
}

static uint32_t
rdpgfx_cache_import_offer (RdpgfxServerContext                 *rdpgfx_context,
                           const RDPGFX_CACHE_IMPORT_OFFER_PDU *cache_import_offer)
{
  RDPGFX_CACHE_IMPORT_REPLY_PDU cache_import_reply = {0};

  return rdpgfx_context->CacheImportReply (rdpgfx_context, &cache_import_reply);
}

static void
maybe_rewrite_frame_history (GrdRdpGraphicsPipeline *graphics_pipeline,
                             uint32_t                pending_frame_acks)
{
  GfxFrameInfo *gfx_frame_info;

  if (g_queue_get_length (graphics_pipeline->encoded_frames) == 0)
    return;

  reduce_tracked_frame_infos (graphics_pipeline, pending_frame_acks + 1);

  while ((gfx_frame_info = g_queue_pop_tail (graphics_pipeline->encoded_frames)))
    {
      GrdRdpFrameInfo *frame_info = &gfx_frame_info->frame_info;
      uint32_t surface_serial = gfx_frame_info->surface_serial;
      GfxSurfaceContext *surface_context;

      if (!g_hash_table_lookup_extended (graphics_pipeline->serial_surface_table,
                                         GUINT_TO_POINTER (surface_serial),
                                         NULL, (gpointer *) &surface_context))
        g_assert_not_reached ();

      if (surface_context->gfx_surface)
        {
          grd_rdp_gfx_surface_unack_last_acked_frame (surface_context->gfx_surface,
                                                      frame_info->frame_id,
                                                      frame_info->enc_time_us);
        }

      g_free (gfx_frame_info);
    }
}

static void
clear_all_unacked_frames_in_gfx_surface (gpointer key,
                                         gpointer value,
                                         gpointer user_data)
{
  GrdRdpGfxSurface *gfx_surface = value;

  grd_rdp_gfx_surface_clear_all_unacked_frames (gfx_surface);
}

static gboolean
frame_serial_free (gpointer key,
                   gpointer value,
                   gpointer user_data)
{
  GrdRdpGraphicsPipeline *graphics_pipeline = user_data;
  uint32_t surface_serial = GPOINTER_TO_UINT (value);

  surface_serial_unref (graphics_pipeline, surface_serial);

  return TRUE;
}

static void
suspend_frame_acknowledgement (GrdRdpGraphicsPipeline *graphics_pipeline)
{
  graphics_pipeline->frame_acks_suspended = TRUE;

  g_hash_table_foreach (graphics_pipeline->surface_table,
                        clear_all_unacked_frames_in_gfx_surface, NULL);

  reduce_tracked_frame_infos (graphics_pipeline, 0);
  g_hash_table_foreach_remove (graphics_pipeline->frame_serial_table,
                               frame_serial_free, graphics_pipeline);
}

static void
handle_frame_ack_event (GrdRdpGraphicsPipeline             *graphics_pipeline,
                        const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frame_acknowledge)
{
  uint32_t pending_frame_acks;
  gpointer value = NULL;

  pending_frame_acks = graphics_pipeline->total_frames_encoded -
                       frame_acknowledge->totalFramesDecoded;
  if (pending_frame_acks <= MAX_TRACKED_ENC_FRAMES &&
      !g_hash_table_contains (graphics_pipeline->frame_serial_table,
                              GUINT_TO_POINTER (frame_acknowledge->frameId)))
    return;

  maybe_rewrite_frame_history (graphics_pipeline, pending_frame_acks);
  if (frame_acknowledge->queueDepth != SUSPEND_FRAME_ACKNOWLEDGEMENT)
    graphics_pipeline->frame_acks_suspended = FALSE;

  if (g_hash_table_steal_extended (graphics_pipeline->frame_serial_table,
                                   GUINT_TO_POINTER (frame_acknowledge->frameId),
                                   NULL, &value))
    {
      GfxSurfaceContext *surface_context;
      uint32_t surface_serial;

      surface_serial = GPOINTER_TO_UINT (value);
      surface_serial_unref (graphics_pipeline, surface_serial);

      if (!g_hash_table_lookup_extended (graphics_pipeline->serial_surface_table,
                                         GUINT_TO_POINTER (surface_serial),
                                         NULL, (gpointer *) &surface_context))
        g_assert_not_reached ();

      if (surface_context->gfx_surface)
        {
          grd_rdp_gfx_surface_ack_frame (surface_context->gfx_surface,
                                         frame_acknowledge->frameId,
                                         g_get_monotonic_time ());
        }
    }

  if (frame_acknowledge->queueDepth == SUSPEND_FRAME_ACKNOWLEDGEMENT)
    suspend_frame_acknowledgement (graphics_pipeline);
}

static uint32_t
rdpgfx_frame_acknowledge (RdpgfxServerContext                *rdpgfx_context,
                          const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frame_acknowledge)
{
  GrdRdpGraphicsPipeline *graphics_pipeline = rdpgfx_context->custom;

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  handle_frame_ack_event (graphics_pipeline, frame_acknowledge);
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);

  return CHANNEL_RC_OK;
}

static uint32_t
rdpgfx_qoe_frame_acknowledge (RdpgfxServerContext                    *rdpgfx_context,
                              const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU *qoe_frame_acknowledge)
{
  return CHANNEL_RC_OK;
}

void
grd_rdp_graphics_pipeline_maybe_init (GrdRdpGraphicsPipeline *graphics_pipeline)
{
  RdpgfxServerContext *rdpgfx_context;

  if (!graphics_pipeline)
    return;

  if (graphics_pipeline->channel_opened)
    return;

  if (WaitForSingleObject (graphics_pipeline->stop_event, 0) == WAIT_OBJECT_0)
    return;

  rdpgfx_context = graphics_pipeline->rdpgfx_context;
  if (!rdpgfx_context->Open (rdpgfx_context))
    {
      g_warning ("[RDP.RDPGFX] Failed to open Graphics Pipeline. The client "
                 "probably falsely advertised GFX support");
      grd_session_rdp_notify_error (graphics_pipeline->session_rdp,
                                    ERRINFO_GRAPHICS_SUBSYSTEM_FAILED);
      return;
    }

  graphics_pipeline->channel_opened = TRUE;

  return;
}

GrdRdpGraphicsPipeline *
grd_rdp_graphics_pipeline_new (GrdSessionRdp *session_rdp,
                               HANDLE         vcm,
                               HANDLE         stop_event,
                               rdpContext    *rdp_context,
                               wStream       *encode_stream,
                               RFX_CONTEXT   *rfx_context)
{
  GrdRdpGraphicsPipeline *graphics_pipeline;
  RdpgfxServerContext *rdpgfx_context;

  graphics_pipeline = g_object_new (GRD_TYPE_RDP_GRAPHICS_PIPELINE, NULL);
  rdpgfx_context = rdpgfx_server_context_new (vcm);
  if (!rdpgfx_context)
    g_error ("[RDP.RDPGFX] Failed to create server context");

  graphics_pipeline->rdpgfx_context = rdpgfx_context;
  graphics_pipeline->stop_event = stop_event;
  graphics_pipeline->session_rdp = session_rdp;
  graphics_pipeline->encode_stream = encode_stream;
  graphics_pipeline->rfx_context = rfx_context;

  rdpgfx_context->CapsAdvertise = rdpgfx_caps_advertise;
  rdpgfx_context->CacheImportOffer = rdpgfx_cache_import_offer;
  rdpgfx_context->FrameAcknowledge = rdpgfx_frame_acknowledge;
  rdpgfx_context->QoeFrameAcknowledge = rdpgfx_qoe_frame_acknowledge;
  rdpgfx_context->rdpcontext = rdp_context;
  rdpgfx_context->custom = graphics_pipeline;

  return graphics_pipeline;
}

static void
reset_graphics_pipeline (GrdRdpGraphicsPipeline *graphics_pipeline)
{
  GList *surfaces;
  GList *l;

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  surfaces = g_hash_table_get_values (graphics_pipeline->surface_table);
  g_hash_table_steal_all (graphics_pipeline->surface_table);

  reduce_tracked_frame_infos (graphics_pipeline, 0);
  g_hash_table_foreach_remove (graphics_pipeline->frame_serial_table,
                               frame_serial_free, graphics_pipeline);
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);

  for (l = surfaces; l; l = l->next)
    {
      GrdRdpGfxSurface *gfx_surface = l->data;
      GrdRdpSurface *rdp_surface;

      rdp_surface = grd_rdp_gfx_surface_get_rdp_surface (gfx_surface);
      g_clear_object (&rdp_surface->gfx_surface);
    }
  g_list_free (surfaces);

  g_mutex_lock (&graphics_pipeline->gfx_mutex);
  graphics_pipeline->frame_acks_suspended = FALSE;
  graphics_pipeline->total_frames_encoded = 0;

  g_assert (g_hash_table_size (graphics_pipeline->surface_table) == 0);
  g_assert (g_hash_table_size (graphics_pipeline->codec_context_table) == 0);
  g_assert (g_hash_table_size (graphics_pipeline->frame_serial_table) == 0);
  g_assert (g_hash_table_size (graphics_pipeline->serial_surface_table) == 0);
  g_assert (g_queue_get_length (graphics_pipeline->encoded_frames) == 0);
  g_mutex_unlock (&graphics_pipeline->gfx_mutex);
}

static void
grd_rdp_graphics_pipeline_dispose (GObject *object)
{
  GrdRdpGraphicsPipeline *graphics_pipeline = GRD_RDP_GRAPHICS_PIPELINE (object);

  if (graphics_pipeline->channel_opened)
    {
      reset_graphics_pipeline (graphics_pipeline);
      graphics_pipeline->rdpgfx_context->Close (graphics_pipeline->rdpgfx_context);
      graphics_pipeline->channel_opened = FALSE;
    }

  if (graphics_pipeline->protocol_reset_source)
    {
      g_source_destroy (graphics_pipeline->protocol_reset_source);
      g_clear_pointer (&graphics_pipeline->protocol_reset_source, g_source_unref);
    }

  if (graphics_pipeline->encoded_frames)
    {
      g_assert (g_queue_get_length (graphics_pipeline->encoded_frames) == 0);
      g_clear_pointer (&graphics_pipeline->encoded_frames, g_queue_free);
    }

  g_clear_pointer (&graphics_pipeline->cap_sets, g_free);

  g_assert (g_hash_table_size (graphics_pipeline->serial_surface_table) == 0);
  g_clear_pointer (&graphics_pipeline->serial_surface_table, g_hash_table_destroy);
  g_clear_pointer (&graphics_pipeline->frame_serial_table, g_hash_table_destroy);
  g_clear_pointer (&graphics_pipeline->codec_context_table, g_hash_table_destroy);
  g_clear_pointer (&graphics_pipeline->surface_table, g_hash_table_destroy);
  g_clear_pointer (&graphics_pipeline->rdpgfx_context, rdpgfx_server_context_free);

  G_OBJECT_CLASS (grd_rdp_graphics_pipeline_parent_class)->dispose (object);
}

static const char *
rdpgfx_caps_version_to_string (uint32_t caps_version)
{
  switch (caps_version)
    {
    case RDPGFX_CAPVERSION_106:
      return "RDPGFX_CAPVERSION_106";
    case RDPGFX_CAPVERSION_105:
      return "RDPGFX_CAPVERSION_105";
    case RDPGFX_CAPVERSION_104:
      return "RDPGFX_CAPVERSION_104";
    case RDPGFX_CAPVERSION_103:
      return "RDPGFX_CAPVERSION_103";
    case RDPGFX_CAPVERSION_102:
      return "RDPGFX_CAPVERSION_102";
    case RDPGFX_CAPVERSION_101:
      return "RDPGFX_CAPVERSION_101";
    case RDPGFX_CAPVERSION_10:
      return "RDPGFX_CAPVERSION_10";
    case RDPGFX_CAPVERSION_81:
      return "RDPGFX_CAPVERSION_81";
    case RDPGFX_CAPVERSION_8:
      return "RDPGFX_CAPVERSION_8";
    default:
      g_assert_not_reached ();
    }

  return NULL;
}

static gboolean
test_caps_version (GrdRdpGraphicsPipeline *graphics_pipeline,
                   RDPGFX_CAPSET          *cap_sets,
                   uint16_t                n_cap_sets,
                   uint32_t                caps_version)
{
  RdpgfxServerContext *rdpgfx_context = graphics_pipeline->rdpgfx_context;
  rdpSettings *rdp_settings = rdpgfx_context->rdpcontext->settings;
  RDPGFX_CAPS_CONFIRM_PDU caps_confirm = {0};
  uint16_t i;

  for (i = 0; i < n_cap_sets; ++i)
    {
      if (cap_sets[i].version == caps_version)
        {
          uint32_t flags = cap_sets[i].flags;

          switch (caps_version)
            {
            case RDPGFX_CAPVERSION_106:
            case RDPGFX_CAPVERSION_105:
            case RDPGFX_CAPVERSION_104:
            case RDPGFX_CAPVERSION_103:
            case RDPGFX_CAPVERSION_102:
            case RDPGFX_CAPVERSION_101:
            case RDPGFX_CAPVERSION_10:
              rdp_settings->GfxAVC444v2 = rdp_settings->GfxAVC444 =
                rdp_settings->GfxH264 = !(flags & RDPGFX_CAPS_FLAG_AVC_DISABLED);
              break;
            case RDPGFX_CAPVERSION_81:
              rdp_settings->GfxAVC444v2 = rdp_settings->GfxAVC444 = FALSE;
              rdp_settings->GfxH264 = !!(flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED);
              break;
            case RDPGFX_CAPVERSION_8:
              rdp_settings->GfxAVC444v2 = rdp_settings->GfxAVC444 = FALSE;
              rdp_settings->GfxH264 = FALSE;
              break;
            default:
              g_assert_not_reached ();
            }

          g_message ("[RDP.RDPGFX] CapsAdvertise: Accepting capability set with version "
                     "%s, Client cap flags: H264 (AVC444): %s, H264 (AVC420): %s",
                     rdpgfx_caps_version_to_string (caps_version),
                     rdp_settings->GfxAVC444v2 ? "true" : "false",
                     rdp_settings->GfxH264 ? "true" : "false");
          if (!graphics_pipeline->initialized)
            graphics_pipeline->initial_version = caps_version;
          graphics_pipeline->initialized = TRUE;

          reset_graphics_pipeline (graphics_pipeline);

          caps_confirm.capsSet = &cap_sets[i];

          rdpgfx_context->CapsConfirm (rdpgfx_context, &caps_confirm);

          return TRUE;
        }
    }

  return FALSE;
}

static gboolean
reset_protocol (gpointer user_data)
{
  GrdRdpGraphicsPipeline *graphics_pipeline = user_data;
  GrdSessionRdp *session_rdp = graphics_pipeline->session_rdp;
  RDPGFX_CAPSET *cap_sets;
  uint16_t n_cap_sets;
  size_t i;

  g_mutex_lock (&graphics_pipeline->caps_mutex);
  cap_sets = g_steal_pointer (&graphics_pipeline->cap_sets);
  n_cap_sets = graphics_pipeline->n_cap_sets;
  g_mutex_unlock (&graphics_pipeline->caps_mutex);

  if (!cap_sets || !n_cap_sets)
    {
      g_assert (graphics_pipeline->initialized);

      g_free (cap_sets);

      return G_SOURCE_CONTINUE;
    }

  for (i = 0; i < G_N_ELEMENTS (cap_list); ++i)
    {
      if (test_caps_version (graphics_pipeline,
                             cap_sets, n_cap_sets,
                             cap_list[i]))
        {
          grd_session_rdp_notify_graphics_pipeline_ready (session_rdp);
          g_free (cap_sets);

          return G_SOURCE_CONTINUE;
        }
    }
  g_free (cap_sets);

  /*
   * CapsAdvertise already checked the capability sets to have at least one
   * supported version.
   * It is therefore impossible to hit this path.
   */
  g_assert_not_reached ();

  return G_SOURCE_CONTINUE;
}

static gboolean
protocol_reset_source_dispatch (GSource     *source,
                                GSourceFunc  callback,
                                gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs protocol_reset_source_funcs =
{
  .dispatch = protocol_reset_source_dispatch,
};

static void
grd_rdp_graphics_pipeline_init (GrdRdpGraphicsPipeline *graphics_pipeline)
{
  GSource *protocol_reset_source;

  graphics_pipeline->surface_table = g_hash_table_new (NULL, NULL);
  graphics_pipeline->codec_context_table = g_hash_table_new (NULL, NULL);

  graphics_pipeline->frame_serial_table = g_hash_table_new (NULL, NULL);
  graphics_pipeline->serial_surface_table = g_hash_table_new_full (NULL, NULL,
                                                                   NULL, g_free);
  graphics_pipeline->encoded_frames = g_queue_new ();

  g_mutex_init (&graphics_pipeline->gfx_mutex);

  protocol_reset_source = g_source_new (&protocol_reset_source_funcs,
                                        sizeof (GSource));
  g_source_set_callback (protocol_reset_source, reset_protocol,
                         graphics_pipeline, NULL);
  g_source_set_ready_time (protocol_reset_source, -1);
  g_source_attach (protocol_reset_source, NULL);
  graphics_pipeline->protocol_reset_source = protocol_reset_source;
}

static void
grd_rdp_graphics_pipeline_class_init (GrdRdpGraphicsPipelineClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_graphics_pipeline_dispose;
}