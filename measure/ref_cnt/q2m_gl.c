/*
 * q2m_gl.c — замерочная сборка Quake 2 для VDAC4: «считающий» GL для ref_gl.
 *
 * Рендер ref_gl id работает как обычно: обходит BSP, отсекает по PVS и пирамиде
 * видимости, интерполирует кадры моделей, считает свет. Но вызовы OpenGL уходят сюда,
 * и вместо рисования мы считаем то, что получил бы растеризатор в ПЛИС:
 *   - примитивы (многоугольник, полоса, веер, треугольники), вершины, треугольники;
 *   - площадь на экране в точках после отсечения ближней плоскостью и краями экрана
 *     и после отбрасывания задних граней (как делает GL) — отсюда перерисовка;
 *   - смены текстур и объём разных текстур за кадр, подгрузку динамических lightmap.
 * Всё раскладывается по видам рисования q2m_cat (q2m_ref.h).
 *
 * Матрицы ведём сами (MatrixMode, Rotate, Translate, Frustum, Ortho…), как GL 1.1.
 * cvar q2m_count 0 выключает подсчёт: остаются пустые заглушки — так меряется чистое
 * время подготовки кадра на процессоре.
 *
 * Файлы: <q2m_out>.ref.csv — строка на кадр; <q2m_out>.ref.jsonl — память рендера по
 * картам (команда q2m_refstats).
 */

#include "gl_local.h"
#include <math.h>
#include <stdio.h>

int q2m_cat;

extern model_t mod_known[];		/* gl_model.c */
extern int     mod_numknown;

/* ------------------------------------------------------------ состояние */

static cvar_t *q2m_out, *q2m_count, *q2m_frame;
static FILE   *q2m_csv;
static FILE   *q2m_json;

typedef struct
{
	int    prims, verts, tris;
	double area;
} q2m_catstat_t;

static struct
{
	q2m_catstat_t c[Q2M_C_N];
	int    binds, utex, ulm;
	double utex_bytes;
	double dlm_luxels;		/* подгрузка динамических lightmap за кадр (точек) */
	double teximg_texels;	/* TexImage2D во время кадра (не при загрузке уровня) */
} fs;

static int in_frame;

/* матрицы, столбцами как в GL */
static float mv_stack[32][16], pr_stack[8][16];
static int   mv_top, pr_top;
static int   matrix_mode = GL_MODELVIEW;
static float mvp[16];
static int   vp_x, vp_y, vp_w = 640, vp_h = 480;
static int   cull_on, cull_mode = GL_BACK, front_ccw = 1;

static float *Q2M_Cur (void)
{
	return matrix_mode == GL_PROJECTION ? pr_stack[pr_top] : mv_stack[mv_top];
}

