/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers
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
// gl_mesh.c: triangle model functions

#include "quakedef.h"
#include "gl_heap.h"

/*
=================================================================

ALIAS MODEL DISPLAY LIST GENERATION

=================================================================
*/

qmodel_t   *aliasmodel;
aliashdr_t *paliashdr;

int used[8192]; // qboolean

// the command list holds counts and s/t values that are valid for
// every frame
int commands[8192];
int numcommands;

// all frames will have their vertexes rearranged and expanded
// so they are in the order expected by the command list
int vertexorder[8192];
int numorder;

int allverts, alltris;

int stripverts[128];
int striptris[128];
int stripcount;

/*
================
StripLength
================
*/
int StripLength (int starttri, int startv)
{
	int          m1, m2;
	int          j;
	mtriangle_t *last, *check;
	int          k;

	used[starttri] = 2;

	last = &triangles[starttri];

	stripverts[0] = last->vertindex[(startv) % 3];
	stripverts[1] = last->vertindex[(startv + 1) % 3];
	stripverts[2] = last->vertindex[(startv + 2) % 3];

	striptris[0] = starttri;
	stripcount = 1;

	m1 = last->vertindex[(startv + 2) % 3];
	m2 = last->vertindex[(startv + 1) % 3];

	// look for a matching triangle
nexttri:
	for (j = starttri + 1, check = &triangles[starttri + 1]; j < pheader->numtris; j++, check++)
	{
		if (check->facesfront != last->facesfront)
			continue;
		for (k = 0; k < 3; k++)
		{
			if (check->vertindex[k] != m1)
				continue;
			if (check->vertindex[(k + 1) % 3] != m2)
				continue;

			// this is the next part of the fan

			// if we can't use this triangle, this tristrip is done
			if (used[j])
				goto done;

			// the new edge
			if (stripcount & 1)
				m2 = check->vertindex[(k + 2) % 3];
			else
				m1 = check->vertindex[(k + 2) % 3];

			stripverts[stripcount + 2] = check->vertindex[(k + 2) % 3];
			striptris[stripcount] = j;
			stripcount++;

			used[j] = 2;
			goto nexttri;
		}
	}
done:

	// clear the temp used flags
	for (j = starttri + 1; j < pheader->numtris; j++)
		if (used[j] == 2)
			used[j] = 0;

	return stripcount;
}

/*
===========
FanLength
===========
*/
int FanLength (int starttri, int startv)
{
	int          m1, m2;
	int          j;
	mtriangle_t *last, *check;
	int          k;

	used[starttri] = 2;

	last = &triangles[starttri];

	stripverts[0] = last->vertindex[(startv) % 3];
	stripverts[1] = last->vertindex[(startv + 1) % 3];
	stripverts[2] = last->vertindex[(startv + 2) % 3];

	striptris[0] = starttri;
	stripcount = 1;

	m1 = last->vertindex[(startv + 0) % 3];
	m2 = last->vertindex[(startv + 2) % 3];

	// look for a matching triangle
nexttri:
	for (j = starttri + 1, check = &triangles[starttri + 1]; j < pheader->numtris; j++, check++)
	{
		if (check->facesfront != last->facesfront)
			continue;
		for (k = 0; k < 3; k++)
		{
			if (check->vertindex[k] != m1)
				continue;
			if (check->vertindex[(k + 1) % 3] != m2)
				continue;

			// this is the next part of the fan

			// if we can't use this triangle, this tristrip is done
			if (used[j])
				goto done;

			// the new edge
			m2 = check->vertindex[(k + 2) % 3];

			stripverts[stripcount + 2] = m2;
			striptris[stripcount] = j;
			stripcount++;

			used[j] = 2;
			goto nexttri;
		}
	}
done:

	// clear the temp used flags
	for (j = starttri + 1; j < pheader->numtris; j++)
		if (used[j] == 2)
			used[j] = 0;

	return stripcount;
}

