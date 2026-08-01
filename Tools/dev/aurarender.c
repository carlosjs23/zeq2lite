/*
 * aurarender.c
 * Render the aura pipeline headlessly: the real mesh, the real shaders.
 *
 *   cc -O2 -o aurarender aurarender.c -lz \
 *      -framework OpenGL -DGL_SILENCE_DEPRECATION
 *   ./aurarender aura.iqm aura_vp.glsl aura_fp.glsl out.png [size] [strip.raw]
 *
 * Loads the ring mesh the build generated, compiles the very GLSL the game
 * ships, binds the same uniform contract cg_auras.c fills in, draws one
 * frame into an offscreen framebuffer, and writes the alpha plane as a PNG.
 * What lands in that image is the pipeline's own output - vertex program,
 * fragment program, mesh payload and all - with no game engine anywhere.
 *
 * A windowless CGL context is what makes this possible on macOS: a legacy
 * (2.1) profile, matching the engine's, needs no window, no display and no
 * permissions. The uniforms are the engine's names; the values below are a
 * standing player at a typical camera range, time zero. Anything that moves
 * with time - scroll, wobble - is evaluated exactly as the game would at
 * that instant, because it is the same code running.
 *
 * The output is the red plane after compositing over black with the
 * stage's real blend (GL_ONE, GL_ONE_MINUS_SRC_ALPHA): the fragment stage
 * emits premultiplied colour, so over black that plane is the aura's
 * luminance - the thing the reference art actually shows. With a strip
 * argument, the raw RGBA blob (two u32le then pixels) lands on texture
 * unit zero with the clampmapT wrap the game stage uses.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#define TAU 6.28318530717958647692

/* --------------------------------------------------------------------------
   The player this frame stands on: a humanoid bounding box, relative to the
   entity origin, and the aura configuration cg_auras.c would send for the
   stock tierDefault. Keep these in step with the tier config by hand.
-------------------------------------------------------------------------- */

static const float boxMins[3] = { -16.0f, -16.0f, -30.0f };
static const float boxMaxs[3] = {  16.0f,  16.0f,  42.0f };

#define AURA_SCALE      1.2f     /* auraScale * modulate(1) */
#define ORIGIN_DISTANCE 1.05f    /* auraOriginDistance */
#define PADDING         0.02f    /* auraPadding */
#define AMPLITUDE       1.0f     /* auraAmplitude */
#define WAVELENGTH      2.0f     /* auraWavelength */
#define SCROLL_SPEED    1.5f     /* auraScrollSpeed */

#define CAM_RANGE 160.0f
#define CAM_FOV   75.0f

/* ---------------------------------------------------------------- PNG out */

static void BE32(unsigned char *p, unsigned long v) {
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)(v);
}

static void PngChunk(FILE *f, const char *type, const unsigned char *data, unsigned long len) {
	unsigned char head[8];
	unsigned long crc;

	BE32(head, len);
	memcpy(head + 4, type, 4);
	fwrite(head, 1, 8, f);
	if (len)
		fwrite(data, 1, len, f);
	crc = crc32(0, (const unsigned char *)type, 4);
	if (len)
		crc = crc32(crc, data, len);
	BE32(head, crc);
	fwrite(head, 1, 4, f);
}

static int WriteGreyPNG(const char *path, const unsigned char *grey, int w, int h) {
	unsigned long rawLen = (unsigned long)h * (1 + (unsigned long)w);
	unsigned char *raw = malloc(rawLen);
	uLongf zLen = compressBound(rawLen);
	unsigned char *z = malloc(zLen);
	unsigned char ihdr[13];
	FILE *f;
	int y;

	if (!raw || !z)
		return 0;
	for (y = 0; y < h; y++) {
		raw[(unsigned long)y * (w + 1)] = 0;
		memcpy(raw + (unsigned long)y * (w + 1) + 1,
		       grey + (unsigned long)y * w, w);
	}
	if (compress2(z, &zLen, raw, rawLen, 9) != Z_OK)
		return 0;
	f = fopen(path, "wb");
	if (!f)
		return 0;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	BE32(ihdr, (unsigned long)w);
	BE32(ihdr + 4, (unsigned long)h);
	ihdr[8] = 8;
	ihdr[9] = 0;	/* greyscale */
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	PngChunk(f, "IHDR", ihdr, 13);
	PngChunk(f, "IDAT", z, zLen);
	PngChunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw);
	free(z);
	return 1;
}