static void Q2M_Identity (float *m)
{
	memset (m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1;
}

/* r = a * b (столбцами) */
static void Q2M_Mul (float *r, const float *a, const float *b)
{
	float t[16];
	int   i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			t[j * 4 + i] = a[0 * 4 + i] * b[j * 4 + 0] + a[1 * 4 + i] * b[j * 4 + 1]
			             + a[2 * 4 + i] * b[j * 4 + 2] + a[3 * 4 + i] * b[j * 4 + 3];
	memcpy (r, t, sizeof(t));
}

static void Q2M_MulCur (const float *m)
{
	float *c = Q2M_Cur ();
	Q2M_Mul (c, c, m);
}

/* ------------------------------------------------ сборка треугольников */

typedef struct { float x, y, z, w; } q2m_v4;

static GLenum prim_mode;
static int    prim_n;
static q2m_v4 v_first, v_prev2, v_prev1, q_first, q_b, q_c;

#define Q2M_MAXP 16

/* Отсечь многоугольник по плоскости d(v) >= 0 (Сазерленд — Ходжмен). */
static int Q2M_ClipH (q2m_v4 *in, int n, q2m_v4 *out, int which)
{
	int i, m = 0;

	for (i = 0; i < n; i++)
	{
		q2m_v4 *a = &in[i], *b = &in[(i + 1) % n];
		float   da, db;

		switch (which)
		{
		case 0: da = a->z + a->w; db = b->z + b->w; break;	/* ближняя плоскость */
		default: da = a->w - 0.001f; db = b->w - 0.001f; break;
		}
		if (da >= 0)
			out[m++] = *a;
		if ((da >= 0) != (db >= 0) && m < Q2M_MAXP)
		{
			float  t = da / (da - db);
			q2m_v4 r;

			r.x = a->x + (b->x - a->x) * t;
			r.y = a->y + (b->y - a->y) * t;
			r.z = a->z + (b->z - a->z) * t;
			r.w = a->w + (b->w - a->w) * t;
			out[m++] = r;
		}
		if (m >= Q2M_MAXP - 1)
			break;
	}
	return m;
}

/* Отсечь 2D-многоугольник по краю окна: ось 0 — x, 1 — y; sign 1 — «не меньше», -1 — «не больше» */
static int Q2M_Clip2 (float (*in)[2], int n, float (*out)[2], int axis, float lim, int sign)
{
	int i, m = 0;

	for (i = 0; i < n; i++)
	{
		float *a = in[i], *b = in[(i + 1) % n];
		float  da = (a[axis] - lim) * sign, db = (b[axis] - lim) * sign;

		if (da >= 0)
		{
			out[m][0] = a[0];
			out[m][1] = a[1];
			m++;
		}
		if ((da >= 0) != (db >= 0) && m < Q2M_MAXP)
		{
			float t = da / (da - db);

			out[m][0] = a[0] + (b[0] - a[0]) * t;
			out[m][1] = a[1] + (b[1] - a[1]) * t;
			m++;
		}
		if (m >= Q2M_MAXP - 1)
			break;
	}
	return m;
}

/* Площадь треугольника на экране (точки), 0 — отброшен как задняя грань или вне экрана. */
static double Q2M_TriArea (const q2m_v4 *a, const q2m_v4 *b, const q2m_v4 *c)
{
	q2m_v4 p0[Q2M_MAXP], p1[Q2M_MAXP];
	float  s0[Q2M_MAXP][2], s1[Q2M_MAXP][2];
	int    n, i;
	double area = 0;

	p0[0] = *a; p0[1] = *b; p0[2] = *c;
	n = Q2M_ClipH (p0, 3, p1, 0);
	if (n < 3)
		return 0;
	n = Q2M_ClipH (p1, n, p0, 1);
	if (n < 3)
		return 0;
	for (i = 0; i < n; i++)
	{
		s0[i][0] = vp_x + (p0[i].x / p0[i].w + 1) * 0.5f * vp_w;
		s0[i][1] = vp_y + (p0[i].y / p0[i].w + 1) * 0.5f * vp_h;
	}
	/* знак площади до обрезки краями экрана: он и решает лицо/изнанку */
	for (i = 0; i < n; i++)
		area += (double)s0[i][0] * s0[(i + 1) % n][1] - (double)s0[(i + 1) % n][0] * s0[i][1];
	if (cull_on)
	{
		int front = (area > 0) == (front_ccw != 0);

		if (cull_mode == GL_FRONT_AND_BACK)
			return 0;
		if (cull_mode == GL_FRONT && front)
			return 0;
		if (cull_mode == GL_BACK && !front)
			return 0;
	}
	n = Q2M_Clip2 (s0, n, s1, 0, (float)vp_x, 1);
	if (n < 3) return 0;
	n = Q2M_Clip2 (s1, n, s0, 0, (float)(vp_x + vp_w), -1);
	if (n < 3) return 0;
	n = Q2M_Clip2 (s0, n, s1, 1, (float)vp_y, 1);
	if (n < 3) return 0;
	n = Q2M_Clip2 (s1, n, s0, 1, (float)(vp_y + vp_h), -1);
	if (n < 3) return 0;
	area = 0;
	for (i = 0; i < n; i++)
		area += (double)s0[i][0] * s0[(i + 1) % n][1] - (double)s0[(i + 1) % n][0] * s0[i][1];
	return fabs (area) * 0.5;
}

static void Q2M_Tri (const q2m_v4 *a, const q2m_v4 *b, const q2m_v4 *c)
{
	q2m_catstat_t *s = &fs.c[q2m_cat];

	s->tris++;
	s->area += Q2M_TriArea (a, b, c);
}

static void Q2M_Vertex (float x, float y, float z)
{
	q2m_v4 v;

	fs.c[q2m_cat].verts++;
	if (!q2m_count->value)
		return;
	v.x = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
	v.y = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
	v.z = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
	v.w = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];

	switch (prim_mode)
	{
	case GL_TRIANGLES:
		if (prim_n % 3 == 0) v_prev2 = v;
		else if (prim_n % 3 == 1) v_prev1 = v;
		else Q2M_Tri (&v_prev2, &v_prev1, &v);
		break;
	case GL_TRIANGLE_STRIP:
		if (prim_n >= 2)
		{
			if ((prim_n - 2) & 1)
				Q2M_Tri (&v_prev1, &v_prev2, &v);
			else
				Q2M_Tri (&v_prev2, &v_prev1, &v);
		}
		v_prev2 = v_prev1;
		v_prev1 = v;
		break;
	case GL_TRIANGLE_FAN:
	case GL_POLYGON:
		if (prim_n == 0)
			v_first = v;
		else if (prim_n >= 2)
			Q2M_Tri (&v_first, &v_prev1, &v);
		v_prev1 = v;
		break;
	case GL_QUADS:
		switch (prim_n & 3)
		{
		case 0: q_first = v; break;
		case 1: q_b = v; break;
		case 2: q_c = v; Q2M_Tri (&q_first, &q_b, &q_c); break;
		case 3: Q2M_Tri (&q_first, &q_c, &v); break;
		}
		break;
	default:	/* линии и точки: площади нет */
		break;
	}
	prim_n++;
}