/*
================
BuildTris

Generate a list of trifans or strips
for the model, which holds for all frames
================
*/
void BuildTris (void)
{
	int   i, j, k;
	int   startv;
	float s, t;
	int   len, bestlen, besttype;
	int   bestverts[1024];
	int   besttris[1024];
	int   type;

	//
	// build tristrips
	//
	numorder = 0;
	numcommands = 0;
	memset (used, 0, sizeof (used));
	for (i = 0; i < pheader->numtris; i++)
	{
		// pick an unused triangle and start the trifan
		if (used[i])
			continue;

		bestlen = 0;
		besttype = 0;
		for (type = 0; type < 2; type++)
		//	type = 1;
		{
			for (startv = 0; startv < 3; startv++)
			{
				if (type == 1)
					len = StripLength (i, startv);
				else
					len = FanLength (i, startv);
				if (len > bestlen)
				{
					besttype = type;
					bestlen = len;
					for (j = 0; j < bestlen + 2; j++)
						bestverts[j] = stripverts[j];
					for (j = 0; j < bestlen; j++)
						besttris[j] = striptris[j];
				}
			}
		}

		// mark the tris on the best strip as used
		for (j = 0; j < bestlen; j++)
			used[besttris[j]] = 1;

		if (besttype == 1)
			commands[numcommands++] = (bestlen + 2);
		else
			commands[numcommands++] = -(bestlen + 2);

		for (j = 0; j < bestlen + 2; j++)
		{
			int tmp;

			// emit a vertex into the reorder buffer
			k = bestverts[j];
			vertexorder[numorder++] = k;

			// emit s/t coords into the commands stream
			s = stverts[k].s;
			t = stverts[k].t;
			if (!triangles[besttris[0]].facesfront && stverts[k].onseam)
				s += pheader->skinwidth / 2; // on back side
			s = (s + 0.5) / pheader->skinwidth;
			t = (t + 0.5) / pheader->skinheight;

			//	*(float *)&commands[numcommands++] = s;
			//	*(float *)&commands[numcommands++] = t;
			// NOTE: 4 == sizeof(int)
			//	   == sizeof(float)
			memcpy (&tmp, &s, 4);
			commands[numcommands++] = tmp;
			memcpy (&tmp, &t, 4);
			commands[numcommands++] = tmp;
		}
	}

	commands[numcommands++] = 0; // end of list marker

	Con_DPrintf2 ("%3i tri %3i vert %3i cmd\n", pheader->numtris, numorder, numcommands);

	allverts += numorder;
	alltris += pheader->numtris;
}

static void GL_MakeAliasModelDisplayLists_VBO (void);
static void GLMesh_LoadVertexBuffer (qmodel_t *m, const aliashdr_t *hdr);

/*
================
GL_MakeAliasModelDisplayLists
================
*/
void GL_MakeAliasModelDisplayLists (qmodel_t *m, aliashdr_t *hdr)
{
	int         i, j;
	int        *cmds;
	trivertx_t *verts;
	int         count;    // johnfitz -- precompute texcoords for padded skins
	int        *loadcmds; // johnfitz

	aliasmodel = m;
	paliashdr = hdr; // (aliashdr_t *)Mod_Extradata (m);

	// johnfitz -- generate meshes
	Con_DPrintf2 ("meshing %s...\n", m->name);
	BuildTris ();

	// save the data out

	paliashdr->poseverts = numorder;

	cmds = (int *)Mem_Alloc (numcommands * 4);
	paliashdr->commands = (byte *)cmds - (byte *)paliashdr;

	// johnfitz -- precompute texcoords for padded skins
	loadcmds = commands;
	while (1)
	{
		*cmds++ = count = *loadcmds++;

		if (!count)
			break;

		if (count < 0)
			count = -count;

		do
		{
			*(float *)cmds++ = (*(float *)loadcmds++);
			*(float *)cmds++ = (*(float *)loadcmds++);
		} while (--count);
	}
	// johnfitz

	verts = (trivertx_t *)Mem_Alloc (paliashdr->numposes * paliashdr->poseverts * sizeof (trivertx_t));
	paliashdr->posedata = (byte *)verts - (byte *)paliashdr;
	for (i = 0; i < paliashdr->numposes; i++)
		for (j = 0; j < numorder; j++)
			*verts++ = poseverts[i][vertexorder[j]];

	// ericw
	GL_MakeAliasModelDisplayLists_VBO ();
}

