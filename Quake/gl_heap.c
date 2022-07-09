/*
Copyright (C) 2016 Axel Gneiting

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "gl_heap.h"

#include "glquake.h"
#include "mem.h"

static unsigned *fan_indices = NULL;
int              fan_indices_count = 0;

#define ALLOC_STEP 255

int RT_GetFanIndexCount(int vertexcount)
{
	int tricount = q_max(vertexcount - 2, 0);
	return tricount * 3;
}

const unsigned *RT_GetFanIndices (int vertexcount)
{
	int count = RT_GetFanIndexCount (vertexcount);

	if (count == 0)
	{
		return NULL;
	}

	if (fan_indices_count < count)
	{
		if (fan_indices != NULL)
		{
			Mem_Free (fan_indices);
			fan_indices = NULL;
		}

		fan_indices_count = ((count + ALLOC_STEP - 1) / ALLOC_STEP) * ALLOC_STEP;
		fan_indices = Mem_Alloc (sizeof (unsigned) * fan_indices_count);

		assert (fan_indices_count % 3 == 0);

		for (int i = 0; i < fan_indices_count / 3; i++)
		{
			fan_indices[i + 0] = 0;
			fan_indices[i + 1] = 1 + i;
			fan_indices[i + 2] = 2 + i;
		}
	}

	return fan_indices;
}