/* ------------------------------------------- заменённые функции GL */

static void APIENTRY q2m_glBegin (GLenum mode)
{
	prim_mode = mode;
	prim_n = 0;
	fs.c[q2m_cat].prims++;
	if (q2m_count->value)
		Q2M_Mul (mvp, pr_stack[pr_top], mv_stack[mv_top]);
}

static void APIENTRY q2m_glEnd (void)
{
	/* у треугольников площадь уже сложена; здесь считать нечего */
}

static void APIENTRY q2m_glVertex2f (GLfloat x, GLfloat y)
{
	Q2M_Vertex (x, y, 0);
}

static void APIENTRY q2m_glVertex3f (GLfloat x, GLfloat y, GLfloat z)
{
	Q2M_Vertex (x, y, z);
}

static void APIENTRY q2m_glVertex3fv (const GLfloat *v)
{
	Q2M_Vertex (v[0], v[1], v[2]);
}

static void APIENTRY q2m_glMatrixMode (GLenum mode)
{
	matrix_mode = mode;
}

static void APIENTRY q2m_glLoadIdentity (void)
{
	Q2M_Identity (Q2M_Cur ());
}

static void APIENTRY q2m_glLoadMatrixf (const GLfloat *m)
{
	memcpy (Q2M_Cur (), m, 16 * sizeof(float));
}

static void APIENTRY q2m_glMultMatrixf (const GLfloat *m)
{
	Q2M_MulCur (m);
}

static void APIENTRY q2m_glPushMatrix (void)
{
	if (matrix_mode == GL_PROJECTION)
	{
		if (pr_top < 7)
		{
			memcpy (pr_stack[pr_top + 1], pr_stack[pr_top], sizeof(pr_stack[0]));
			pr_top++;
		}
	}
	else if (mv_top < 31)
	{
		memcpy (mv_stack[mv_top + 1], mv_stack[mv_top], sizeof(mv_stack[0]));
		mv_top++;
	}
}

