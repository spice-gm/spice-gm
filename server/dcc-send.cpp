/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include <config.h>

#include <common/marshaller.h>
#include <common/generated_server_marshallers.h>

#include "dcc-private.h"
#include "display-channel-private.h"
#include "red-qxl.h"

enum FillBitsType {
    FILL_BITS_TYPE_INVALID,
    FILL_BITS_TYPE_CACHE,
    FILL_BITS_TYPE_SURFACE,
    FILL_BITS_TYPE_COMPRESS_LOSSLESS,
    FILL_BITS_TYPE_COMPRESS_LOSSY,
    FILL_BITS_TYPE_BITMAP,
};

enum BitmapDataType {
    BITMAP_DATA_TYPE_INVALID,
    BITMAP_DATA_TYPE_CACHE,
    BITMAP_DATA_TYPE_SURFACE,
    BITMAP_DATA_TYPE_BITMAP,
    BITMAP_DATA_TYPE_BITMAP_TO_CACHE,
};

struct BitmapData {
    BitmapDataType type;
    uint64_t id; // surface id or cache item id
    SpiceRect lossy_rect;
};

static int dcc_pixmap_cache_unlocked_hit(DisplayChannelClient *dcc, uint64_t id, int *lossy)
{
    PixmapCache *cache = dcc->priv->pixmap_cache;
    NewCacheItem *item;
    uint64_t serial;

    serial = dcc->get_message_serial();
    item = cache->hash_table[BITS_CACHE_HASH_KEY(id)];

    while (item) {
        if (item->id == id) {
            ring_remove(&item->lru_link);
            ring_add(&cache->lru, &item->lru_link);
            spice_assert(dcc->priv->id < MAX_CACHE_CLIENTS);
            item->sync[dcc->priv->id] = serial;
            cache->sync[dcc->priv->id] = serial;
            *lossy = item->lossy;
            break;
        }
        item = item->next;
    }

    return !!item;
}

static int dcc_pixmap_cache_hit(DisplayChannelClient *dcc, uint64_t id, int *lossy)
{
    int hit;
    PixmapCache *cache = dcc->priv->pixmap_cache;

    pthread_mutex_lock(&cache->lock);
    hit = dcc_pixmap_cache_unlocked_hit(dcc, id, lossy);
    pthread_mutex_unlock(&cache->lock);
    return hit;
}

/* set area=NULL for testing the whole surface */
static bool is_surface_area_lossy(DisplayChannelClient *dcc, uint32_t surface_id,
                                  const SpiceRect *area, SpiceRect *out_lossy_area)
{
    RedSurface *surface;
    QRegion *surface_lossy_region;
    QRegion lossy_region;
    DisplayChannel *display = DCC_TO_DC(dcc);

    spice_return_val_if_fail(display_channel_validate_surface(display, surface_id), FALSE);

    surface = &display->priv->surfaces[surface_id];
    surface_lossy_region = &dcc->priv->surface_client_lossy_region[surface_id];

    if (!area) {
        if (region_is_empty(surface_lossy_region)) {
            return FALSE;
        }
        out_lossy_area->top = 0;
        out_lossy_area->left = 0;
        out_lossy_area->bottom = surface->context.height;
        out_lossy_area->right = surface->context.width;
        return TRUE;
    }

    region_init(&lossy_region);
    region_add(&lossy_region, area);
    region_and(&lossy_region, surface_lossy_region);
    if (region_is_empty(&lossy_region)) {
        return FALSE;
    }
    out_lossy_area->left = lossy_region.extents.x1;
    out_lossy_area->top = lossy_region.extents.y1;
    out_lossy_area->right = lossy_region.extents.x2;
    out_lossy_area->bottom = lossy_region.extents.y2;
    region_destroy(&lossy_region);
    return TRUE;
}

/* returns if the bitmap was already sent lossy to the client. If the bitmap hasn't been sent yet
   to the client, returns false. "area" is for surfaces. If area = NULL,
   all the surface is considered. out_lossy_data will hold info about the bitmap, and its lossy
   area in case it is lossy and part of a surface. */
