/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2015 Red Hat, Inc.

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
#ifndef URS_H_
#define URS_H_

#include "utils.h"

#define URS_MAX_WIDTH       1920
#define URS_MAX_HEIGHT      1080
#define URS_TILE_WIDTH      16
#define URS_TILE_HEIGHT     16
#define URS_TILE_MAX_ITEM   50

#define URS_MAX_TILE_X      ((URS_MAX_WIDTH  + URS_TILE_WIDTH  - 1) / URS_TILE_WIDTH )   
#define URS_MAX_TILE_Y      ((URS_MAX_HEIGHT + URS_TILE_HEIGHT - 1) / URS_TILE_HEIGHT)
#define URS_MAX_TILES       (URS_MAX_TILE_X * URS_MAX_TILE_Y)
#define URS_TILES_MAX_ITEM  (URS_TILE_MAX_ITEM * URS_MAX_TILES)

#define URS_DEFAULT_ITVL_NS 500000000LL
#define SECEND_NS           1000000000LL
typedef struct _URSTileItem{
    RingItem link;
    int active;
    red_time_t update_time;
} URSTileItem;

typedef struct _URSTile{
    Ring history;
    Ring free_buf;
    uint32_t count;
} URSTile;

typedef struct _UdtRgnStat{
    red_time_t histroy_interval;
    uint32_t x_num;
    uint32_t y_num;
    URSTile *tiles;
    URSTileItem *items;
} UdtRgnStat;

#endif /* URS_H_ */