unsigned int r_meshindexbuffer = 0;
unsigned int r_meshvertexbuffer = 0;

/*
================
GL_MakeAliasModelDisplayLists_VBO

Saves data needed to build the VBO for this model on the hunk. Afterwards this
is copied to Mod_Extradata.

Original code by MH from RMQEngine
================
*/
void GL_MakeAliasModelDisplayLists_VBO (void)
{
	int             i, j;
	int             maxverts_vbo;
	trivertx_t     *verts;
	unsigned short *indexes;
	aliasmesh_t    *desc;

	// first, copy the verts onto the hunk
	verts = (trivertx_t *)Mem_Alloc (paliashdr->numposes * paliashdr->numverts * sizeof (trivertx_t));
	paliashdr->vertexes = (byte *)verts - (byte *)paliashdr;
	for (i = 0; i < paliashdr->numposes; i++)
		for (j = 0; j < paliashdr->numverts; j++)
			verts[i * paliashdr->numverts + j] = poseverts[i][j];

	// there can never be more than this number of verts and we just put them all on the hunk
	maxverts_vbo = pheader->numtris * 3;
	desc = (aliasmesh_t *)Mem_Alloc (sizeof (aliasmesh_t) * maxverts_vbo);

	// there will always be this number of indexes
	indexes = (unsigned short *)Mem_Alloc (sizeof (unsigned short) * maxverts_vbo);

	pheader->indexes = (intptr_t)indexes - (intptr_t)pheader;
	pheader->meshdesc = (intptr_t)desc - (intptr_t)pheader;
	pheader->numindexes = 0;
	pheader->numverts_vbo = 0;

	for (i = 0; i < pheader->numtris; i++)
	{
		for (j = 0; j < 3; j++)
		{
			int v;

			// index into hdr->vertexes
			unsigned short vertindex = triangles[i].vertindex[j];

			// basic s/t coords
			int s = stverts[vertindex].s;
			int t = stverts[vertindex].t;

			// check for back side and adjust texcoord s
			if (!triangles[i].facesfront && stverts[vertindex].onseam)
				s += pheader->skinwidth / 2;

			// see does this vert already exist
			for (v = 0; v < pheader->numverts_vbo; v++)
			{
				// it could use the same xyz but have different s and t
				if (desc[v].vertindex == vertindex && (int)desc[v].st[0] == s && (int)desc[v].st[1] == t)
				{
					// exists; emit an index for it
					indexes[pheader->numindexes++] = v;

					// no need to check any more
					break;
				}
			}

			if (v == pheader->numverts_vbo)
			{
				// doesn't exist; emit a new vert and index
				indexes[pheader->numindexes++] = pheader->numverts_vbo;

				desc[pheader->numverts_vbo].vertindex = vertindex;
				desc[pheader->numverts_vbo].st[0] = s;
				desc[pheader->numverts_vbo++].st[1] = t;
			}
		}
	}

	// upload immediately
	GLMesh_LoadVertexBuffer (aliasmodel, pheader);
}

#define NUMVERTEXNORMALS 162
extern float r_avertexnormals[NUMVERTEXNORMALS][3];

/*
================
GLMesh_DeleteVertexBuffer
================
*/
static void GLMesh_DeleteVertexBuffer (qmodel_t *m)
{
	if (m->rtvertices != NULL)
	{
	    Mem_Free (m->rtvertices);
	    m->rtvertices = NULL;
	}

	if (m->rtindices != NULL)
	{
		Mem_Free (m->rtindices);
		m->rtindices = NULL;
	}
}