static bool is_bitmap_lossy(DisplayChannelClient *dcc, SpiceImage *image, SpiceRect *area,
                            BitmapData *out_data)
{
    if (image == nullptr) {
        // self bitmap
        out_data->type = BITMAP_DATA_TYPE_BITMAP;
        return FALSE;
    }

    if ((image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        int is_hit_lossy;

        out_data->id = image->descriptor.id;
        if (dcc_pixmap_cache_hit(dcc, image->descriptor.id, &is_hit_lossy)) {
            out_data->type = BITMAP_DATA_TYPE_CACHE;
            return is_hit_lossy;
        }
        out_data->type = BITMAP_DATA_TYPE_BITMAP_TO_CACHE;
    } else {
         out_data->type = BITMAP_DATA_TYPE_BITMAP;
    }

    if (image->descriptor.type != SPICE_IMAGE_TYPE_SURFACE) {
        return FALSE;
    }

    out_data->type = BITMAP_DATA_TYPE_SURFACE;
    out_data->id = image->u.surface.surface_id;

    return is_surface_area_lossy(dcc, out_data->id,
                                 area, &out_data->lossy_rect);
}

static bool is_brush_lossy(DisplayChannelClient *dcc, SpiceBrush *brush,
                           BitmapData *out_data)
{
    if (brush->type == SPICE_BRUSH_TYPE_PATTERN) {
        return is_bitmap_lossy(dcc, brush->u.pattern.pat, nullptr,
                               out_data);
    }
    out_data->type = BITMAP_DATA_TYPE_INVALID;
    return FALSE;
}

static RedChannelClient::Pipe::iterator get_pipe_tail(RedChannelClient::Pipe& pipe)
{
    return pipe.empty() ? pipe.end() : --pipe.end();
}

static void red_display_add_image_to_pixmap_cache(DisplayChannelClient *dcc,
                                                  SpiceImage *image, SpiceImage *io_image,
                                                  int is_lossy)
{
    DisplayChannel *display_channel G_GNUC_UNUSED = DCC_TO_DC(dcc);

    if ((image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        spice_assert(image->descriptor.width * image->descriptor.height > 0);
        if (!(io_image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_REPLACE_ME)) {
            if (dcc_pixmap_cache_unlocked_add(dcc, image->descriptor.id,
                                              image->descriptor.width * image->descriptor.height,
                                              is_lossy)) {
                io_image->descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_ME;
                dcc->priv->send_data.pixmap_cache_items[dcc->priv->send_data.num_pixmap_cache_items++] =
                                                                               image->descriptor.id;
                stat_inc_counter(display_channel->priv->add_to_cache_counter, 1);
            }
        }
    }

    if (!(io_image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        stat_inc_counter(display_channel->priv->non_cache_counter, 1);
    }
}

static void marshal_sub_msg_inval_list(SpiceMarshaller *m,
                                       FreeList *free_list)
{
    /* type + size + submessage */
    spice_marshaller_add_uint16(m, SPICE_MSG_DISPLAY_INVAL_LIST);
    spice_marshaller_add_uint32(m, sizeof(*free_list->res) +
                                free_list->res->count * sizeof(free_list->res->resources[0]));
    spice_marshall_msg_display_inval_list(m, free_list->res);
}

static void marshal_sub_msg_inval_list_wait(SpiceMarshaller *m,
                                            FreeList *free_list)
{
    /* type + size + submessage */
    spice_marshaller_add_uint16(m, SPICE_MSG_WAIT_FOR_CHANNELS);
    spice_marshaller_add_uint32(m, sizeof(free_list->wait.header) +
                                free_list->wait.header.wait_count * sizeof(free_list->wait.buf[0]));
    spice_marshall_msg_wait_for_channels(m, &free_list->wait.header);
}

/* use legacy SpiceDataHeader (with sub_list) */
static void send_free_list_legacy(DisplayChannelClient *dcc)
{
    FreeList *free_list = &dcc->priv->send_data.free_list;
    SpiceMarshaller *marshaller;
    int sub_list_len = 1;
    SpiceMarshaller *wait_m = nullptr;
    SpiceMarshaller *inval_m;
    SpiceMarshaller *sub_list_m;

    marshaller = dcc->get_marshaller();
    inval_m = spice_marshaller_get_submarshaller(marshaller);

    marshal_sub_msg_inval_list(inval_m, free_list);

    if (free_list->wait.header.wait_count) {
        wait_m = spice_marshaller_get_submarshaller(marshaller);
        marshal_sub_msg_inval_list_wait(wait_m, free_list);
        sub_list_len++;
    }

    sub_list_m = spice_marshaller_get_submarshaller(marshaller);
    spice_marshaller_add_uint16(sub_list_m, sub_list_len);
    if (wait_m) {
        spice_marshaller_add_uint32(sub_list_m, spice_marshaller_get_offset(wait_m));
    }
    spice_marshaller_add_uint32(sub_list_m, spice_marshaller_get_offset(inval_m));
    dcc->set_header_sub_list(spice_marshaller_get_offset(sub_list_m));
}

/* use mini header and SPICE_MSG_LIST */
static void send_free_list(DisplayChannelClient *dcc)
{
    FreeList *free_list = &dcc->priv->send_data.free_list;
    const int sub_list_len = 2;
    SpiceMarshaller *urgent_marshaller;
    SpiceMarshaller *wait_m;
    SpiceMarshaller *inval_m;
    uint32_t sub_arr_offset;
    uint32_t wait_offset = 0;
    uint32_t inval_offset = 0;
    int i;

    urgent_marshaller = dcc->switch_to_urgent_sender();
    for (i = 0; i < dcc->priv->send_data.num_pixmap_cache_items; i++) {
        int dummy;
        /* When using the urgent marshaller, the serial number of the message that is
         * going to be sent right after the SPICE_MSG_LIST, is increased by one.
         * But all this message pixmaps cache references used its old serial.
         * we use pixmap_cache_items to collect these pixmaps, and we update their serial
         * by calling pixmap_cache_hit. */
        dcc_pixmap_cache_hit(dcc, dcc->priv->send_data.pixmap_cache_items[i], &dummy);
    }

    if (!free_list->wait.header.wait_count) {
        /* only one message, no need for a list */
        dcc->init_send_data(SPICE_MSG_DISPLAY_INVAL_LIST);
        spice_marshall_msg_display_inval_list(urgent_marshaller, free_list->res);
        return;
    }

    dcc->init_send_data(SPICE_MSG_LIST);

    /* append invalidate list */
    inval_m = spice_marshaller_get_submarshaller(urgent_marshaller);
    marshal_sub_msg_inval_list(inval_m, free_list);

    /* append wait list */
    wait_m = spice_marshaller_get_submarshaller(urgent_marshaller);
    marshal_sub_msg_inval_list_wait(wait_m, free_list);

    sub_arr_offset = sub_list_len * sizeof(uint32_t);

    spice_marshaller_add_uint16(urgent_marshaller, sub_list_len);
    inval_offset = spice_marshaller_get_offset(inval_m); // calc the offset before
                                                         // adding the sub list
                                                         // offsets array to the marshaller
    /* adding the array of offsets */
    wait_offset = spice_marshaller_get_offset(wait_m);
    spice_marshaller_add_uint32(urgent_marshaller, wait_offset + sub_arr_offset);
    spice_marshaller_add_uint32(urgent_marshaller, inval_offset + sub_arr_offset);
}

static void fill_base(SpiceMarshaller *base_marshaller, Drawable *drawable)
{
    SpiceMsgDisplayBase base;

    base.surface_id = drawable->surface_id;
    base.box = drawable->red_drawable->bbox;
    base.clip = drawable->red_drawable->clip;

    spice_marshall_DisplayBase(base_marshaller, &base);
}

static void marshaller_compress_buf_free(uint8_t *data, void *opaque)
{
    compress_buf_free((RedCompressBuf*) opaque);
}

static void marshaller_add_compressed(SpiceMarshaller *m,
                                      RedCompressBuf *comp_buf, size_t size)
{
    size_t max = size;
    size_t now;
    do {
        spice_return_if_fail(comp_buf);
        now = MIN(sizeof(comp_buf->buf), max);
        max -= now;
        spice_marshaller_add_by_ref_full(m, comp_buf->buf.bytes, now,
                                         marshaller_compress_buf_free, comp_buf);
        comp_buf = comp_buf->send_next;
    } while (max);
}

static void marshaller_unref_drawable(uint8_t *data, void *opaque)
{
    auto drawable = (Drawable *) opaque;
    drawable_unref(drawable);
}

/* if the number of times fill_bits can be called per one qxl_drawable increases -
   MAX_LZ_DRAWABLE_INSTANCES must be increased as well */
/* NOTE: 'simage' should be owned by the drawable. The drawable will be kept
 * alive until the marshalled message has been sent. See comments below for
 * more information */
static FillBitsType fill_bits(DisplayChannelClient *dcc, SpiceMarshaller *m,
                              SpiceImage *simage, Drawable *drawable, int can_lossy)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    SpiceImage image;
    compress_send_data_t comp_send_data = {nullptr};
    SpiceMarshaller *bitmap_palette_out, *lzplt_palette_out;

    if (simage == nullptr) {
        spice_assert(drawable->red_drawable->self_bitmap_image);
        simage = drawable->red_drawable->self_bitmap_image;
    }

    image.descriptor = simage->descriptor;
    image.descriptor.flags = 0;
    if (simage->descriptor.flags & SPICE_IMAGE_FLAGS_HIGH_BITS_SET) {
        image.descriptor.flags = SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
    }
    pthread_mutex_lock(&dcc->priv->pixmap_cache->lock);

    if ((simage->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        int lossy_cache_item;
        if (dcc_pixmap_cache_unlocked_hit(dcc, image.descriptor.id, &lossy_cache_item)) {
            dcc->priv->send_data.pixmap_cache_items[dcc->priv->send_data.num_pixmap_cache_items++] =
                image.descriptor.id;
            if (can_lossy || !lossy_cache_item) {
                if (!display->priv->enable_jpeg || lossy_cache_item) {
                    image.descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE;
                } else {
                    // making sure, in multiple monitor scenario, that lossy items that
                    // should have been replaced with lossless data by one display channel,
                    // will be retrieved as lossless by another display channel.
                    image.descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE_LOSSLESS;
                }
                spice_marshall_Image(m, &image,
                                     &bitmap_palette_out, &lzplt_palette_out);
                spice_assert(bitmap_palette_out == nullptr);
                spice_assert(lzplt_palette_out == nullptr);
                stat_inc_counter(display->priv->cache_hits_counter, 1);
                pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
                return FILL_BITS_TYPE_CACHE;
            }
            pixmap_cache_unlocked_set_lossy(dcc->priv->pixmap_cache, simage->descriptor.id, FALSE);
            image.descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_REPLACE_ME;
        }
    }

    switch (simage->descriptor.type) {
    case SPICE_IMAGE_TYPE_SURFACE: {
        int surface_id;
        RedSurface *surface;

        surface_id = simage->u.surface.surface_id;
        if (!display_channel_validate_surface(display, surface_id)) {
            spice_warning("Invalid surface in SPICE_IMAGE_TYPE_SURFACE");
            pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
            return FILL_BITS_TYPE_SURFACE;
        }

        surface = &display->priv->surfaces[surface_id];
        image.descriptor.type = SPICE_IMAGE_TYPE_SURFACE;
        image.descriptor.flags = 0;
        image.descriptor.width = surface->context.width;
        image.descriptor.height = surface->context.height;

        image.u.surface.surface_id = surface_id;
        spice_marshall_Image(m, &image,
                             &bitmap_palette_out, &lzplt_palette_out);
        spice_assert(bitmap_palette_out == nullptr);
        spice_assert(lzplt_palette_out == nullptr);
        pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
        return FILL_BITS_TYPE_SURFACE;
    }
    case SPICE_IMAGE_TYPE_BITMAP: {
        SpiceBitmap *bitmap = &image.u.bitmap;
#ifdef DUMP_BITMAP
        dump_bitmap(&simage->u.bitmap);
#endif
        /* Images must be added to the cache only after they are compressed
           in order to prevent starvation in the client between pixmap_cache and
           global dictionary (in cases of multiple monitors) */
        if (red_stream_get_family(dcc->get_stream()) == AF_UNIX ||
            !dcc_compress_image(dcc, &image, &simage->u.bitmap,
                                drawable, can_lossy, &comp_send_data)) {
            SpicePalette *palette;

            red_display_add_image_to_pixmap_cache(dcc, simage, &image, FALSE);

            *bitmap = simage->u.bitmap;
            bitmap->flags = bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN;

            palette = bitmap->palette;
            dcc_palette_cache_palette(dcc, palette, &bitmap->flags);
            spice_marshall_Image(m, &image,
                                 &bitmap_palette_out, &lzplt_palette_out);
            spice_assert(lzplt_palette_out == nullptr);

            if (bitmap_palette_out && palette) {
                spice_marshall_Palette(bitmap_palette_out, palette);
            }

            /* 'drawable' owns this bitmap data, so it must be kept
             * alive until the message is sent. */
            for (unsigned int i = 0; i < bitmap->data->num_chunks; i++) {
                drawable->refs++;
                spice_marshaller_add_by_ref_full(m, bitmap->data->chunk[i].data,
                                                 bitmap->data->chunk[i].len,
                                                 marshaller_unref_drawable, drawable);
            }
            pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
            return FILL_BITS_TYPE_BITMAP;
        }
        red_display_add_image_to_pixmap_cache(dcc, simage, &image, comp_send_data.is_lossy);

        spice_marshall_Image(m, &image, &bitmap_palette_out, &lzplt_palette_out);
        spice_assert(bitmap_palette_out == nullptr);

        marshaller_add_compressed(m, comp_send_data.comp_buf,
                                  comp_send_data.comp_buf_size);

        if (lzplt_palette_out && comp_send_data.lzplt_palette) {
            spice_marshall_Palette(lzplt_palette_out, comp_send_data.lzplt_palette);
        }

        spice_assert(!comp_send_data.is_lossy || can_lossy);
        pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
        return (comp_send_data.is_lossy ? FILL_BITS_TYPE_COMPRESS_LOSSY :
                                          FILL_BITS_TYPE_COMPRESS_LOSSLESS);
    }
    case SPICE_IMAGE_TYPE_QUIC:
        red_display_add_image_to_pixmap_cache(dcc, simage, &image, FALSE);
        image.u.quic = simage->u.quic;
        spice_marshall_Image(m, &image,
                             &bitmap_palette_out, &lzplt_palette_out);
        spice_assert(bitmap_palette_out == nullptr);
        spice_assert(lzplt_palette_out == nullptr);
        /* 'drawable' owns this image data, so it must be kept
         * alive until the message is sent. */
        for (unsigned int i = 0; i < image.u.quic.data->num_chunks; i++) {
            drawable->refs++;
            spice_marshaller_add_by_ref_full(m, image.u.quic.data->chunk[i].data,
                                             image.u.quic.data->chunk[i].len,
                                             marshaller_unref_drawable, drawable);
        }
        pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
        return FILL_BITS_TYPE_COMPRESS_LOSSLESS;
    default:
        spice_error("invalid image type %u", image.descriptor.type);
    }
    pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
    return FILL_BITS_TYPE_INVALID;
}

static void fill_mask(DisplayChannelClient *dcc, SpiceMarshaller *m,
                      SpiceImage *mask_bitmap, Drawable *drawable)
{
    if (mask_bitmap && m) {
        if (dcc->priv->image_compression != SPICE_IMAGE_COMPRESSION_OFF) {
            /* todo: pass compression argument */
            SpiceImageCompression save_img_comp = dcc->priv->image_compression;
            dcc->priv->image_compression = SPICE_IMAGE_COMPRESSION_OFF;
            fill_bits(dcc, m, mask_bitmap, drawable, FALSE);
            dcc->priv->image_compression = save_img_comp;
        } else {
            fill_bits(dcc, m, mask_bitmap, drawable, FALSE);
        }
    }
}

static void fill_attr(SpiceMarshaller *m, SpiceLineAttr *attr)
{
    int i;

    if (m && attr->style_nseg) {
        for (i = 0 ; i < attr->style_nseg; i++) {
            spice_marshaller_add_uint32(m, attr->style[i]);
        }
    }
}

static void marshall_qxl_draw_fill(DisplayChannelClient *dcc,
                                   SpiceMarshaller *base_marshaller,
                                   RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceFill fill;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_FILL);
    fill_base(base_marshaller, item);
    fill = drawable->u.fill;
    spice_marshall_Fill(base_marshaller,
                        &fill,
                        &brush_pat_out,
                        &mask_bitmap_out);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, fill.brush.u.pattern.pat, item, FALSE);
    }

    fill_mask(dcc, mask_bitmap_out, fill.mask.bitmap, item);
}