static void APIENTRY q2m_glPopMatrix (void)
{
	if (matrix_mode == GL_PROJECTION)
	{
		if (pr_top > 0)
			pr_top--;
	}
	else if (mv_top > 0)
		mv_top--;
}

static void APIENTRY q2m_glTranslatef (GLfloat x, GLfloat y, GLfloat z)
{
	float m[16];

	Q2M_Identity (m);
	m[12] = x; m[13] = y; m[14] = z;
	Q2M_MulCur (m);
}

static void APIENTRY q2m_glScalef (GLfloat x, GLfloat y, GLfloat z)
{
	float m[16];

	Q2M_Identity (m);
	m[0] = x; m[5] = y; m[10] = z;
	Q2M_MulCur (m);
}

static void APIENTRY q2m_glRotatef (GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
	float  m[16], c, s, l;
	double a = angle * M_PI / 180.0;

	l = (float)sqrt (x * x + y * y + z * z);
	if (l <= 0)
		return;
	x /= l; y /= l; z /= l;
	c = (float)cos (a);
	s = (float)sin (a);
	Q2M_Identity (m);
	m[0] = x * x * (1 - c) + c;     m[4] = x * y * (1 - c) - z * s; m[8] = x * z * (1 - c) + y * s;
	m[1] = y * x * (1 - c) + z * s; m[5] = y * y * (1 - c) + c;     m[9] = y * z * (1 - c) - x * s;
	m[2] = x * z * (1 - c) - y * s; m[6] = y * z * (1 - c) + x * s; m[10] = z * z * (1 - c) + c;
	Q2M_MulCur (m);
}

static void APIENTRY q2m_glFrustum (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
	float m[16];

	memset (m, 0, sizeof(m));
	m[0] = (float)(2 * n / (r - l));
	m[5] = (float)(2 * n / (t - b));
	m[8] = (float)((r + l) / (r - l));
	m[9] = (float)((t + b) / (t - b));
	m[10] = (float)(-(f + n) / (f - n));
	m[11] = -1;
	m[14] = (float)(-2 * f * n / (f - n));
	Q2M_MulCur (m);
}

static void APIENTRY q2m_glOrtho (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
	float m[16];

	Q2M_Identity (m);
	m[0] = (float)(2 / (r - l));
	m[5] = (float)(2 / (t - b));
	m[10] = (float)(-2 / (f - n));
	m[12] = (float)(-(r + l) / (r - l));
	m[13] = (float)(-(t + b) / (t - b));
	m[14] = (float)(-(f + n) / (f - n));
	Q2M_MulCur (m);
}

static void APIENTRY q2m_glViewport (GLint x, GLint y, GLsizei w, GLsizei h)
{
	vp_x = x; vp_y = y; vp_w = w; vp_h = h;
}

static void APIENTRY q2m_glGetFloatv (GLenum pname, GLfloat *params)
{
	if (pname == GL_MODELVIEW_MATRIX)
		memcpy (params, mv_stack[mv_top], 16 * sizeof(float));
	else if (pname == GL_PROJECTION_MATRIX)
		memcpy (params, pr_stack[pr_top], 16 * sizeof(float));
	else
		params[0] = 0;
}

static void APIENTRY q2m_glGetIntegerv (GLenum pname, GLint *params)
{
	params[0] = 0;
}

static const GLubyte *APIENTRY q2m_glGetString (GLenum name)
{
	switch (name)
	{
	case GL_VENDOR:     return (const GLubyte *)"VDAC4 q2m";
	case GL_RENDERER:   return (const GLubyte *)"q2m counting rasterizer";
	case GL_VERSION:    return (const GLubyte *)"1.1";
	case GL_EXTENSIONS: return (const GLubyte *)"GL_SGIS_multitexture";
	}
	return (const GLubyte *)"";
}

static GLenum APIENTRY q2m_glGetError (void)
{
	return GL_NO_ERROR;
}

static void APIENTRY q2m_glEnable (GLenum cap)
{
	if (cap == GL_CULL_FACE)
		cull_on = 1;
}

