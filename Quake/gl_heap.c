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



int GetNextStep (int count, int step)
{
	int div = (count + step - 1) / step;
	return div * step;
}



static uint32_t *fan_indices = NULL;
int              fan_indices_count = 0;
#define FANINDEX_ALLOC_STEP 255

int RT_GetFanIndexCount(int vertexcount)
{
	int tricount = q_max(vertexcount - 2, 0);
	return tricount * 3;
}

const uint32_t *RT_GetFanIndices (int vertexcount)
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

		fan_indices_count = GetNextStep (count, FANINDEX_ALLOC_STEP);
		fan_indices = Mem_Alloc (sizeof (uint32_t) * fan_indices_count);

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


static void *scratch_bytes = NULL;
size_t       scratch_bytes_count = 0;
#define SCRATCH_ALLOC_STEP 255

void *RT_AllocScratchMemory (size_t bytecount)
{
	if (bytecount == 0)
	{
		assert (false);
		return NULL;
	}

	if (scratch_bytes_count < bytecount)
	{
		if (scratch_bytes != NULL)
		{
			Mem_Free (scratch_bytes);
			scratch_bytes = NULL;
		}

		scratch_bytes_count = GetNextStep (scratch_bytes_count, SCRATCH_ALLOC_STEP);
		scratch_bytes = Mem_Alloc (scratch_bytes_count);
	}

	return scratch_bytes;
}

void *RT_AllocScratchMemoryNulled (size_t bytecount)
{
	void *dst = RT_AllocScratchMemory (bytecount);
	memset (dst, 0, bytecount);
	return dst;
}