static void surface_lossy_region_update(DisplayChannelClient *dcc,
                                        Drawable *item, int has_mask, int lossy)
{
    QRegion *surface_lossy_region;
    RedDrawable *drawable;

    if (has_mask && !lossy) {
        return;
    }

    surface_lossy_region = &dcc->priv->surface_client_lossy_region[item->surface_id];
    drawable = item->red_drawable;

    if (drawable->clip.type == SPICE_CLIP_TYPE_RECTS ) {
        QRegion clip_rgn;
        QRegion draw_region;
        region_init(&clip_rgn);
        region_init(&draw_region);
        region_add(&draw_region, &drawable->bbox);
        region_add_clip_rects(&clip_rgn, drawable->clip.rects);
        region_and(&draw_region, &clip_rgn);
        if (lossy) {
            region_or(surface_lossy_region, &draw_region);
        } else {
            region_exclude(surface_lossy_region, &draw_region);
        }

        region_destroy(&clip_rgn);
        region_destroy(&draw_region);
    } else { /* no clip */
        if (!lossy) {
            region_remove(surface_lossy_region, &drawable->bbox);
        } else {
            region_add(surface_lossy_region, &drawable->bbox);
        }
    }
}

static bool drawable_intersects_with_areas(Drawable *drawable, int surface_ids[],
                                           SpiceRect *surface_areas[],
                                           int num_surfaces)
{
    int i;
    for (i = 0; i < num_surfaces; i++) {
        if (surface_ids[i] == drawable->red_drawable->surface_id) {
            if (rect_intersects(surface_areas[i], &drawable->red_drawable->bbox)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static bool pipe_rendered_drawables_intersect_with_areas(DisplayChannelClient *dcc,
                                                         int surface_ids[],
                                                         SpiceRect *surface_areas[],
                                                         int num_surfaces)
{
    spice_assert(num_surfaces);

    for (const auto &pipe_item : dcc->get_pipe()) {
        Drawable *drawable;

        if (pipe_item->type != RED_PIPE_ITEM_TYPE_DRAW)
            continue;
        drawable = static_cast<RedDrawablePipeItem*>(pipe_item.get())->drawable;

        if (ring_item_is_linked(&drawable->list_link))
            continue; // item hasn't been rendered

        if (drawable_intersects_with_areas(drawable, surface_ids, surface_areas, num_surfaces)) {
            return TRUE;
        }
    }

    return FALSE;
}

static bool drawable_depends_on_areas(Drawable *drawable, int surface_ids[],
                                      SpiceRect surface_areas[], int num_surfaces)
{
    int i;
    RedDrawable *red_drawable;
    int drawable_has_shadow;
    SpiceRect shadow_rect = {0, 0, 0, 0};

    red_drawable = drawable->red_drawable;
    drawable_has_shadow = has_shadow(red_drawable);

    if (drawable_has_shadow) {
       int delta_x = red_drawable->u.copy_bits.src_pos.x - red_drawable->bbox.left;
       int delta_y = red_drawable->u.copy_bits.src_pos.y - red_drawable->bbox.top;

       shadow_rect.left = red_drawable->u.copy_bits.src_pos.x;
       shadow_rect.top = red_drawable->u.copy_bits.src_pos.y;
       shadow_rect.right = red_drawable->bbox.right + delta_x;
       shadow_rect.bottom = red_drawable->bbox.bottom + delta_y;
    }

    for (i = 0; i < num_surfaces; i++) {
        int x;
        int dep_surface_id;

         for (x = 0; x < 3; ++x) {
            dep_surface_id = drawable->surface_deps[x];
            if (dep_surface_id == surface_ids[i]) {
                if (rect_intersects(&surface_areas[i], &red_drawable->surfaces_rects[x])) {
                    return TRUE;
                }
            }
        }

        if (surface_ids[i] == red_drawable->surface_id) {
            if (drawable_has_shadow) {
                if (rect_intersects(&surface_areas[i], &shadow_rect)) {
                    return TRUE;
                }
            }

            // not dependent on dest
            if (red_drawable->effect == QXL_EFFECT_OPAQUE) {
                continue;
            }

            if (rect_intersects(&surface_areas[i], &red_drawable->bbox)) {
                return TRUE;
            }
        }

    }
    return FALSE;
}

static void red_pipe_replace_rendered_drawables_with_images(DisplayChannelClient *dcc,
                                                            int first_surface_id,
                                                            SpiceRect *first_area)
{
    int resent_surface_ids[MAX_PIPE_SIZE];
    SpiceRect resent_areas[MAX_PIPE_SIZE]; // not pointers since drawables may be released
    int num_resent;

    resent_surface_ids[0] = first_surface_id;
    resent_areas[0] = *first_area;
    num_resent = 1;

    auto &pipe = dcc->get_pipe();

    // going from the oldest to the newest
    for (auto l = pipe.end(); l != pipe.begin(); ) {
        --l;
        RedPipeItem *pipe_item = l->get();

        if (pipe_item->type != RED_PIPE_ITEM_TYPE_DRAW)
            continue;
        auto dpi = static_cast<RedDrawablePipeItem*>(pipe_item);
        auto drawable = dpi->drawable;
        if (ring_item_is_linked(&drawable->list_link))
            continue; // item hasn't been rendered

        // When a drawable command, X, depends on bitmaps that were resent,
        // these bitmaps state at the client might not be synchronized with X
        // (i.e., the bitmaps can be more futuristic w.r.t X). Thus, X shouldn't
        // be rendered at the client, and we replace it with an image as well.
        if (!drawable_depends_on_areas(drawable,
                                       resent_surface_ids,
                                       resent_areas,
                                       num_resent)) {
            continue;
        }

        dcc_add_surface_area_image(dcc, drawable->red_drawable->surface_id,
                                   &drawable->red_drawable->bbox, l, TRUE);
        resent_surface_ids[num_resent] = drawable->red_drawable->surface_id;
        resent_areas[num_resent] = drawable->red_drawable->bbox;
        num_resent++;

        l = pipe.erase(l);
    }
}

static void red_add_lossless_drawable_dependencies(DisplayChannelClient *dcc,
                                                   Drawable *item,
                                                   int deps_surfaces_ids[],
                                                   SpiceRect *deps_areas[],
                                                   int num_deps)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    RedDrawable *drawable = item->red_drawable;
    int sync_rendered = FALSE;
    int i;

    if (!ring_item_is_linked(&item->list_link)) {
        /* drawable was already rendered, we may not be able to retrieve the lossless data
           for the lossy areas */
        sync_rendered = TRUE;

        // checking if the drawable itself or one of the other commands
        // that were rendered, affected the areas that need to be resent
        if (!drawable_intersects_with_areas(item, deps_surfaces_ids,
                                            deps_areas, num_deps)) {
            if (pipe_rendered_drawables_intersect_with_areas(dcc,
                                                             deps_surfaces_ids,
                                                             deps_areas,
                                                             num_deps)) {
                sync_rendered = TRUE;
            }
        } else {
            sync_rendered = TRUE;
        }
    } else {
        sync_rendered = FALSE;
        for (i = 0; i < num_deps; i++) {
            display_channel_draw_until(display, deps_areas[i], deps_surfaces_ids[i], item);
        }
    }

    if (!sync_rendered) {
        // pushing the pipe item back to the pipe
        dcc_append_drawable(dcc, item);
        // the surfaces areas will be sent as DRAW_COPY commands, that
        // will be executed before the current drawable
        for (i = 0; i < num_deps; i++) {
            dcc_add_surface_area_image(dcc, deps_surfaces_ids[i], deps_areas[i],
                                       get_pipe_tail(dcc->get_pipe()), FALSE);

        }
    } else {
        int drawable_surface_id[1];
        SpiceRect *drawable_bbox[1];

        drawable_surface_id[0] = drawable->surface_id;
        drawable_bbox[0] = &drawable->bbox;

        // check if the other rendered images in the pipe have updated the drawable bbox
        if (pipe_rendered_drawables_intersect_with_areas(dcc,
                                                         drawable_surface_id,
                                                         drawable_bbox,
                                                         1)) {
            red_pipe_replace_rendered_drawables_with_images(dcc,
                                                            drawable->surface_id,
                                                            &drawable->bbox);
        }

        dcc_add_surface_area_image(dcc, drawable->surface_id, &drawable->bbox,
                                   get_pipe_tail(dcc->get_pipe()), TRUE);
    }
}

static void red_lossy_marshall_qxl_draw_fill(DisplayChannelClient *dcc,
                                             SpiceMarshaller *m,
                                             RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;

    int dest_allowed_lossy = FALSE;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    uint16_t rop;

    rop = drawable->u.fill.rop_descriptor;

    dest_allowed_lossy = !((rop & SPICE_ROPD_OP_OR) ||
                           (rop & SPICE_ROPD_OP_AND) ||
                           (rop & SPICE_ROPD_OP_XOR));

    brush_is_lossy = is_brush_lossy(dcc, &drawable->u.fill.brush,
                                    &brush_bitmap_data);
    if (!dest_allowed_lossy) {
        dest_is_lossy = is_surface_area_lossy(dcc, item->surface_id, &drawable->bbox,
                                              &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        !(brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE))) {
        int has_mask = !!drawable->u.fill.mask.bitmap;

        marshall_qxl_draw_fill(dcc, m, dpi);
        // either the brush operation is opaque, or the dest is not lossy
        surface_lossy_region_update(dcc, item, has_mask, FALSE);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(dcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static FillBitsType red_marshall_qxl_draw_opaque(DisplayChannelClient *dcc,
                                                 SpiceMarshaller *base_marshaller,
                                                 RedDrawablePipeItem *dpi,
                                                 int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceOpaque opaque;
    FillBitsType src_send_type;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_OPAQUE);
    fill_base(base_marshaller, item);
    opaque = drawable->u.opaque;
    spice_marshall_Opaque(base_marshaller,
                          &opaque,
                          &src_bitmap_out,
                          &brush_pat_out,
                          &mask_bitmap_out);

    src_send_type = fill_bits(dcc, src_bitmap_out, opaque.src_bitmap, item,
                              src_allowed_lossy);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, opaque.brush.u.pattern.pat, item, FALSE);
    }
    fill_mask(dcc, mask_bitmap_out, opaque.mask.bitmap, item);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_opaque(DisplayChannelClient *dcc,
                                               SpiceMarshaller *m,
                                               RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;

    int src_allowed_lossy;
    int rop;
    int src_is_lossy = FALSE;
    int brush_is_lossy = FALSE;
    BitmapData src_bitmap_data;
    BitmapData brush_bitmap_data;

    rop = drawable->u.opaque.rop_descriptor;
    src_allowed_lossy = !((rop & SPICE_ROPD_OP_OR) ||
                          (rop & SPICE_ROPD_OP_AND) ||
                          (rop & SPICE_ROPD_OP_XOR));

    brush_is_lossy = is_brush_lossy(dcc, &drawable->u.opaque.brush,
                                    &brush_bitmap_data);

    if (!src_allowed_lossy) {
        src_is_lossy = is_bitmap_lossy(dcc, drawable->u.opaque.src_bitmap,
                                       &drawable->u.opaque.src_area,
                                       &src_bitmap_data);
    }

    if (!(brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) &&
        !(src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE))) {
        FillBitsType src_send_type;
        int has_mask = !!drawable->u.opaque.mask.bitmap;

        src_send_type = red_marshall_qxl_draw_opaque(dcc, m, dpi, src_allowed_lossy);
        if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
            src_is_lossy = TRUE;
        } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
            src_is_lossy = FALSE;
        }

        surface_lossy_region_update(dcc, item, has_mask, src_is_lossy);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(dcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static FillBitsType red_marshall_qxl_draw_copy(DisplayChannelClient *dcc,
                                               SpiceMarshaller *base_marshaller,
                                               RedDrawablePipeItem *dpi,
                                               int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceCopy copy;
    FillBitsType src_send_type;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_COPY);
    fill_base(base_marshaller, item);
    copy = drawable->u.copy;
    spice_marshall_Copy(base_marshaller,
                        &copy,
                        &src_bitmap_out,
                        &mask_bitmap_out);

    src_send_type = fill_bits(dcc, src_bitmap_out, copy.src_bitmap, item, src_allowed_lossy);
    fill_mask(dcc, mask_bitmap_out, copy.mask.bitmap, item);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_copy(DisplayChannelClient *dcc,
                                             SpiceMarshaller *base_marshaller,
                                             RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.copy.mask.bitmap;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    FillBitsType src_send_type;

    src_is_lossy = is_bitmap_lossy(dcc, drawable->u.copy.src_bitmap,
                                   &drawable->u.copy.src_area, &src_bitmap_data);

    src_send_type = red_marshall_qxl_draw_copy(dcc, base_marshaller, dpi, TRUE);
    if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
        src_is_lossy = TRUE;
    } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
        src_is_lossy = FALSE;
    }
    surface_lossy_region_update(dcc, item, has_mask,
                                src_is_lossy);
}

static void red_marshall_qxl_draw_transparent(DisplayChannelClient *dcc,
                                              SpiceMarshaller *base_marshaller,
                                              RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceTransparent transparent;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_TRANSPARENT);
    fill_base(base_marshaller, item);
    transparent = drawable->u.transparent;
    spice_marshall_Transparent(base_marshaller,
                               &transparent,
                               &src_bitmap_out);
    fill_bits(dcc, src_bitmap_out, transparent.src_bitmap, item, FALSE);
}

static void red_lossy_marshall_qxl_draw_transparent(DisplayChannelClient *dcc,
                                                    SpiceMarshaller *base_marshaller,
                                                    RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;

    src_is_lossy = is_bitmap_lossy(dcc, drawable->u.transparent.src_bitmap,
                                   &drawable->u.transparent.src_area, &src_bitmap_data);

    if (!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) {
        red_marshall_qxl_draw_transparent(dcc, base_marshaller, dpi);
        // don't update surface lossy region since transperent areas might be lossy
    } else {
        int resend_surface_ids[1];
        SpiceRect *resend_areas[1];

        resend_surface_ids[0] = src_bitmap_data.id;
        resend_areas[0] = &src_bitmap_data.lossy_rect;

        red_add_lossless_drawable_dependencies(dcc, item,
                                               resend_surface_ids, resend_areas, 1);
    }
}

static FillBitsType red_marshall_qxl_draw_alpha_blend(DisplayChannelClient *dcc,
                                                      SpiceMarshaller *base_marshaller,
                                                      RedDrawablePipeItem *dpi,
                                                      int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceAlphaBlend alpha_blend;
    FillBitsType src_send_type;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_ALPHA_BLEND);
    fill_base(base_marshaller, item);
    alpha_blend = drawable->u.alpha_blend;
    spice_marshall_AlphaBlend(base_marshaller,
                              &alpha_blend,
                              &src_bitmap_out);
    src_send_type = fill_bits(dcc, src_bitmap_out, alpha_blend.src_bitmap, item,
                              src_allowed_lossy);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_alpha_blend(DisplayChannelClient *dcc,
                                                    SpiceMarshaller *base_marshaller,
                                                    RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    FillBitsType src_send_type;

    src_is_lossy = is_bitmap_lossy(dcc, drawable->u.alpha_blend.src_bitmap,
                                   &drawable->u.alpha_blend.src_area, &src_bitmap_data);

    src_send_type = red_marshall_qxl_draw_alpha_blend(dcc, base_marshaller, dpi, TRUE);

    if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
        src_is_lossy = TRUE;
    } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
        src_is_lossy = FALSE;
    }

    if (src_is_lossy) {
        surface_lossy_region_update(dcc, item, FALSE, src_is_lossy);
    } // else, the area stays lossy/lossless as the destination
}