static void APIENTRY q2m_glDisable (GLenum cap)
{
	if (cap == GL_CULL_FACE)
		cull_on = 0;
}

static void APIENTRY q2m_glCullFace (GLenum mode)
{
	cull_mode = mode;
}

static void APIENTRY q2m_glFrontFace (GLenum mode)
{
	front_ccw = mode == GL_CCW;
}

static int tex_stamp[4096];
static int frame_stamp = 1;

static void APIENTRY q2m_glBindTexture (GLenum target, GLuint texture)
{
	fs.binds++;
	if (texture >= 4096 || tex_stamp[texture] == frame_stamp)
		return;
	tex_stamp[texture] = frame_stamp;
	if (texture >= TEXNUM_IMAGES && texture < TEXNUM_IMAGES + MAX_GLTEXTURES)
	{
		image_t *img = &gltextures[texture - TEXNUM_IMAGES];

		fs.utex++;
		fs.utex_bytes += (double)img->width * img->height;	/* 8 бит, верхний уровень mip */
	}
	else if (texture >= TEXNUM_LIGHTMAPS && texture < TEXNUM_SCRAPS)
		fs.ulm++;
}

static void APIENTRY q2m_glTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset,
	GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
	if (in_frame)
		fs.dlm_luxels += (double)width * height;
}

static void APIENTRY q2m_glTexImage2D (GLenum target, GLint level, GLint internalformat, GLsizei width,
	GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
	if (in_frame)
		fs.teximg_texels += (double)width * height;
}

static void APIENTRY q2m_glMTexCoord2fSGIS (GLenum target, GLfloat s, GLfloat t)
{
}

static void APIENTRY q2m_glSelectTextureSGIS (GLenum target)
{
}

static PROC WINAPI q2m_wglGetProcAddress (LPCSTR name)
{
	if (!strcmp (name, "glMTexCoord2fSGIS"))
		return (PROC)q2m_glMTexCoord2fSGIS;
	if (!strcmp (name, "glSelectTextureSGIS"))
		return (PROC)q2m_glSelectTextureSGIS;
	return NULL;
}

/* Поставить считающие функции вместо пустых заглушек (зовёт QGL_Init из qgl_cnt.c). */
void Q2M_InstallGL (void)
{
	qglBegin = q2m_glBegin;
	qglEnd = q2m_glEnd;
	qglVertex2f = q2m_glVertex2f;
	qglVertex3f = q2m_glVertex3f;
	qglVertex3fv = q2m_glVertex3fv;
	qglMatrixMode = q2m_glMatrixMode;
	qglLoadIdentity = q2m_glLoadIdentity;
	qglLoadMatrixf = q2m_glLoadMatrixf;
	qglMultMatrixf = q2m_glMultMatrixf;
	qglPushMatrix = q2m_glPushMatrix;
	qglPopMatrix = q2m_glPopMatrix;
	qglTranslatef = q2m_glTranslatef;
	qglScalef = q2m_glScalef;
	qglRotatef = q2m_glRotatef;
	qglFrustum = q2m_glFrustum;
	qglOrtho = q2m_glOrtho;
	qglViewport = q2m_glViewport;
	qglGetFloatv = q2m_glGetFloatv;
	qglGetIntegerv = q2m_glGetIntegerv;
	qglGetString = q2m_glGetString;
	qglGetError = q2m_glGetError;
	qglEnable = q2m_glEnable;
	qglDisable = q2m_glDisable;
	qglCullFace = q2m_glCullFace;
	qglFrontFace = q2m_glFrontFace;
	qglBindTexture = q2m_glBindTexture;
	qglTexSubImage2D = q2m_glTexSubImage2D;
	qglTexImage2D = q2m_glTexImage2D;
	qwglGetProcAddress = q2m_wglGetProcAddress;
	Q2M_Identity (mv_stack[0]);
	Q2M_Identity (pr_stack[0]);
}

/* ---------------------------------------------------- GLimp без окна */

