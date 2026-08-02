/*
 * auragen.c
 * Procedural generator for the aura art: a full teardrop sprite and the
 * ring strip the aura shaders sample.
 *
 *   cc -O2 -o auragen auragen.c -lz
 *   ./auragen [outdir] [seed] [-p]
 *
 * Writes aura_sprite.png (1024x1280) and aura_strip.png (512x128) with
 * RGB solid white and the whole silhouette in alpha, which is what
 * aura_fp.glsl expects of the strip. Straight (non-premultiplied) alpha.
 *
 * Both images come out of one field function, in a polar-ish domain:
 * u runs around the perimeter with period 1, t runs outward with 0 at
 * the dense body's edge and 1 at the farthest a spike may reach.
 *
 *  - The licks are ridged fractal noise over u: folding the noise about
 *    its midline and squaring gives sharp peaks with rounded valleys,
 *    where plain fBm gives billows.
 *  - The filaments are high-frequency noise over u alone, held constant
 *    along t. Constancy along the radius is exactly what makes a noise
 *    value read as a strand; a slight t-dependent wiggle keeps the hairs
 *    from being ruler-straight.
 *  - The sprite's base shape reuses aura_vp.glsl's width profile
 *    (V_OPEN / V_CLOSE / V_NORM), so the drawn teardrop and the in-game
 *    ring agree by construction.
 *
 * All angular noise is lattice noise with integer periods, so every
 * octave tiles: the strip meets itself across the S wrap and the sprite
 * closes at the bottom of the ring with no seam.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define TAU 6.28318530717958647692f

/* ---------------------------------------------------------------------------
   PNG writing: 8-bit RGBA, one zlib stream, filter 0 on every scanline.
--------------------------------------------------------------------------- */

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

static int WritePNG(const char *path, const unsigned char *rgba, int w, int h) {
	static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	unsigned long rawLen = (unsigned long)h * (1 + (unsigned long)w * 4);
	unsigned char *raw = malloc(rawLen);
	uLongf zLen = compressBound(rawLen);
	unsigned char *z = malloc(zLen);
	unsigned char ihdr[13];
	FILE *f;
	int y;

	if (!raw || !z) {
		free(raw);
		free(z);
		return 0;
	}

	for (y = 0; y < h; y++) {
		unsigned char *row = raw + (unsigned long)y * (1 + (unsigned long)w * 4);
		row[0] = 0;
		memcpy(row + 1, rgba + (unsigned long)y * w * 4, (unsigned long)w * 4);
	}

	if (compress2(z, &zLen, raw, rawLen, 9) != Z_OK) {
		free(raw);
		free(z);
		return 0;
	}

	f = fopen(path, "wb");
	if (!f) {
		free(raw);
		free(z);
		return 0;
	}

	fwrite(sig, 1, 8, f);
	BE32(ihdr, (unsigned long)w);
	BE32(ihdr + 4, (unsigned long)h);
	ihdr[8]  = 8;	/* bit depth */
	ihdr[9]  = 6;	/* colour type: RGBA */
	ihdr[10] = 0;
	ihdr[11] = 0;
	ihdr[12] = 0;
	PngChunk(f, "IHDR", ihdr, 13);
	PngChunk(f, "IDAT", z, zLen);
	PngChunk(f, "IEND", NULL, 0);
	fclose(f);

	free(raw);
	free(z);
	return 1;
}

/* ---------------------------------------------------------------------------
   Noise: hashed lattice value noise, periodic over integer lattices so
   every octave tiles around the perimeter.
--------------------------------------------------------------------------- */