static void red_marshall_qxl_copy_bits(RedChannelClient *rcc,
                                       SpiceMarshaller *base_marshaller,
                                       RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpicePoint copy_bits;

    rcc->init_send_data(SPICE_MSG_DISPLAY_COPY_BITS);
    fill_base(base_marshaller, item);
    copy_bits = drawable->u.copy_bits.src_pos;
    spice_marshall_Point(base_marshaller,
                         &copy_bits);
}

static void red_lossy_marshall_qxl_copy_bits(DisplayChannelClient *dcc,
                                             SpiceMarshaller *base_marshaller,
                                             RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceRect src_rect;
    int horz_offset;
    int vert_offset;
    int src_is_lossy;
    SpiceRect src_lossy_area;

    red_marshall_qxl_copy_bits(dcc, base_marshaller, dpi);

    horz_offset = drawable->u.copy_bits.src_pos.x - drawable->bbox.left;
    vert_offset = drawable->u.copy_bits.src_pos.y - drawable->bbox.top;

    src_rect.left = drawable->u.copy_bits.src_pos.x;
    src_rect.top = drawable->u.copy_bits.src_pos.y;
    src_rect.right = drawable->bbox.right + horz_offset;
    src_rect.bottom = drawable->bbox.bottom + vert_offset;

    src_is_lossy = is_surface_area_lossy(dcc, item->surface_id,
                                         &src_rect, &src_lossy_area);

    surface_lossy_region_update(dcc, item, FALSE, src_is_lossy);
}