static void Q2M_OpenFiles (void)
{
	char name[MAX_OSPATH + 16];

	if (q2m_csv || !q2m_out->string[0])
		return;
	Com_sprintf (name, sizeof(name), "%s.ref.csv", q2m_out->string);
	q2m_csv = fopen (name, "w");
	Com_sprintf (name, sizeof(name), "%s.ref.jsonl", q2m_out->string);
	q2m_json = fopen (name, "w");
	if (q2m_csv)
	{
		static const char *cn[Q2M_C_N] = { "world", "warp", "sky", "alias", "bmodel", "sprite",
			"part", "alpha", "blend", "2d" };
		int i;

		fprintf (q2m_csv, "frame,w,h,ents,parts,dlights,binds,utex,utex_bytes,ulm,dlm_luxels,teximg_texels");
		for (i = 0; i < Q2M_C_N; i++)
			fprintf (q2m_csv, ",%s_prims,%s_verts,%s_tris,%s_area", cn[i], cn[i], cn[i], cn[i]);
		fprintf (q2m_csv, "\n");
	}
}

int GLimp_Init (void *hinstance, void *hWnd)
{
	q2m_out = ri.Cvar_Get ("q2m_out", "", 0);
	q2m_count = ri.Cvar_Get ("q2m_count", "1", 0);
	q2m_frame = ri.Cvar_Get ("q2m_frame", "0", 0);
	Q2M_OpenFiles ();
	return true;
}

int GLimp_SetMode (int *pwidth, int *pheight, int mode, qboolean fullscreen)
{
	int width, height;

	if (!ri.Vid_GetModeInfo (&width, &height, mode))
		return rserr_invalid_mode;
	*pwidth = width;
	*pheight = height;
	ri.Vid_NewWindow (width, height);
	return rserr_ok;
}

void GLimp_Shutdown (void)
{
	if (q2m_csv)
		fclose (q2m_csv);
	if (q2m_json)
		fclose (q2m_json);
	q2m_csv = q2m_json = NULL;
}

void GLimp_BeginFrame (float camera_separation)
{
	memset (&fs, 0, sizeof(fs));
	frame_stamp++;
	in_frame = 1;
	q2m_cat = Q2M_C_2D;
}

void GLimp_EndFrame (void)
{
	int i;

	in_frame = 0;
	if (!q2m_csv)
		return;
	fprintf (q2m_csv, "%d,%d,%d,%d,%d,%d,%d,%d,%.0f,%d,%.0f,%.0f", (int)q2m_frame->value,
		vid.width, vid.height, r_newrefdef.num_entities, r_newrefdef.num_particles,
		r_newrefdef.num_dlights, fs.binds, fs.utex, fs.utex_bytes, fs.ulm, fs.dlm_luxels,
		fs.teximg_texels);
	for (i = 0; i < Q2M_C_N; i++)
		fprintf (q2m_csv, ",%d,%d,%d,%.0f", fs.c[i].prims, fs.c[i].verts, fs.c[i].tris, fs.c[i].area);
	fprintf (q2m_csv, "\n");
}

void GLimp_AppActivate (qboolean active)
{
}

void GLimp_EnableLogging (qboolean enable)
{
}

void GLimp_LogNewFrame (void)
{
}

/* -------------------------------------- память рендера на карте (JSON) */

static void Q2M_Str (FILE *f, const char *s)
{
	fputc ('"', f);
	for ( ; *s; s++)
	{
		if (*s == '"' || *s == '\\')
			fputc ('\\', f);
		if ((unsigned char)*s >= 32)
			fputc (*s, f);
	}
	fputc ('"', f);
}