/* ---------------------------------------------------------------- IQM in */

typedef struct {
	int            numVerts, numTris;
	float         *positions;	/* 3f */
	float         *texcoords;	/* 2f */
	float         *normals;		/* 3f: direction + outline radius payload */
	unsigned char *colors;		/* 4ub */
	unsigned int  *tris;		/* 3ui */
} mesh_t;

static int LoadIQM(const char *path, mesh_t *m) {
	FILE *f = fopen(path, "rb");
	unsigned char *data;
	long size;
	unsigned int hdr[27];
	unsigned int numVA, numV, ofsVA, numTri, ofsTri;
	unsigned int i;

	if (!f) {
		fprintf(stderr, "aurarender: cannot open %s\n", path);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(size);
	if (fread(data, 1, size, f) != (size_t)size) {
		fclose(f);
		return 0;
	}
	fclose(f);

	if (memcmp(data, "INTERQUAKEMODEL\0", 16)) {
		fprintf(stderr, "aurarender: %s is not an IQM\n", path);
		return 0;
	}
	memcpy(hdr, data + 16, sizeof(hdr));
	numV   = hdr[8];
	numVA  = hdr[7];
	ofsVA  = hdr[9];
	numTri = hdr[10];
	ofsTri = hdr[11];

	m->numVerts = (int)numV;
	m->numTris = (int)numTri;
	m->positions = NULL;
	m->texcoords = NULL;
	m->normals = NULL;
	m->colors = NULL;

	for (i = 0; i < numVA; i++) {
		unsigned int va[5];

		memcpy(va, data + ofsVA + i * 20, 20);
		/* type, flags, format, size, offset */
		if (va[0] == 0 && va[2] == 7)		/* IQM_POSITION, float */
			m->positions = (float *)(data + va[4]);
		else if (va[0] == 1 && va[2] == 7)	/* IQM_TEXCOORD, float */
			m->texcoords = (float *)(data + va[4]);
		else if (va[0] == 2 && va[2] == 7)	/* IQM_NORMAL, float */
			m->normals = (float *)(data + va[4]);
		else if (va[0] == 6 && va[2] == 1)	/* IQM_COLOR, ubyte */
			m->colors = data + va[4];
	}
	m->tris = (unsigned int *)(data + ofsTri);

	if (!m->positions || !m->texcoords || !m->normals || !m->colors) {
		fprintf(stderr, "aurarender: %s lacks a needed vertex array\n", path);
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------- GL helpers */

static char *ReadFile(const char *path) {
	FILE *f = fopen(path, "rb");
	long size;
	char *text;

	if (!f) {
		fprintf(stderr, "aurarender: cannot open %s\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	text = malloc(size + 1);
	if (fread(text, 1, size, f) != (size_t)size) {
		fclose(f);
		return NULL;
	}
	text[size] = 0;
	fclose(f);
	return text;
}

static GLuint CompileStage(GLenum kind, const char *path) {
	char *src = ReadFile(path);
	GLuint sh;
	GLint ok;

	if (!src)
		return 0;
	sh = glCreateShader(kind);
	glShaderSource(sh, 1, (const GLchar **)&src, NULL);
	glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[4096];

		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		fprintf(stderr, "aurarender: %s failed to compile:\n%s\n", path, log);
		return 0;
	}
	free(src);
	return sh;
}

/* Column-major, as glUniformMatrix4fv wants them. */

static void MatPerspective(float *m, float fovyDeg, float aspect, float zn, float zf) {
	float t = 1.0f / tanf((float)(fovyDeg * TAU / 720.0));

	memset(m, 0, 16 * sizeof(float));
	m[0]  = t / aspect;
	m[5]  = t;
	m[10] = (zf + zn) / (zn - zf);
	m[11] = -1.0f;
	m[14] = 2.0f * zf * zn / (zn - zf);
}

/* The camera sits south of the player on -Y, level with the box centre,
   looking +Y with Z up: eye space x = world x, y = world z - eyeZ,
   z = -(world y + range). */
static void MatLookNorth(float *m, float range, float eyeZ) {
	memset(m, 0, 16 * sizeof(float));
	m[0] = 1.0f;			/* x -> x */
	m[6] = -1.0f;			/* y -> -z */
	m[9] = 1.0f;			/* z -> y */
	m[13] = -eyeZ;
	m[14] = -range;
	m[15] = 1.0f;
}

static void MatMul(float *out, const float *a, const float *b) {
	int r, c, k;

	for (c = 0; c < 4; c++) {
		for (r = 0; r < 4; r++) {
			float s = 0.0f;

			for (k = 0; k < 4; k++)
				s += a[k * 4 + r] * b[c * 4 + k];
			out[c * 4 + r] = s;
		}
	}
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
	const char *meshPath, *vpPath, *fpPath, *outPath, *stripPath;
	int size = 1024;
	mesh_t mesh;
	CGLPixelFormatAttribute attrs[] = {
		kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
		kCGLPFAColorSize, (CGLPixelFormatAttribute)32,
		kCGLPFAAllowOfflineRenderers,
		(CGLPixelFormatAttribute)0
	};
	CGLPixelFormatObj pix;
	CGLContextObj ctx;
	GLint npix;
	GLuint fbo, rb, prog, vs, fs;
	GLint ok, loc;
	float proj[16], view[16], mvp[16];
	float params[16];
	unsigned char *rgba, *grey;
	int i;

	if (argc < 5) {
		fprintf(stderr, "usage: aurarender <aura.iqm> <vp.glsl> <fp.glsl> <out.png> [size] [strip.raw]\n");
		return 2;
	}
	meshPath = argv[1];
	vpPath = argv[2];
	fpPath = argv[3];
	outPath = argv[4];
	if (argc > 5)
		size = atoi(argv[5]);
	stripPath = argc > 6 ? argv[6] : NULL;

	if (!LoadIQM(meshPath, &mesh))
		return 1;

	if (CGLChoosePixelFormat(attrs, &pix, &npix) != kCGLNoError || !pix) {
		fprintf(stderr, "aurarender: no pixel format\n");
		return 1;
	}
	if (CGLCreateContext(pix, NULL, &ctx) != kCGLNoError) {
		fprintf(stderr, "aurarender: no GL context\n");
		return 1;
	}
	CGLSetCurrentContext(ctx);

	glGenFramebuffersEXT(1, &fbo);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
	glGenRenderbuffersEXT(1, &rb);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, size, size);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
	                             GL_RENDERBUFFER_EXT, rb);
	if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
		fprintf(stderr, "aurarender: framebuffer incomplete\n");
		return 1;
	}

	vs = CompileStage(GL_VERTEX_SHADER, vpPath);
	fs = CompileStage(GL_FRAGMENT_SHADER, fpPath);
	if (!vs || !fs)
		return 1;
	prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[4096];

		glGetProgramInfoLog(prog, sizeof(log), NULL, log);
		fprintf(stderr, "aurarender: link failed:\n%s\n", log);
		return 1;
	}
	glUseProgram(prog);

	/* The engine's uniform contract, filled the way cg_auras.c fills it for
	   a standing player: force is gravity, so the flow is straight up. */
	MatPerspective(proj, CAM_FOV, 1.0f, 4.0f, 4096.0f);
	MatLookNorth(view, CAM_RANGE,
	             (boxMins[2] + boxMaxs[2]) * 0.5f);
	MatMul(mvp, proj, view);

	params[0] = 0.0f;  params[1] = 0.0f;  params[2] = -1.0f;
	params[3] = AURA_SCALE;
	params[4] = boxMins[0]; params[5] = boxMins[1]; params[6] = boxMins[2];
	params[7] = ORIGIN_DISTANCE;
	params[8] = boxMaxs[0]; params[9] = boxMaxs[1]; params[10] = boxMaxs[2];
	params[11] = PADDING;
	params[12] = AMPLITUDE;
	params[13] = WAVELENGTH;
	params[14] = SCROLL_SPEED;
	params[15] = 0.0f;

	loc = glGetUniformLocation(prog, "u_ProgramParams");
	glUniform4fv(loc, 4, params);
	loc = glGetUniformLocation(prog, "u_ModelViewProjectionMatrix");
	glUniformMatrix4fv(loc, 1, GL_FALSE, mvp);
	loc = glGetUniformLocation(prog, "u_ProjectionMatrix");
	glUniformMatrix4fv(loc, 1, GL_FALSE, proj);
	loc = glGetUniformLocation(prog, "u_Time");
	if (loc >= 0)
		glUniform1f(loc, 0.0f);
	loc = glGetUniformLocation(prog, "u_EntityColor");
	if (loc >= 0)
		glUniform4f(loc, 1.0f, 1.0f, 1.0f, 1.0f);

	if (stripPath) {
		FILE *sf = fopen(stripPath, "rb");
		unsigned int dims[2];
		unsigned char *texels;
		GLuint tex;

		if (!sf) {
			fprintf(stderr, "aurarender: cannot open %s\n", stripPath);
			return 1;
		}
		if (fread(dims, 4, 2, sf) != 2) {
			fclose(sf);
			return 1;
		}
		texels = malloc((unsigned long)dims[0] * dims[1] * 4);
		if (fread(texels, 4, (size_t)dims[0] * dims[1], sf)
		    != (size_t)dims[0] * dims[1]) {
			fclose(sf);
			return 1;
		}
		fclose(sf);

		glGenTextures(1, &tex);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex);
		/* clampmapT: S repeats around the ring, T clamps at the tips. */
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dims[0], dims[1], 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, texels);
		free(texels);

		loc = glGetUniformLocation(prog, "u_Texture0");
		if (loc >= 0)
			glUniform1i(loc, 0);
	}

	glViewport(0, 0, size, size);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	/* The stage's own blend: premultiplied over. Over a black clear the
	   red plane that comes back is the aura's luminance. */
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, mesh.positions);
	glEnableClientState(GL_NORMAL_ARRAY);
	glNormalPointer(GL_FLOAT, 0, mesh.normals);
	glEnableClientState(GL_COLOR_ARRAY);
	glColorPointer(4, GL_UNSIGNED_BYTE, 0, mesh.colors);
	glClientActiveTexture(GL_TEXTURE0);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, 0, mesh.texcoords);

	glDrawElements(GL_TRIANGLES, mesh.numTris * 3, GL_UNSIGNED_INT, mesh.tris);
	glFinish();

	rgba = malloc((unsigned long)size * size * 4);
	grey = malloc((unsigned long)size * size);
	glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

	/* GL reads bottom-up; the PNG wants top-down. */
	for (i = 0; i < size; i++) {
		int x;

		for (x = 0; x < size; x++)
			grey[(unsigned long)(size - 1 - i) * size + x] =
				rgba[((unsigned long)i * size + x) * 4 + 0];
	}
	if (!WriteGreyPNG(outPath, grey, size, size)) {
		fprintf(stderr, "aurarender: failed writing %s\n", outPath);
		return 1;
	}
	printf("wrote %s (%dx%d luminance)\n", outPath, size, size);
	return 0;
}