static void red_marshall_qxl_draw_blend(DisplayChannelClient *dcc,
                                        SpiceMarshaller *base_marshaller,
                                        RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceBlend blend;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_BLEND);
    fill_base(base_marshaller, item);
    blend = drawable->u.blend;
    spice_marshall_Blend(base_marshaller,
                         &blend,
                         &src_bitmap_out,
                         &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, blend.src_bitmap, item, FALSE);

    fill_mask(dcc, mask_bitmap_out, blend.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_blend(DisplayChannelClient *dcc,
                                              SpiceMarshaller *base_marshaller,
                                              RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(dcc, drawable->u.blend.src_bitmap,
                                   &drawable->u.blend.src_area, &src_bitmap_data);
    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if (!dest_is_lossy &&
        (!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))) {
        red_marshall_qxl_draw_blend(dcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(dcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_blackness(DisplayChannelClient *dcc,
                                            SpiceMarshaller *base_marshaller,
                                            RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceBlackness blackness;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_BLACKNESS);
    fill_base(base_marshaller, item);
    blackness = drawable->u.blackness;

    spice_marshall_Blackness(base_marshaller,
                             &blackness,
                             &mask_bitmap_out);

    fill_mask(dcc, mask_bitmap_out, blackness.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_blackness(DisplayChannelClient *dcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.blackness.mask.bitmap;

    red_marshall_qxl_draw_blackness(dcc, base_marshaller, dpi);

    surface_lossy_region_update(dcc, item, has_mask, FALSE);
}

static void red_marshall_qxl_draw_whiteness(DisplayChannelClient *dcc,
                                            SpiceMarshaller *base_marshaller,
                                            RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceWhiteness whiteness;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_WHITENESS);
    fill_base(base_marshaller, item);
    whiteness = drawable->u.whiteness;

    spice_marshall_Whiteness(base_marshaller,
                             &whiteness,
                             &mask_bitmap_out);

    fill_mask(dcc, mask_bitmap_out, whiteness.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_whiteness(DisplayChannelClient *dcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.whiteness.mask.bitmap;

    red_marshall_qxl_draw_whiteness(dcc, base_marshaller, dpi);

    surface_lossy_region_update(dcc, item, has_mask, FALSE);
}

static void red_marshall_qxl_draw_inverse(DisplayChannelClient *dcc,
                                          SpiceMarshaller *base_marshaller,
                                          Drawable *item)
{
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceInvers inverse;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_INVERS);
    fill_base(base_marshaller, item);
    inverse = drawable->u.invers;

    spice_marshall_Invers(base_marshaller,
                          &inverse,
                          &mask_bitmap_out);

    fill_mask(dcc, mask_bitmap_out, inverse.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_inverse(DisplayChannelClient *dcc,
                                                SpiceMarshaller *base_marshaller,
                                                Drawable *item)
{
    red_marshall_qxl_draw_inverse(dcc, base_marshaller, item);
}

static void red_marshall_qxl_draw_rop3(DisplayChannelClient *dcc,
                                       SpiceMarshaller *base_marshaller,
                                       RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceRop3 rop3;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *mask_bitmap_out;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_ROP3);
    fill_base(base_marshaller, item);
    rop3 = drawable->u.rop3;
    spice_marshall_Rop3(base_marshaller,
                        &rop3,
                        &src_bitmap_out,
                        &brush_pat_out,
                        &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, rop3.src_bitmap, item, FALSE);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, rop3.brush.u.pattern.pat, item, FALSE);
    }
    fill_mask(dcc, mask_bitmap_out, rop3.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_rop3(DisplayChannelClient *dcc,
                                             SpiceMarshaller *base_marshaller,
                                             RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(dcc, drawable->u.rop3.src_bitmap,
                                   &drawable->u.rop3.src_area, &src_bitmap_data);
    brush_is_lossy = is_brush_lossy(dcc, &drawable->u.rop3.brush,
                                    &brush_bitmap_data);
    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if ((!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        (!brush_is_lossy || (brush_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        !dest_is_lossy) {
        int has_mask = !!drawable->u.rop3.mask.bitmap;
        red_marshall_qxl_draw_rop3(dcc, base_marshaller, dpi);
        surface_lossy_region_update(dcc, item, has_mask, FALSE);
    } else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(dcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_composite(DisplayChannelClient *dcc,
                                            SpiceMarshaller *base_marshaller,
                                            RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceComposite composite;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_COMPOSITE);
    fill_base(base_marshaller, item);
    composite = drawable->u.composite;
    spice_marshall_Composite(base_marshaller,
                             &composite,
                             &src_bitmap_out,
                             &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, composite.src_bitmap, item, FALSE);
    if (mask_bitmap_out) {
        fill_bits(dcc, mask_bitmap_out, composite.mask_bitmap, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_composite(DisplayChannelClient *dcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int mask_is_lossy;
    BitmapData mask_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(dcc, drawable->u.composite.src_bitmap,
                                   nullptr, &src_bitmap_data);
    mask_is_lossy = drawable->u.composite.mask_bitmap &&
        is_bitmap_lossy(dcc, drawable->u.composite.mask_bitmap, nullptr, &mask_bitmap_data);

    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if ((!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))   &&
        (!mask_is_lossy || (mask_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        !dest_is_lossy) {
        red_marshall_qxl_draw_composite(dcc, base_marshaller, dpi);
        surface_lossy_region_update(dcc, item, FALSE, FALSE);
    }
    else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (mask_is_lossy && (mask_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = mask_bitmap_data.id;
            resend_areas[num_resend] = &mask_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(dcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_stroke(DisplayChannelClient *dcc,
                                         SpiceMarshaller *base_marshaller,
                                         RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceStroke stroke;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *style_out;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_STROKE);
    fill_base(base_marshaller, item);
    stroke = drawable->u.stroke;
    spice_marshall_Stroke(base_marshaller,
                          &stroke,
                          &style_out,
                          &brush_pat_out);

    fill_attr(style_out, &stroke.attr);
    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, stroke.brush.u.pattern.pat, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_stroke(DisplayChannelClient *dcc,
                                               SpiceMarshaller *base_marshaller,
                                               RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int rop;

    brush_is_lossy = is_brush_lossy(dcc, &drawable->u.stroke.brush,
                                    &brush_bitmap_data);

    // back_mode is not used at the client. Ignoring.
    rop = drawable->u.stroke.fore_mode;

    // assuming that if the brush type is solid, the destination can
    // be lossy, no matter what the rop is.
    if (drawable->u.stroke.brush.type != SPICE_BRUSH_TYPE_SOLID &&
        ((rop & SPICE_ROPD_OP_OR) || (rop & SPICE_ROPD_OP_AND) ||
        (rop & SPICE_ROPD_OP_XOR))) {
        dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                              &drawable->bbox, &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        (!brush_is_lossy || (brush_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)))
    {
        red_marshall_qxl_draw_stroke(dcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        // TODO: use the path in order to resend smaller areas
        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = drawable->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(dcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_text(DisplayChannelClient *dcc,
                                       SpiceMarshaller *base_marshaller,
                                       RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceText text;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *back_brush_pat_out;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_TEXT);
    fill_base(base_marshaller, item);
    text = drawable->u.text;
    spice_marshall_Text(base_marshaller,
                        &text,
                        &brush_pat_out,
                        &back_brush_pat_out);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, text.fore_brush.u.pattern.pat, item, FALSE);
    }
    if (back_brush_pat_out) {
        fill_bits(dcc, back_brush_pat_out, text.back_brush.u.pattern.pat, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_text(DisplayChannelClient *dcc,
                                             SpiceMarshaller *base_marshaller,
                                             RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int fg_is_lossy;
    BitmapData fg_bitmap_data;
    int bg_is_lossy;
    BitmapData bg_bitmap_data;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int rop = 0;

    fg_is_lossy = is_brush_lossy(dcc, &drawable->u.text.fore_brush,
                                 &fg_bitmap_data);
    bg_is_lossy = is_brush_lossy(dcc, &drawable->u.text.back_brush,
                                 &bg_bitmap_data);

    // assuming that if the brush type is solid, the destination can
    // be lossy, no matter what the rop is.
    if (drawable->u.text.fore_brush.type != SPICE_BRUSH_TYPE_SOLID) {
        rop = drawable->u.text.fore_mode;
    }

    if (drawable->u.text.back_brush.type != SPICE_BRUSH_TYPE_SOLID) {
        rop |= drawable->u.text.back_mode;
    }

    if ((rop & SPICE_ROPD_OP_OR) || (rop & SPICE_ROPD_OP_AND) ||
        (rop & SPICE_ROPD_OP_XOR)) {
        dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                              &drawable->bbox, &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        (!fg_is_lossy || (fg_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        (!bg_is_lossy || (bg_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))) {
        red_marshall_qxl_draw_text(dcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (fg_is_lossy && (fg_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = fg_bitmap_data.id;
            resend_areas[num_resend] = &fg_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (bg_is_lossy && (bg_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = bg_bitmap_data.id;
            resend_areas[num_resend] = &bg_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = drawable->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }
        red_add_lossless_drawable_dependencies(dcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_release_video_encoder_buffer(uint8_t *data, void *opaque)
{
    auto buffer = (VideoBuffer*)opaque;
    buffer->free(buffer);
}

static bool red_marshall_stream_data(DisplayChannelClient *dcc,
                                     SpiceMarshaller *base_marshaller,
                                     Drawable *drawable)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    VideoStream *stream = drawable->stream;
    SpiceCopy *copy;
    uint32_t frame_mm_time;
    int is_sized;
    VideoEncodeResults ret;

    spice_assert(drawable->red_drawable->type == QXL_DRAW_COPY);

    copy = &drawable->red_drawable->u.copy;
    if (copy->src_bitmap->descriptor.type != SPICE_IMAGE_TYPE_BITMAP) {
        return FALSE;
    }

    is_sized = (copy->src_area.right - copy->src_area.left != stream->width) ||
               (copy->src_area.bottom - copy->src_area.top != stream->height) ||
               !rect_is_equal(&drawable->red_drawable->bbox, &stream->dest_area);

    if (is_sized &&
        !dcc->test_remote_cap(SPICE_DISPLAY_CAP_SIZED_STREAM)) {
        return FALSE;
    }

    int stream_id = display_channel_get_video_stream_id(display, stream);
    VideoStreamAgent *agent = &dcc->priv->stream_agents[stream_id];
    VideoBuffer *outbuf;
    /* workaround for vga streams */
    frame_mm_time =  drawable->red_drawable->mm_time ?
                        drawable->red_drawable->mm_time :
                        reds_get_mm_time();
    ret = !agent->video_encoder ? VIDEO_ENCODER_FRAME_UNSUPPORTED :
          agent->video_encoder->encode_frame(agent->video_encoder,
                                             frame_mm_time,
                                             &copy->src_bitmap->u.bitmap,
                                             &copy->src_area, stream->top_down,
                                             drawable->red_drawable,
                                             &outbuf);
    switch (ret) {
    case VIDEO_ENCODER_FRAME_DROP:
#ifdef STREAM_STATS
        agent->stats.num_drops_fps++;
#endif
        return TRUE;
    case VIDEO_ENCODER_FRAME_UNSUPPORTED:
        return FALSE;
    case VIDEO_ENCODER_FRAME_ENCODE_DONE:
        break;
    default:
        spice_error("bad return value (%d) from VideoEncoder::encode_frame", ret);
        return FALSE;
    }

    if (!is_sized) {
        SpiceMsgDisplayStreamData stream_data;

        dcc->init_send_data(SPICE_MSG_DISPLAY_STREAM_DATA);

        stream_data.base.id = stream_id;
        stream_data.base.multi_media_time = frame_mm_time;
        stream_data.data_size = outbuf->size;

        spice_marshall_msg_display_stream_data(base_marshaller, &stream_data);
    } else {
        SpiceMsgDisplayStreamDataSized stream_data;

        dcc->init_send_data(SPICE_MSG_DISPLAY_STREAM_DATA_SIZED);

        stream_data.base.id = stream_id;
        stream_data.base.multi_media_time = frame_mm_time;
        stream_data.data_size = outbuf->size;
        stream_data.width = copy->src_area.right - copy->src_area.left;
        stream_data.height = copy->src_area.bottom - copy->src_area.top;
        stream_data.dest = drawable->red_drawable->bbox;

        spice_debug("stream %d: sized frame: dest ==> ", stream_data.base.id);
        rect_debug(&stream_data.dest);
        spice_marshall_msg_display_stream_data_sized(base_marshaller, &stream_data);
    }
    spice_marshaller_add_by_ref_full(base_marshaller, outbuf->data, outbuf->size,
                                     &red_release_video_encoder_buffer, outbuf);
#ifdef STREAM_STATS
    agent->stats.num_frames_sent++;
    agent->stats.size_sent += outbuf->size;
    agent->stats.end = frame_mm_time;
#endif

    return TRUE;
}

static inline void marshall_inval_palette(RedChannelClient *rcc,
                                          SpiceMarshaller *base_marshaller,
                                          RedCachePipeItem *cache_item)
{
    rcc->init_send_data(SPICE_MSG_DISPLAY_INVAL_PALETTE);

    spice_marshall_msg_display_inval_palette(base_marshaller, &cache_item->inval_one);

}

static void display_channel_marshall_migrate_data_surfaces(DisplayChannelClient *dcc,
                                                           SpiceMarshaller *m,
                                                           int lossy)
{
    SpiceMarshaller *m2 = spice_marshaller_get_ptr_submarshaller(m);
    uint32_t num_surfaces_created;
    uint8_t *num_surfaces_created_ptr;
    uint32_t i;

    num_surfaces_created_ptr = spice_marshaller_reserve_space(m2, sizeof(uint32_t));
    num_surfaces_created = 0;
    for (i = 0; i < NUM_SURFACES; i++) {
        SpiceRect lossy_rect;

        if (!dcc->priv->surface_client_created[i]) {
            continue;
        }
        spice_marshaller_add_uint32(m2, i);
        num_surfaces_created++;

        if (!lossy) {
            continue;
        }
        region_extents(&dcc->priv->surface_client_lossy_region[i], &lossy_rect);
        spice_marshaller_add_int32(m2, lossy_rect.left);
        spice_marshaller_add_int32(m2, lossy_rect.top);
        spice_marshaller_add_int32(m2, lossy_rect.right);
        spice_marshaller_add_int32(m2, lossy_rect.bottom);
    }
    spice_marshaller_set_uint32(m2, num_surfaces_created_ptr, num_surfaces_created);
}

static void display_channel_marshall_migrate_data(DisplayChannelClient *dcc,
                                                  SpiceMarshaller *base_marshaller)
{
    DisplayChannel *display_channel;
    ImageEncoders *encoders = dcc_get_encoders(dcc);
    SpiceMigrateDataDisplay display_data = {0,};
    GlzEncDictRestoreData glz_dict_data;

    display_channel = DCC_TO_DC(dcc);

    dcc->init_send_data(SPICE_MSG_MIGRATE_DATA);
    spice_marshaller_add_uint32(base_marshaller, SPICE_MIGRATE_DATA_DISPLAY_MAGIC);
    spice_marshaller_add_uint32(base_marshaller, SPICE_MIGRATE_DATA_DISPLAY_VERSION);

    spice_assert(dcc->priv->pixmap_cache);
    SPICE_VERIFY(MIGRATE_DATA_DISPLAY_MAX_CACHE_CLIENTS == 4 &&
                 MIGRATE_DATA_DISPLAY_MAX_CACHE_CLIENTS == MAX_CACHE_CLIENTS);

    display_data.message_serial = dcc->get_message_serial();
    display_data.low_bandwidth_setting = dcc_is_low_bandwidth(dcc);

    display_data.pixmap_cache_freezer = pixmap_cache_freeze(dcc->priv->pixmap_cache);
    display_data.pixmap_cache_id = dcc->priv->pixmap_cache->id;
    display_data.pixmap_cache_size = dcc->priv->pixmap_cache->size;
    memcpy(display_data.pixmap_cache_clients, dcc->priv->pixmap_cache->sync,
           sizeof(display_data.pixmap_cache_clients));

    image_encoders_glz_get_restore_data(encoders, &display_data.glz_dict_id,
                                        &glz_dict_data);
    display_data.glz_dict_data = glz_dict_data;

    /* all data besided the surfaces ref */
    spice_marshaller_add(base_marshaller,
                         (uint8_t *)&display_data, sizeof(display_data) - sizeof(uint32_t));
    display_channel_marshall_migrate_data_surfaces(dcc, base_marshaller,
                                                   display_channel->priv->enable_jpeg);
}

static void display_channel_marshall_pixmap_sync(DisplayChannelClient *dcc,
                                                 SpiceMarshaller *base_marshaller)
{
    SpiceMsgWaitForChannels wait;
    PixmapCache *pixmap_cache;

    dcc->init_send_data(SPICE_MSG_WAIT_FOR_CHANNELS);
    pixmap_cache = dcc->priv->pixmap_cache;

    pthread_mutex_lock(&pixmap_cache->lock);

    wait.wait_count = 1;
    wait.wait_list[0].channel_type = SPICE_CHANNEL_DISPLAY;
    wait.wait_list[0].channel_id = pixmap_cache->generation_initiator.client;
    wait.wait_list[0].message_serial = pixmap_cache->generation_initiator.message;
    dcc->priv->pixmap_cache_generation = pixmap_cache->generation;
    dcc->priv->pending_pixmaps_sync = FALSE;

    pthread_mutex_unlock(&pixmap_cache->lock);

    spice_marshall_msg_wait_for_channels(base_marshaller, &wait);
}

static void dcc_pixmap_cache_reset(DisplayChannelClient *dcc, SpiceMsgWaitForChannels* sync_data)
{
    PixmapCache *cache = dcc->priv->pixmap_cache;
    uint8_t wait_count;
    uint64_t serial;
    uint32_t i;

    serial = dcc->get_message_serial();
    pthread_mutex_lock(&cache->lock);
    pixmap_cache_clear(cache);

    dcc->priv->pixmap_cache_generation = ++cache->generation;
    cache->generation_initiator.client = dcc->priv->id;
    cache->generation_initiator.message = serial;
    cache->sync[dcc->priv->id] = serial;

    wait_count = 0;
    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        if (cache->sync[i] && i != dcc->priv->id) {
            sync_data->wait_list[wait_count].channel_type = SPICE_CHANNEL_DISPLAY;
            sync_data->wait_list[wait_count].channel_id = i;
            sync_data->wait_list[wait_count++].message_serial = cache->sync[i];
        }
    }
    sync_data->wait_count = wait_count;
    pthread_mutex_unlock(&cache->lock);
}

static void display_channel_marshall_reset_cache(DisplayChannelClient *dcc,
                                                 SpiceMarshaller *base_marshaller)
{
    SpiceMsgWaitForChannels wait;

    dcc->init_send_data(SPICE_MSG_DISPLAY_INVAL_ALL_PIXMAPS);
    dcc_pixmap_cache_reset(dcc, &wait);

    spice_marshall_msg_display_inval_all_pixmaps(base_marshaller,
                                                 &wait);
}

static void red_marshall_image(DisplayChannelClient *dcc,
                               SpiceMarshaller *m,
                               RedImageItem *item)
{
    DisplayChannel *display;
    SpiceImage red_image;
    SpiceBitmap bitmap;
    SpiceChunks *chunks;
    QRegion *surface_lossy_region;
    SpiceMsgDisplayDrawCopy copy;
    SpiceMarshaller *src_bitmap_out, *mask_bitmap_out;
    SpiceMarshaller *bitmap_palette_out, *lzplt_palette_out;

    spice_assert(dcc && item);

    display = DCC_TO_DC(dcc);
    spice_assert(display);

    QXL_SET_IMAGE_ID(&red_image, QXL_IMAGE_GROUP_RED, display_channel_generate_uid(display));
    red_image.descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    red_image.descriptor.flags = item->image_flags;
    red_image.descriptor.width = item->width;
    red_image.descriptor.height = item->height;

    bitmap.format = item->image_format;
    bitmap.flags = 0;
    if (item->top_down) {
        bitmap.flags |= SPICE_BITMAP_FLAGS_TOP_DOWN;
    }
    bitmap.x = item->width;
    bitmap.y = item->height;
    bitmap.stride = item->stride;
    bitmap.palette = nullptr;
    bitmap.palette_id = 0;

    chunks = spice_chunks_new_linear(item->data, bitmap.stride * bitmap.y);
    bitmap.data = chunks;

    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_COPY);

    copy.base.surface_id = item->surface_id;
    copy.base.box.left = item->pos.x;
    copy.base.box.top = item->pos.y;
    copy.base.box.right = item->pos.x + bitmap.x;
    copy.base.box.bottom = item->pos.y + bitmap.y;
    copy.base.clip.type = SPICE_CLIP_TYPE_NONE;
    copy.data.rop_descriptor = SPICE_ROPD_OP_PUT;
    copy.data.src_area.left = 0;
    copy.data.src_area.top = 0;
    copy.data.src_area.right = bitmap.x;
    copy.data.src_area.bottom = bitmap.y;
    copy.data.scale_mode = 0;
    copy.data.src_bitmap = nullptr;
    copy.data.mask.flags = 0;
    copy.data.mask.pos.x = 0;
    copy.data.mask.pos.y = 0;
    copy.data.mask.bitmap = nullptr;

    spice_marshall_msg_display_draw_copy(m, &copy,
                                         &src_bitmap_out, &mask_bitmap_out);

    compress_send_data_t comp_send_data = {nullptr};

    int comp_succeeded = dcc_compress_image(dcc, &red_image, &bitmap, nullptr, item->can_lossy, &comp_send_data);

    surface_lossy_region = &dcc->priv->surface_client_lossy_region[item->surface_id];
    if (comp_succeeded) {
        spice_marshall_Image(src_bitmap_out, &red_image,
                             &bitmap_palette_out, &lzplt_palette_out);

        marshaller_add_compressed(src_bitmap_out,
                                  comp_send_data.comp_buf, comp_send_data.comp_buf_size);

        if (lzplt_palette_out && comp_send_data.lzplt_palette) {
            spice_marshall_Palette(lzplt_palette_out, comp_send_data.lzplt_palette);
        }

        if (spice_image_descriptor_is_lossy(&red_image.descriptor)) {
            region_add(surface_lossy_region, &copy.base.box);
        } else {
            region_remove(surface_lossy_region, &copy.base.box);
        }
    } else {
        red_image.descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
        red_image.u.bitmap = bitmap;

        spice_marshall_Image(src_bitmap_out, &red_image,
                             &bitmap_palette_out, &lzplt_palette_out);
        item->add_to_marshaller(src_bitmap_out, item->data,
                                bitmap.y * bitmap.stride);
        region_remove(surface_lossy_region, &copy.base.box);
    }
    spice_chunks_destroy(chunks);
}

static void marshall_lossy_qxl_drawable(DisplayChannelClient *dcc,
                                        SpiceMarshaller *base_marshaller,
                                        RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    switch (item->red_drawable->type) {
    case QXL_DRAW_FILL:
        red_lossy_marshall_qxl_draw_fill(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_OPAQUE:
        red_lossy_marshall_qxl_draw_opaque(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_COPY:
        red_lossy_marshall_qxl_draw_copy(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_lossy_marshall_qxl_draw_transparent(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        red_lossy_marshall_qxl_draw_alpha_blend(dcc, base_marshaller, dpi);
        break;
    case QXL_COPY_BITS:
        red_lossy_marshall_qxl_copy_bits(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_BLEND:
        red_lossy_marshall_qxl_draw_blend(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_BLACKNESS:
        red_lossy_marshall_qxl_draw_blackness(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_WHITENESS:
        red_lossy_marshall_qxl_draw_whiteness(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_INVERS:
        red_lossy_marshall_qxl_draw_inverse(dcc, base_marshaller, item);
        break;
    case QXL_DRAW_ROP3:
        red_lossy_marshall_qxl_draw_rop3(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_COMPOSITE:
        red_lossy_marshall_qxl_draw_composite(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_STROKE:
        red_lossy_marshall_qxl_draw_stroke(dcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_TEXT:
        red_lossy_marshall_qxl_draw_text(dcc, base_marshaller, dpi);
        break;
    default:
        spice_warn_if_reached();
    }
}

static void marshall_lossless_qxl_drawable(DisplayChannelClient *dcc,
                                           SpiceMarshaller *m,
                                           RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;

    switch (drawable->type) {
    case QXL_DRAW_FILL:
        marshall_qxl_draw_fill(dcc, m, dpi);
        break;
    case QXL_DRAW_OPAQUE:
        red_marshall_qxl_draw_opaque(dcc, m, dpi, FALSE);
        break;
    case QXL_DRAW_COPY:
        red_marshall_qxl_draw_copy(dcc, m, dpi, FALSE);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_marshall_qxl_draw_transparent(dcc, m, dpi);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        red_marshall_qxl_draw_alpha_blend(dcc, m, dpi, FALSE);
        break;
    case QXL_COPY_BITS:
        red_marshall_qxl_copy_bits(dcc, m, dpi);
        break;
    case QXL_DRAW_BLEND:
        red_marshall_qxl_draw_blend(dcc, m, dpi);
        break;
    case QXL_DRAW_BLACKNESS:
        red_marshall_qxl_draw_blackness(dcc, m, dpi);
        break;
    case QXL_DRAW_WHITENESS:
        red_marshall_qxl_draw_whiteness(dcc, m, dpi);
        break;
    case QXL_DRAW_INVERS:
        red_marshall_qxl_draw_inverse(dcc, m, item);
        break;
    case QXL_DRAW_ROP3:
        red_marshall_qxl_draw_rop3(dcc, m, dpi);
        break;
    case QXL_DRAW_STROKE:
        red_marshall_qxl_draw_stroke(dcc, m, dpi);
        break;
    case QXL_DRAW_COMPOSITE:
        red_marshall_qxl_draw_composite(dcc, m, dpi);
        break;
    case QXL_DRAW_TEXT:
        red_marshall_qxl_draw_text(dcc, m, dpi);
        break;
    default:
        spice_warn_if_reached();
    }
}

static void marshall_qxl_drawable(DisplayChannelClient *dcc,
                                  SpiceMarshaller *m,
                                  RedDrawablePipeItem *dpi)
{
    spice_return_if_fail(dcc);

    Drawable *item = dpi->drawable;
    DisplayChannel *display = DCC_TO_DC(dcc);

    spice_return_if_fail(display);
    /* allow sized frames to be streamed, even if they where replaced by another frame, since
     * newer frames might not cover sized frames completely if they are bigger */
    if (item->stream && red_marshall_stream_data(dcc, m, item)) {
        return;
    }
    if (display->priv->enable_jpeg)
        marshall_lossy_qxl_drawable(dcc, m, dpi);
    else
        marshall_lossless_qxl_drawable(dcc, m, dpi);
}

static void marshall_stream_start(DisplayChannelClient *dcc,
                                  SpiceMarshaller *base_marshaller,
                                  VideoStreamAgent *agent)
{
    VideoStream *stream = agent->stream;

    spice_assert(stream);
    if (!agent->video_encoder) {
        /* Without a video encoder nothing will be streamed */
        return;
    }
    dcc->init_send_data(SPICE_MSG_DISPLAY_STREAM_CREATE);
    SpiceMsgDisplayStreamCreate stream_create;
    SpiceClipRects clip_rects;

    stream_create.surface_id = 0;
    stream_create.id = display_channel_get_video_stream_id(DCC_TO_DC(dcc), stream);
    stream_create.flags = stream->top_down ? SPICE_STREAM_FLAGS_TOP_DOWN : 0;
    stream_create.codec_type = agent->video_encoder->codec_type;

    stream_create.src_width = stream->width;
    stream_create.src_height = stream->height;
    stream_create.stream_width = stream_create.src_width;
    stream_create.stream_height = stream_create.src_height;
    stream_create.dest = stream->dest_area;

    if (stream->current) {
        RedDrawable *red_drawable = stream->current->red_drawable;
        stream_create.clip = red_drawable->clip;
    } else {
        stream_create.clip.type = SPICE_CLIP_TYPE_RECTS;
        clip_rects.num_rects = 0;
        stream_create.clip.rects = &clip_rects;
    }

    stream_create.stamp = 0;

    spice_marshall_msg_display_stream_create(base_marshaller, &stream_create);
}

static void marshall_stream_clip(DisplayChannelClient *dcc,
                                 SpiceMarshaller *base_marshaller,
                                 VideoStreamClipItem *item)
{
    VideoStreamAgent *agent = item->stream_agent;

    spice_return_if_fail(agent->stream);

    dcc->init_send_data(SPICE_MSG_DISPLAY_STREAM_CLIP);
    SpiceMsgDisplayStreamClip stream_clip;

    stream_clip.id = display_channel_get_video_stream_id(DCC_TO_DC(dcc), agent->stream);
    stream_clip.clip.type = item->clip_type;
    stream_clip.clip.rects = item->rects.get();

    spice_marshall_msg_display_stream_clip(base_marshaller, &stream_clip);
}

static void marshall_stream_end(DisplayChannelClient *dcc,
                                SpiceMarshaller *base_marshaller,
                                VideoStreamAgent* agent)
{
    SpiceMsgDisplayStreamDestroy destroy;

    dcc->init_send_data(SPICE_MSG_DISPLAY_STREAM_DESTROY);
    destroy.id = display_channel_get_video_stream_id(DCC_TO_DC(dcc), agent->stream);
    video_stream_agent_stop(agent);
    spice_marshall_msg_display_stream_destroy(base_marshaller, &destroy);
}

static void marshall_upgrade(DisplayChannelClient *dcc, SpiceMarshaller *m,
                             RedUpgradeItem *item)
{
    RedChannel *channel = dcc->get_channel();
    RedDrawable *red_drawable;
    SpiceMsgDisplayDrawCopy copy;
    SpiceMarshaller *src_bitmap_out, *mask_bitmap_out;

    spice_assert(channel && item && item->drawable);
    dcc->init_send_data(SPICE_MSG_DISPLAY_DRAW_COPY);

    red_drawable = item->drawable->red_drawable;
    spice_assert(red_drawable->type == QXL_DRAW_COPY);
    spice_assert(red_drawable->u.copy.rop_descriptor == SPICE_ROPD_OP_PUT);
    spice_assert(red_drawable->u.copy.mask.bitmap == nullptr);

    copy.base.surface_id = 0;
    copy.base.box = red_drawable->bbox;
    copy.base.clip.type = SPICE_CLIP_TYPE_RECTS;
    copy.base.clip.rects = item->rects.get();
    copy.data = red_drawable->u.copy;

    spice_marshall_msg_display_draw_copy(m, &copy,
                                         &src_bitmap_out, &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, copy.data.src_bitmap, item->drawable, FALSE);
}

static void marshall_surface_create(DisplayChannelClient *dcc,
                                    SpiceMarshaller *base_marshaller,
                                    SpiceMsgSurfaceCreate *surface_create)
{
    region_init(&dcc->priv->surface_client_lossy_region[surface_create->surface_id]);
    dcc->init_send_data(SPICE_MSG_DISPLAY_SURFACE_CREATE);

    spice_marshall_msg_display_surface_create(base_marshaller, surface_create);
}

static void marshall_surface_destroy(DisplayChannelClient *dcc,
                                     SpiceMarshaller *base_marshaller, uint32_t surface_id)
{
    SpiceMsgSurfaceDestroy surface_destroy;

    region_destroy(&dcc->priv->surface_client_lossy_region[surface_id]);
    dcc->init_send_data(SPICE_MSG_DISPLAY_SURFACE_DESTROY);

    surface_destroy.surface_id = surface_id;

    spice_marshall_msg_display_surface_destroy(base_marshaller, &surface_destroy);
}

static void marshall_monitors_config(RedChannelClient *rcc, SpiceMarshaller *base_marshaller,
                                     MonitorsConfig *monitors_config)
{
    int heads_size = sizeof(SpiceHead) * monitors_config->count;
    int i;
    auto msg = (SpiceMsgDisplayMonitorsConfig *) g_malloc0(sizeof(SpiceMsgDisplayMonitorsConfig) + heads_size);
    int count = 0; // ignore monitors_config->count, it may contain zero width monitors, remove them now

    rcc->init_send_data(SPICE_MSG_DISPLAY_MONITORS_CONFIG);
    for (i = 0 ; i < monitors_config->count; ++i) {
        if (monitors_config->heads[i].width == 0 || monitors_config->heads[i].height == 0) {
            continue;
        }
        msg->heads[count].monitor_id = monitors_config->heads[i].id;
        msg->heads[count].surface_id = monitors_config->heads[i].surface_id;
        msg->heads[count].width = monitors_config->heads[i].width;
        msg->heads[count].height = monitors_config->heads[i].height;
        msg->heads[count].x = monitors_config->heads[i].x;
        msg->heads[count].y = monitors_config->heads[i].y;
        count++;
    }
    msg->count = count;
    msg->max_allowed = monitors_config->max_allowed;
    spice_marshall_msg_display_monitors_config(base_marshaller, msg);
    g_free(msg);
}

static void marshall_stream_activate_report(RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            RedStreamActivateReportItem *report_item)
{
    SpiceMsgDisplayStreamActivateReport msg;

    rcc->init_send_data(SPICE_MSG_DISPLAY_STREAM_ACTIVATE_REPORT);
    msg.stream_id = report_item->stream_id;
    msg.unique_id = report_item->report_id;
    msg.max_window_size = RED_STREAM_CLIENT_REPORT_WINDOW;
    msg.timeout_ms = RED_STREAM_CLIENT_REPORT_TIMEOUT;
    spice_marshall_msg_display_stream_activate_report(base_marshaller, &msg);
}

static void marshall_gl_scanout(DisplayChannelClient *dcc,
                                SpiceMarshaller *m,
                                RedPipeItem *item)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    QXLInstance* qxl = display_channel->priv->qxl;

    SpiceMsgDisplayGlScanoutUnix *scanout = red_qxl_get_gl_scanout(qxl);
    if (scanout != nullptr) {
        dcc->init_send_data(SPICE_MSG_DISPLAY_GL_SCANOUT_UNIX);
        spice_marshall_msg_display_gl_scanout_unix(m, scanout);
    }
    red_qxl_put_gl_scanout(qxl, scanout);
}

static void marshall_gl_draw(RedChannelClient *rcc,
                             SpiceMarshaller *m,
                             RedPipeItem *item)
{
    auto p = static_cast<RedGlDrawItem*>(item);

    rcc->init_send_data(SPICE_MSG_DISPLAY_GL_DRAW);
    spice_marshall_msg_display_gl_draw(m, &p->draw);
}


static void begin_send_message(DisplayChannelClient *dcc)
{
    FreeList *free_list = &dcc->priv->send_data.free_list;

    if (free_list->res->count) {
        int sync_count = 0;
        int i;

        for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
            if (i != dcc->priv->id && free_list->sync[i] != 0) {
                free_list->wait.header.wait_list[sync_count].channel_type = SPICE_CHANNEL_DISPLAY;
                free_list->wait.header.wait_list[sync_count].channel_id = i;
                free_list->wait.header.wait_list[sync_count++].message_serial = free_list->sync[i];
            }
        }
        free_list->wait.header.wait_count = sync_count;

        if (dcc->is_mini_header()) {
            send_free_list(dcc);
        } else {
            send_free_list_legacy(dcc);
        }
    }
    dcc->begin_send_message();
}

static void reset_send_data(DisplayChannelClient *dcc)
{
    dcc->priv->send_data.free_list.res->count = 0;
    dcc->priv->send_data.num_pixmap_cache_items = 0;
    memset(dcc->priv->send_data.free_list.sync, 0,
           sizeof(dcc->priv->send_data.free_list.sync));
}

void DisplayChannelClient::send_item(RedPipeItem *pipe_item)
{
    DisplayChannelClient *dcc = this;
    SpiceMarshaller *m = get_marshaller();

    ::reset_send_data(dcc);
    switch (pipe_item->type) {
    case RED_PIPE_ITEM_TYPE_DRAW: {
        auto dpi = static_cast<RedDrawablePipeItem*>(pipe_item);
        marshall_qxl_drawable(this, m, dpi);
        break;
    }
    case RED_PIPE_ITEM_TYPE_INVAL_ONE:
        marshall_inval_palette(this, m, static_cast<RedCachePipeItem*>(pipe_item));
        break;
    case RED_PIPE_ITEM_TYPE_STREAM_CREATE: {
        auto item = static_cast<StreamCreateDestroyItem*>(pipe_item);
        marshall_stream_start(this, m, item->agent);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_CLIP:
        marshall_stream_clip(this, m, static_cast<VideoStreamClipItem*>(pipe_item));
        break;
    case RED_PIPE_ITEM_TYPE_STREAM_DESTROY: {
        auto item = static_cast<StreamCreateDestroyItem*>(pipe_item);
        marshall_stream_end(this, m, item->agent);
        break;
    }
    case RED_PIPE_ITEM_TYPE_UPGRADE:
        marshall_upgrade(this, m, static_cast<RedUpgradeItem*>(pipe_item));
        break;
    case RED_PIPE_ITEM_TYPE_MIGRATE_DATA:
        display_channel_marshall_migrate_data(this, m);
        break;
    case RED_PIPE_ITEM_TYPE_IMAGE:
        red_marshall_image(this, m, static_cast<RedImageItem*>(pipe_item));
        break;
    case RED_PIPE_ITEM_TYPE_PIXMAP_SYNC:
        display_channel_marshall_pixmap_sync(this, m);
        break;
    case RED_PIPE_ITEM_TYPE_PIXMAP_RESET:
        display_channel_marshall_reset_cache(this, m);
        break;
    case RED_PIPE_ITEM_TYPE_INVAL_PALETTE_CACHE:
        dcc_palette_cache_reset(dcc);
        init_send_data(SPICE_MSG_DISPLAY_INVAL_ALL_PALETTES);
        break;
    case RED_PIPE_ITEM_TYPE_CREATE_SURFACE: {
        auto surface_create = static_cast<RedSurfaceCreateItem*>(pipe_item);
        marshall_surface_create(this, m, &surface_create->surface_create);
        break;
    }
    case RED_PIPE_ITEM_TYPE_DESTROY_SURFACE: {
        auto surface_destroy = static_cast<RedSurfaceDestroyItem*>(pipe_item);
        marshall_surface_destroy(this, m, surface_destroy->surface_destroy.surface_id);
        break;
    }
    case RED_PIPE_ITEM_TYPE_MONITORS_CONFIG: {
        auto monconf_item = static_cast<RedMonitorsConfigItem*>(pipe_item);
        marshall_monitors_config(this, m, monconf_item->monitors_config);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT: {
        auto report_item =
            static_cast<RedStreamActivateReportItem*>(pipe_item);
        marshall_stream_activate_report(this, m, report_item);
        break;
    }
    case RED_PIPE_ITEM_TYPE_GL_SCANOUT:
        marshall_gl_scanout(this, m, pipe_item);
        break;
    case RED_PIPE_ITEM_TYPE_GL_DRAW:
        marshall_gl_draw(this, m, pipe_item);
        break;
    default:
        spice_warn_if_reached();
    }

    // a message is pending
    if (send_message_pending()) {
        ::begin_send_message(this);
    }
}