/* «q2m_refstats <имя>»: мир (что лежало бы в ESP), lightmap, модели и картинки (что в SDRAM). */
static void Q2M_RefStats_f (void)
{
	FILE       *f = q2m_json;
	model_t    *w = r_worldmodel;
	int         i, first;
	double      glpoly = 0, lux_all = 0, lux_base = 0;
	int         vis_lump = 0, light_lump = 0;
	dheader_t  *hdr;

	if (!f || !w)
		return;

	for (i = 0; i < w->numsurfaces; i++)
	{
		msurface_t *s = &w->surfaces[i];
		glpoly_t   *p;
		int         maps;

		for (p = s->polys; p; p = p->next)
			glpoly += sizeof(glpoly_t) + (p->numverts - 4) * VERTEXSIZE * sizeof(float);
		if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB) || !s->samples)
			continue;
		for (maps = 0; maps < MAXLIGHTMAPS && s->styles[maps] != 255; maps++)
			;
		lux_base += (double)((s->extents[0] >> 4) + 1) * ((s->extents[1] >> 4) + 1);
		lux_all += (double)((s->extents[0] >> 4) + 1) * ((s->extents[1] >> 4) + 1) * maps;
	}
	if (ri.FS_LoadFile (w->name, (void **)&hdr) > 0)
	{
		vis_lump = LittleLong (hdr->lumps[LUMP_VISIBILITY].filelen);
		light_lump = LittleLong (hdr->lumps[LUMP_LIGHTING].filelen);
		ri.FS_FreeFile (hdr);
	}

	fprintf (f, "{\"map\":");
	Q2M_Str (f, ri.Cmd_Argv (1));
	fprintf (f, ",\"vid\":[%d,%d],\"world\":{\"name\":", vid.width, vid.height);
	Q2M_Str (f, w->name);
	fprintf (f, ",\"submodels\":[%d,%d],\"planes\":[%d,%d],\"leafs\":[%d,%d],\"vertexes\":[%d,%d],"
		"\"edges\":[%d,%d],\"nodes\":[%d,%d],\"texinfo\":[%d,%d],\"surfaces\":[%d,%d],"
		"\"surfedges\":[%d,%d],\"marksurfaces\":[%d,%d],\"vis_lump\":%d,\"lighting_lump\":%d,"
		"\"extradatasize\":%d,\"glpoly_bytes\":%.0f,\"lux_base\":%.0f,\"lux_all\":%.0f}",
		w->numsubmodels, (int)sizeof(mmodel_t), w->numplanes, (int)sizeof(cplane_t),
		w->numleafs, (int)sizeof(mleaf_t), w->numvertexes, (int)sizeof(mvertex_t),
		w->numedges, (int)sizeof(medge_t), w->numnodes, (int)sizeof(mnode_t),
		w->numtexinfo, (int)sizeof(mtexinfo_t), w->numsurfaces, (int)sizeof(msurface_t),
		w->numsurfedges, (int)sizeof(int), w->nummarksurfaces, (int)sizeof(msurface_t *),
		vis_lump, light_lump, w->extradatasize, glpoly, lux_base, lux_all);

	fprintf (f, ",\"models\":[");
	first = 1;
	for (i = 0; i < mod_numknown; i++)
	{
		model_t *m = &mod_known[i];

		if (!m->name[0] || m->registration_sequence != registration_sequence)
			continue;
		fprintf (f, first ? "[" : ",[");
		first = 0;
		Q2M_Str (f, m->name);
		fprintf (f, ",%d,%d,%d]", m->type, m->extradatasize, m->numframes);
	}
	fprintf (f, "],\"images\":[");
	first = 1;
	for (i = 0; i < numgltextures; i++)
	{
		image_t *img = &gltextures[i];

		if (!img->registration_sequence)
			continue;
		if (img->registration_sequence != registration_sequence && img->type != it_pic)
			continue;
		fprintf (f, first ? "[" : ",[");
		first = 0;
		Q2M_Str (f, img->name);
		fprintf (f, ",%d,%d,%d,%d]", img->type, img->width, img->height,
			img->registration_sequence == registration_sequence);
	}
	fprintf (f, "]}\n");
	fflush (f);
}

/* Зовётся из R_Register (вставка в gl_rmain.c). */
void Q2M_RefRegister (void)
{
	ri.Cmd_AddCommand ("q2m_refstats", Q2M_RefStats_f);
}