/*
================
GLMesh_LoadVertexBuffer

Upload the given alias model's mesh to a VBO

Original code by MH from RMQEngine
================
*/
static void GLMesh_LoadVertexBuffer (qmodel_t *m, const aliashdr_t *hdr)
{
    GLMesh_DeleteVertexBuffer (m);

	// ericw -- RMQEngine stored these vbo*ofs values in aliashdr_t, but we must not
	// mutate Mod_Extradata since it might be reloaded from disk, so I moved them to qmodel_t
	// (test case: roman1.bsp from arwop, 64mb heap)

	// ericw -- what RMQEngine called nummeshframes is called numposes in QuakeSpasm

	if (isDedicated)
		return;
	if (!hdr->numindexes)
		return;
	if (!hdr->numposes)
		return;
	if (!hdr->numverts_vbo)
		return;

	// grab the pointers to data in the extradata

	const aliasmesh_t *desc			= (aliasmesh_t *)((byte *)hdr + hdr->meshdesc);
	const short       *indexes		=       (short *)((byte *)hdr + hdr->indexes);
	const trivertx_t  *trivertexes	=  (trivertx_t *)((byte *)hdr + hdr->vertexes);

	// create and fill the 32-bit index buffer
	{
		m->rtindices = Mem_Alloc (hdr->numindexes * sizeof (uint32_t));
		assert (hdr->numindexes % 3 == 0);

		for (int k = 0; k < hdr->numindexes / 3; k++)
		{
			m->rtindices[k * 3 + 0] = indexes[k * 3 + 2];
			m->rtindices[k * 3 + 1] = indexes[k * 3 + 1];
			m->rtindices[k * 3 + 2] = indexes[k * 3 + 0];
		}
	}

	// create the vertex buffer (empty)
	{
		size_t sz = hdr->numposes * (hdr->numverts_vbo * sizeof (RgVertex));

		m->rtvertices = Mem_Alloc (sz);
		memset (m->rtvertices, 0, sz);
	}

	// fill in the vertices at the start of the buffer
	for (size_t f = 0; f < (size_t)hdr->numposes; f++) // ericw -- what RMQEngine called nummeshframes is called numposes in QuakeSpasm
	{
		RgVertex *dstpose = m->rtvertices + (hdr->numverts_vbo * f);
		const trivertx_t *srctv = trivertexes + (hdr->numverts * f);

		for (int v = 0; v < hdr->numverts_vbo; v++)
		{
			trivertx_t trivert = srctv[desc[v].vertindex];

			dstpose[v].position[0] = trivert.v[0];
			dstpose[v].position[1] = trivert.v[1];
			dstpose[v].position[2] = trivert.v[2];
			
			dstpose[v].normal[0] = r_avertexnormals[trivert.lightnormalindex][0];
			dstpose[v].normal[1] = r_avertexnormals[trivert.lightnormalindex][1];
			dstpose[v].normal[2] = r_avertexnormals[trivert.lightnormalindex][2];

			// texCoord is same in all poses
			dstpose[v].texCoord[0] = ((float)desc[v].st[0] + 0.5f) / (float)hdr->skinwidth;
			dstpose[v].texCoord[1] = ((float)desc[v].st[1] + 0.5f) / (float)hdr->skinheight;

			dstpose[v].packedColor = RT_PACKED_COLOR_WHITE;
		}
	}
}

/*
================
GLMesh_LoadVertexBuffers

Loop over all precached alias models, and upload each one to a VBO.
================
*/
void GLMesh_LoadVertexBuffers (void)
{
	int               j;
	qmodel_t         *m;
	const aliashdr_t *hdr;

	for (j = 1; j < MAX_MODELS; j++)
	{
		if (!(m = cl.model_precache[j]))
			break;
		if (m->type != mod_alias)
			continue;

		hdr = (const aliashdr_t *)Mod_Extradata (m);

		GLMesh_LoadVertexBuffer (m, hdr);
	}
}

/*
================
GLMesh_DeleteVertexBuffers

Delete VBOs for all loaded alias models
================
*/
void GLMesh_DeleteVertexBuffers (void)
{
	int       j;
	qmodel_t *m;

	for (j = 1; j < MAX_MODELS; j++)
	{
		if (!(m = cl.model_precache[j]))
			break;
		if (m->type != mod_alias)
			continue;

		GLMesh_DeleteVertexBuffer (m);
	}
}