static unsigned HashU32(unsigned x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static float Hash01(int i, unsigned seed) {
	return HashU32((unsigned)i * 0x9E3779B9u ^ seed) * (1.0f / 4294967296.0f);
}

static float Lerp(float a, float b, float f) {
	return a + (b - a) * f;
}

static float SmoothStep(float e0, float e1, float x) {
	float t = (x - e0) / (e1 - e0);
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

/* u has period 1; the lattice has `period` cells. */
static float VNoise1P(float u, int period, unsigned seed) {
	float xf = u * period;
	int   i  = (int)floorf(xf);
	float f  = xf - i;
	int   i0 = ((i % period) + period) % period;
	int   i1 = (i0 + 1) % period;

	f = f * f * (3.0f - 2.0f * f);
	return Lerp(Hash01(i0, seed), Hash01(i1, seed), f);
}

/* Periodic over u, unbounded over y. */
static float VNoise2P(float u, int period, float y, unsigned seed) {
	float xf = u * period;
	int   xi = (int)floorf(xf);
	float fx = xf - xi;
	int   x0 = ((xi % period) + period) % period;
	int   x1 = (x0 + 1) % period;
	int   yi = (int)floorf(y);
	float fy = y - yi;
	unsigned s0 = seed ^ (unsigned)yi * 0x27d4eb2fu;
	unsigned s1 = seed ^ (unsigned)(yi + 1) * 0x27d4eb2fu;
	float n0, n1;

	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	n0 = Lerp(Hash01(x0, s0), Hash01(x1, s0), fx);
	n1 = Lerp(Hash01(x0, s1), Hash01(x1, s1), fx);
	return Lerp(n0, n1, fy);
}

static float RidgeFbm1(float u, int freq, int octaves, unsigned seed) {
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	int o;

	for (o = 0; o < octaves; o++) {
		float n = VNoise1P(u, freq, seed + (unsigned)o * 0x9E37u);
		float r = 1.0f - fabsf(2.0f * n - 1.0f);
		sum  += r * r * amp;
		norm += amp;
		amp  *= 0.55f;
		freq *= 2;
	}
	return sum / norm;
}

/* ---------------------------------------------------------------------------
   The shared field.
--------------------------------------------------------------------------- */

typedef struct {
	int      macroFreq;	/* licks per wrap */
	int      macroOct;
	int      hairFreq;	/* filaments per wrap */
	float    tipBase;	/* fringe length floor, in t units */
	float    tipAmp;	/* lick length on top of the floor */
	float    hairAmp;	/* per-filament length jitter */
	float    soft;		/* tip edge softness, in t units */
	float    lean;		/* angular shear per unit t, in wraps */
	unsigned seed;
} fieldParams;

/* Alpha at (u, t). `side` is +1/-1 and flips the lean so both flanks
   sweep toward the tip rather than the whole ring shearing one way. */
static float StrandAlpha(const fieldParams *p, float u, float t, float side) {
	float m, hn, tip, a, g, sep;

	u += p->lean * t * side;
	u += (VNoise2P(u, 48, t * 5.0f, p->seed ^ 0x77u) - 0.5f) * 0.006f * t;

	m = RidgeFbm1(u, p->macroFreq, p->macroOct, p->seed);
	/* Contrast, not gamma: pushing the valleys down and the peaks up is
	   what separates the licks; a power curve just shortens everything. */
	m = SmoothStep(0.22f, 0.92f, m);
	hn  = VNoise1P(u, p->hairFreq, p->seed ^ 0x1234u);
	tip = p->tipBase + p->tipAmp * m + p->hairAmp * (hn - 0.5f);

	a = 1.0f - SmoothStep(tip - p->soft, tip, t);

	/* Strands separate as they leave the body: near t = 0 the fringe is a
	   solid sheet, and only past that do the gaps between hairs open. */
	g   = VNoise1P(u, p->hairFreq, p->seed ^ 0xBEEFu);
	sep = 0.30f + 0.70f * SmoothStep(0.15f, 0.85f, g);
	a  *= Lerp(1.0f, sep, SmoothStep(0.05f, 0.5f, t));

	/* Each strand thins toward its own tip. */
	a *= Lerp(1.0f, 0.55f, SmoothStep(0.0f, tip > 0.2f ? tip : 0.2f, t));

	return a;
}

/* ---------------------------------------------------------------------------
   The teardrop sprite.
--------------------------------------------------------------------------- */

#define SPR_W 1024
#define SPR_H 1280

/* aura_vp.glsl's width profile: h is 0 at the base apex, 1 at the crown. */
#define V_OPEN  0.35f
#define V_CLOSE 4.0f
#define V_NORM  1.36f

/* Body extents inside the image, leaving room for the spikes. */
#define BODY_TOP    310.0f
#define BODY_BOTTOM 1050.0f
#define BODY_HALFW  265.0f

/* Polar centre height, as a fraction of the body: near the widest point,
   so the shape is star-shaped about it and one radius per angle exists. */
#define CENTRE_H 0.45f

/* Spike reach as a fraction of the local body radius. Licks carry most of
   their length at the crown, keep presence at the flanks, and get a small
   burst back at the base where the reference gathers its ground splash. */
#define SPAN_MIN  0.42f
#define SPAN_UP   0.40f
#define SPAN_DOWN 0.15f

/* Core translucency: the reference is about half-opaque at the centre and
   only reaches solid white against the rim. */
#define CORE_ALPHA 0.45f

#define RTABLE_SIZE 4096

static float rtable[RTABLE_SIZE];

static float Profile(float h) {
	return powf(h, V_OPEN) * (1.0f - powf(h, V_CLOSE)) * V_NORM;
}

static int InsideBody(float x, float y) {
	float h = (BODY_BOTTOM - y) / (BODY_BOTTOM - BODY_TOP);

	if (h <= 0.0f || h >= 1.0f)
		return 0;
	return fabsf(x - SPR_W * 0.5f) <= BODY_HALFW * Profile(h);
}

static void BuildRTable(void) {
	float cx = SPR_W * 0.5f;
	float cy = BODY_BOTTOM - CENTRE_H * (BODY_BOTTOM - BODY_TOP);
	int k, i;

	for (k = 0; k < RTABLE_SIZE; k++) {
		float theta = ((float)k / RTABLE_SIZE - 0.5f) * TAU;
		float dx = sinf(theta), dy = -cosf(theta);
		float lo = 0.0f, hi = 8.0f;

		/* March out to bracket the boundary, then bisect. */
		while (hi < 620.0f && InsideBody(cx + dx * hi, cy + dy * hi)) {
			lo = hi;
			hi += 8.0f;
		}
		for (i = 0; i < 24; i++) {
			float mid = (lo + hi) * 0.5f;
			if (InsideBody(cx + dx * mid, cy + dy * mid))
				lo = mid;
			else
				hi = mid;
		}
		rtable[k] = (lo + hi) * 0.5f;
	}
}

static float RTableLookup(float u) {
	float xf = u * RTABLE_SIZE;
	int   i  = (int)floorf(xf);
	float f  = xf - i;
	int   i0 = ((i % RTABLE_SIZE) + RTABLE_SIZE) % RTABLE_SIZE;
	int   i1 = (i0 + 1) % RTABLE_SIZE;

	return Lerp(rtable[i0], rtable[i1], f);
}

static float SpriteAlpha(const fieldParams *p, float x, float y) {
	float cx = SPR_W * 0.5f;
	float cy = BODY_BOTTOM - CENTRE_H * (BODY_BOTTOM - BODY_TOP);
	float dx = x - cx, dy = y - cy;
	float r  = sqrtf(dx * dx + dy * dy);
	float theta, u, upness, rb, span, t;

	if (r < 1.0f)
		return CORE_ALPHA;

	theta  = atan2f(dx, -dy);
	u      = theta / TAU + 0.5f;
	upness = 0.5f * (1.0f - dy / r);
	rb     = RTableLookup(u);

	/* The white band hugs the rim: a slow climb through the body, then a
	   fast one over the last stretch before the boundary. */
	if (r < rb) {
		float rho = r / rb;
		return CORE_ALPHA + 0.30f * SmoothStep(0.15f, 0.85f, rho)
		                  + (1.0f - CORE_ALPHA - 0.30f) * SmoothStep(0.85f, 1.0f, rho);
	}

	span = SPAN_MIN + SPAN_UP * upness
	     + SPAN_DOWN * powf(1.0f - upness, 4.0f);
	t = (r - rb) / (rb * span);

	return StrandAlpha(p, u, t, sinf(theta));
}

static void RenderSprite(unsigned char *rgba, const fieldParams *p) {
	int x, y;

	BuildRTable();
	for (y = 0; y < SPR_H; y++) {
		for (x = 0; x < SPR_W; x++) {
			unsigned char *px = rgba + ((unsigned long)y * SPR_W + x) * 4;
			float a = 0.25f * (SpriteAlpha(p, x + 0.25f, y + 0.25f)
			                 + SpriteAlpha(p, x + 0.75f, y + 0.25f)
			                 + SpriteAlpha(p, x + 0.25f, y + 0.75f)
			                 + SpriteAlpha(p, x + 0.75f, y + 0.75f));
			px[0] = px[1] = px[2] = 255;
			px[3] = (unsigned char)(a * 255.0f + 0.5f);
		}
	}
}

/* ---------------------------------------------------------------------------
   The ring strip: S repeats around the ring, T runs body -> tips, and the
   last rows are forced transparent because the stage samples it with
   clampmapT and past-the-end reads must return empty tips.
--------------------------------------------------------------------------- */

#define STRIP_W 512
#define STRIP_H 128

#define STRIP_BODY_V 0.12f
#define STRIP_SPAN_V 0.85f

static void RenderStrip(unsigned char *rgba, const fieldParams *p) {
	int x, y;

	for (y = 0; y < STRIP_H; y++) {
		for (x = 0; x < STRIP_W; x++) {
			unsigned char *px = rgba + ((unsigned long)y * STRIP_W + x) * 4;
			float u = (x + 0.5f) / STRIP_W;
			float v = (y + 0.5f) / STRIP_H;
			float t = (v - STRIP_BODY_V) / STRIP_SPAN_V;
			float a = StrandAlpha(p, u, t, 0.0f);

			a *= 1.0f - SmoothStep(0.96f, 1.0f, v);
			px[0] = px[1] = px[2] = 255;
			px[3] = (unsigned char)(a * 255.0f + 0.5f);
		}
	}
}

/* ---------------------------------------------------------------------------
   Preview: the shipped art is white-on-alpha and invisible in any viewer
   with a light background, so -p also writes copies composited over a
   dark checker, the way an image editor would show them.
--------------------------------------------------------------------------- */

static int WritePreview(const char *path, const unsigned char *rgba, int w, int h) {
	unsigned char *out = malloc((unsigned long)w * h * 4);
	int x, y, ok;

	if (!out)
		return 0;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			const unsigned char *src = rgba + ((unsigned long)y * w + x) * 4;
			unsigned char *dst = out + ((unsigned long)y * w + x) * 4;
			int bg = ((x / 16 + y / 16) & 1) ? 64 : 40;
			int c;

			for (c = 0; c < 3; c++)
				dst[c] = (unsigned char)((src[c] * src[3] + bg * (255 - src[3])) / 255);
			dst[3] = 255;
		}
	}
	ok = WritePNG(path, out, w, h);
	free(out);
	return ok;
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
	const char *outdir = ".";
	unsigned seed = 0x5EED5u;
	int preview = 0, npos = 0, i;
	char path[1024];
	unsigned char *img;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-p"))
			preview = 1;
		else if (npos == 0)
			outdir = argv[i], npos++;
		else
			seed = (unsigned)strtoul(argv[i], NULL, 0);
	}

	fieldParams sprite = { 30, 4, 640, 0.07f, 0.74f, 0.20f, 0.030f, 0.030f, 0 };
	fieldParams strip  = {  8, 4, 150, 0.22f, 0.55f, 0.25f, 0.050f, 0.0f,   0 };

	sprite.seed = seed;
	strip.seed  = seed;

	img = malloc((unsigned long)SPR_W * SPR_H * 4);
	if (!img) {
		fprintf(stderr, "auragen: out of memory\n");
		return 1;
	}

	RenderSprite(img, &sprite);
	snprintf(path, sizeof(path), "%s/aura_sprite.png", outdir);
	if (!WritePNG(path, img, SPR_W, SPR_H)) {
		fprintf(stderr, "auragen: failed writing %s\n", path);
		return 1;
	}
	printf("wrote %s (%dx%d)\n", path, SPR_W, SPR_H);
	if (preview) {
		snprintf(path, sizeof(path), "%s/aura_sprite_preview.png", outdir);
		WritePreview(path, img, SPR_W, SPR_H);
	}

	RenderStrip(img, &strip);
	snprintf(path, sizeof(path), "%s/aura_strip.png", outdir);
	if (!WritePNG(path, img, STRIP_W, STRIP_H)) {
		fprintf(stderr, "auragen: failed writing %s\n", path);
		return 1;
	}
	printf("wrote %s (%dx%d)\n", path, STRIP_W, STRIP_H);
	if (preview) {
		snprintf(path, sizeof(path), "%s/aura_strip_preview.png", outdir);
		WritePreview(path, img, STRIP_W, STRIP_H);
	}

	free(img);
	return 0;
}
