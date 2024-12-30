/*
 * Copyright (C) 2022 Pascal Nowack
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

#include "grd-vk-utils.h"

#include <vulkan/vulkan.h>

typedef struct
{
  VkStructureType type; /* sType */
  const void *next; /* pNext */
} GrdVkBaseStruct;

void
grd_vk_append_to_chain (void *chain,
                        void *element)
{
  GrdVkBaseStruct *tmp;

  tmp = chain;
  while (tmp->next)
    tmp = (GrdVkBaseStruct *) tmp->next;

  tmp->next = element;
}
