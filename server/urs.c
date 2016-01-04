/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2010 Red Hat, Inc.

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

#include "urs.h"

void urs_init (DisplayChannel *display_channel)
{
    UdtRgnStat *urs = &display_channel->urs;

    memset(urs, 0, sizeof(UdtRgnStat));
    urs->tiles = (URSTile *)spice_malloc0(URS_MAX_TILES * sizeof(URSTile));
    urs->items = (URSTileItem *)spice_malloc0(URS_TILES_MAX_ITEM * sizeof(URSTileItem));
}

void urs_reset(DisplayChannel *display_channel, uint32_t width, uint32_t height)
{
    UdtRgnStat *urs = &display_channel->urs;
    uint32_t i, tiles_num;
    URSTile *tile;
    
    memset(urs, 0, sizeof(UdtRgnStat));
    urs->histroy_interval = URS_DEFAULT_ITVL_NS;
    urs->x_num = (width + URS_TILE_WIDTH - 1) / URS_TILE_WIDTH;
    urs->y_num = (height + URS_TILE_HEIGHT - 1) / URS_TILE_HEIGHT;
    tile = urs->tiles;
    tiles_num = urs->x_num * urs->y_num;
    if (tiles_num > URS_MAX_TILES) {
            
    }
    
    memset(tile, 0, tiles_num * sizeof(URSTile));
    memset(urs->items, 0, URS_TILES_MAX_ITEM * sizeof(URSTileItem));
    for (i = 0; i < tiles_num; i++) {
        ring_init(&tile->queue);
        tile++;
    }

    return;
}

void urs_uninit(DisplayChannel *display_channel)
{
    free(display_channel->urs->tiles);
    free(display_channel->urs->items);
}

static URSTileItem *find_no_active_item(URSTileItem *item_start)
{
    int i;
    URSTileItem *item = item_start;
    
    for (i = 0; i < URS_TILE_MAX_ITEM; i++) {
        if (item->active == 0)
            return item;
        item++;
    }
    return NULL;
}

void tile_update_time(URSTileItem *item_start, URSTile *tile, red_time_t time, red_time_t histroy_interval)
{
    Ring *ring = &tile->queue;
    RingItem *now, *next;
    URSTileItem *cur;
    
    RING_FOREACH_SAFE(now, next, ring) {
        URSTileItem *item = SPICE_CONTAINEROF(now, URSTileItem, link);
        if (time - item->update_time < histroy_interval) {
            break;
        }
        item->active = 0;
        ring_remove(now);        
        tile->count--;
    }
    
    if (tile->count >= URS_TILE_MAX_ITEM) {
        cur = ring_get_head(ring);
        ring_remove(cur);
        tile->count--;
    } else {
        cur = find_no_active_item(item_start);
        spice_assert(cur != NULL);
        ring_item_init(&cur->link);
    }    
    ring_add(ring, &cur->link);
    cur->active = 1; 
    cur->update_time = time;
    tile->count++;
}

void urs_add_drawable(UdtRgnStat *urs, Drawable *drawable)
{
    RedDrawable *red_drawable = drawable->red_drawable;
    SpiceRect *rect = &red_drawable->bbox;
    URSTile *tile;
    int32_t left_index, right_index, top_index, bottom_index;
    int i, j, k;
    
    if (!urs)
        return;
    left_index      = rect->left    / URS_TILE_WIDTH;
    right_index     = rect->right   / URS_TILE_WIDTH;
    top_index       = rect->top     / URS_TILE_HEIGHT;
    bottom_index    = rect->bottom  / URS_TILE_HEIGHT;
    for (j = top_index; j <= bottom_index; j++) {
        k = j * urs->x_num;
        for (i = left_index; i <= right_index; i++) {
            tile = &urs->tiles[k + i];
            tile_update_time(&urs->items[(k + i) * URS_TILE_MAX_ITEM], tile, drawable->creation_time, urs->histroy_interval);
        }
    }
}

int urs_if_rect_over_fps(DisplayChannel *display_channel, int surface_id, 
                         SpiceRect *rect, int fps, double percent)
{
    URSTile *tile;
    int32_t left_index, right_index, top_index, bottom_index;
    UdtRgnStat *urs = display_channel->urs;  
    int32_t frame, target;
    int32_t total = 0;
    int i, j, k;
    
    frame = 1.0 * fps * (urs->histroy_interval / SECEND_NS);
        
    left_index      = rect->left    / URS_TILE_WIDTH;
    right_index     = rect->right   / URS_TILE_WIDTH;
    top_index       = rect->top     / URS_TILE_HEIGHT;
    bottom_index    = rect->bottom  / URS_TILE_HEIGHT;

    
    for (j = top_index; j <= bottom_index; j++) {
        k = j * urs->x_num;
        for (i = left_index; i <= right_index; i++) {
            tile = &urs->tiles[k + i];
            if (tile->count >= frame) {
                target++;
            }
        }
    }
    total = (right_index - left_index + 1) * (bottom_index - top_index + 1);
    
    if ((1.0 * target / total) >= percent) {
        return 1;
    } else {
        return 0;
    }    
}

