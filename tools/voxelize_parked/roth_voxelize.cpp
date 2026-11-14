//
// The close-up voxelizer. See roth_voxelize.h for what and why, and
// VOXELIZER.md for the measurements over the retail films.
//
#include "roth_voxelize.h"
#include "roth_gdv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace roth
{

namespace
{

const double PI = 3.14159265358979323846;

inline double Deg(double d) { return d * PI / 180.0; }

uint32_t U32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

} // namespace

//==========================================================================
//
// The film.
//
//==========================================================================

bool VoxCloseupBytes(const uint8_t *dbase300, size_t size, uint32_t closeup, const uint8_t *&gdv, size_t &gdvSize)
{
	const uint64_t at = (uint64_t)closeup * 8;
	if (closeup == 0 || at + 4 > size) return false;
	const uint32_t len = U32(dbase300 + at);
	gdv = dbase300 + at + 4;
	gdvSize = (size_t)std::min<uint64_t>(len, size - at - 4);
	return gdvSize > 0x18;
}

bool VoxDecodeFilm(const uint8_t *data, size_t size, VoxFilm &out, std::string *error)
{
	out = VoxFilm();
	GdvDecoder d;
	if (!d.Open(data, size))
	{
		if (error) *error = "gdv: " + d.Error();
		return false;
	}
	if (d.Version() != 1)
	{
		if (error) *error = "not an 8-bit film";
		return false;
	}
	out.width = d.Width();
	out.height = d.Height();
	std::vector<uint8_t> pic;
	uint8_t first[768] = {};
	while (d.NextFrame())
	{
		d.ExpandPicture(pic);
		if ((int)pic.size() != out.width * out.height) break;
		if (out.frames == 0) memcpy(first, d.Dac(), 768);
		else if (memcmp(first, d.Dac(), 768) != 0) out.paletteChanged = true;
		out.pixels.insert(out.pixels.end(), pic.begin(), pic.end());
		out.frames++;
	}
	if (out.frames == 0)
	{
		if (error) *error = "no frames" + (d.Error().empty() ? std::string() : ": " + d.Error());
		return false;
	}
	for (int i = 0; i < 768; i++) out.palette[i] = (uint8_t)((first[i] << 2) | (first[i] >> 4));

	// The ground: the commonest index on the picture's border over all frames,
	// which must be black.
	size_t hist[256] = {}, total = 0;
	const int w = out.width, h = out.height;
	for (int k = 0; k < out.frames; k++)
	{
		const uint8_t *f = out.Frame(k);
		for (int x = 0; x < w; x++) { hist[f[x]]++; hist[f[(h - 1) * w + x]]++; total += 2; }
		for (int y = 1; y < h - 1; y++) { hist[f[y * w]]++; hist[f[y * w + w - 1]]++; total += 2; }
	}
	int best = 0;
	for (int i = 1; i < 256; i++) if (hist[i] > hist[best]) best = i;
	double share = 0;
	bool dark = true;
	for (int i = 0; i < 256 && total; i++)
	{
		if ((double)hist[i] < 0.10 * (double)total) continue;
		out.groundSet[i] = true;
		share += (double)hist[i] / (double)total;
		const uint8_t *c = out.palette + i * 3;
		if (c[0] + c[1] + c[2] > 96) dark = false;
	}
	out.groundBorderShare = share;
	if (dark && share >= 0.6) out.ground = best;
	else memset(out.groundSet, 0, sizeof out.groundSet);
	return true;
}

void VoxBuildMasks(const VoxFilm &film, std::vector<uint8_t> &masks, int minSpeck)
{
	const int w = film.width, h = film.height, n = film.frames;
	masks.assign((size_t)w * h * n, 0);
	if (film.ground < 0) return;
	std::vector<int> stack, comp;
	for (int k = 0; k < n; k++)
	{
		const uint8_t *f = film.Frame(k);
		uint8_t *m = masks.data() + (size_t)k * w * h;
		for (int i = 0; i < w * h; i++) m[i] = !film.groundSet[f[i]];
		if (minSpeck <= 0) continue;
		// Drop specks: 8-connected parts of minSpeck pixels or fewer.
		std::vector<uint8_t> seen(m, m + w * h);
		for (int i = 0; i < w * h; i++)
		{
			if (seen[i] != 1) continue;
			comp.clear(); stack.assign(1, i); seen[i] = 2;
			while (!stack.empty())
			{
				const int p = stack.back(); stack.pop_back(); comp.push_back(p);
				const int px = p % w, py = p / w;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++)
					{
						const int qx = px + dx, qy = py + dy;
						if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
						const int q = qy * w + qx;
						if (seen[q] == 1) { seen[q] = 2; stack.push_back(q); }
					}
			}
			if ((int)comp.size() <= minSpeck) for (int p : comp) m[p] = 0;
		}
	}
}

//==========================================================================
//
// The camera.
//
//==========================================================================

double VoxCamera::Angle(int k) const
{
	const double off = k >= 0 && k < (int)angleOffset.size() ? angleOffset[k] : 0.0;
	return direction * 2.0 * PI * (double)(k - firstFrame) / period + off;
}

void VoxCamera::Project(int k, double x, double y, double z, double &u, double &v, double &depth) const
{
	const double th = Angle(k), c = cos(th), s = sin(th);
	const double xr = x * c - z * s, zr = x * s + z * c;
	const double cp = cos(elevation), sp = sin(elevation);
	double up = y * cp - zr * sp;
	depth = y * sp + zr * cp;
	double right = xr;
	if (invDistance != 0)
	{
		const double sc = 1.0 / std::max(0.05, 1.0 - depth * invDistance);
		right *= sc; up *= sc;
	}
	const double cr = cos(roll), sr = sin(roll);
	u = axisX + right * cr - up * sr;
	v = axisY - (right * sr + up * cr);
}

void VoxCamera::ViewDir(int k, double d[3]) const
{
	const double th = Angle(k), c = cos(th), s = sin(th);
	const double cp = cos(elevation), sp = sin(elevation);
	d[0] = s * cp; d[1] = sp; d[2] = c * cp;
}

namespace
{

// A camera's projection at one frame, as linear coefficients (plus the
// perspective divide), for the inner loops.
struct FrameProj
{
	double rx[3], up[3], dp[3];     // right, up, depth: dot with (x, y, z)
	double invD, cr, sr, ax, ay;
	void Set(const VoxCamera &cam, int k)
	{
		const double th = cam.Angle(k), c = cos(th), s = sin(th);
		const double cp = cos(cam.elevation), sp = sin(cam.elevation);
		// xr = x c - z s; zr = x s + z c
		rx[0] = c; rx[1] = 0; rx[2] = -s;
		up[0] = -s * sp; up[1] = cp; up[2] = -c * sp;
		dp[0] = s * cp; dp[1] = sp; dp[2] = c * cp;
		invD = cam.invDistance; cr = cos(cam.roll); sr = sin(cam.roll); ax = cam.axisX; ay = cam.axisY;
	}
	inline void Map(double x, double y, double z, double &u, double &v, double &d) const
	{
		double r = rx[0] * x + rx[2] * z, p = up[0] * x + up[1] * y + up[2] * z;
		d = dp[0] * x + dp[1] * y + dp[2] * z;
		if (invD != 0) { const double sc = 1.0 / std::max(0.05, 1.0 - d * invD); r *= sc; p *= sc; }
		u = ax + r * cr - p * sr;
		v = ay - (r * sr + p * cr);
	}
};

// A voxel grid in the object frame.
struct Grid
{
	int nx = 0, ny = 0, nz = 0;
	double vs = 1, x0 = 0, y0 = 0, z0 = 0;
	size_t Size() const { return (size_t)nx * ny * nz; }
	size_t Index(int x, int y, int z) const { return (size_t)x + (size_t)nx * ((size_t)y + (size_t)ny * (size_t)z); }
	double Cx(int x) const { return x0 + (x + 0.5) * vs; }
	double Cy(int y) const { return y0 + (y + 0.5) * vs; }
	double Cz(int z) const { return z0 + (z + 0.5) * vs; }
};

// The masks at one scale: each cell is set when any of the f x f film pixels
// under it is (max pooling keeps thin parts at a coarse scale).
struct Level
{
	int f = 1, w = 0, h = 0, n = 0;
	std::vector<uint8_t> m;
	const uint8_t *Frame(int k) const { return m.data() + (size_t)k * w * h; }
};

Level MakeLevel(const std::vector<uint8_t> &masks, int fw, int fh, int n, int f)
{
	Level L;
	L.f = f; L.n = n; L.w = (fw + f - 1) / f; L.h = (fh + f - 1) / f;
	L.m.assign((size_t)L.w * L.h * n, 0);
	for (int k = 0; k < n; k++)
	{
		const uint8_t *src = masks.data() + (size_t)k * fw * fh;
		uint8_t *dst = L.m.data() + (size_t)k * L.w * L.h;
		for (int y = 0; y < fh; y++)
			for (int x = 0; x < fw; x++)
				if (src[y * fw + x]) dst[(y / f) * L.w + x / f] = 1;
	}
	return L;
}

struct Bounds { int u0, u1, v0, v1; bool any; };    // mask bbox over the turn, in film pixels (u1/v1 exclusive)

Bounds MaskBounds(const std::vector<uint8_t> &masks, int w, int h, int k0, int k1)
{
	Bounds b{ w, 0, h, 0, false };
	for (int k = k0; k <= k1; k++)
	{
		const uint8_t *m = masks.data() + (size_t)k * w * h;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				if (m[y * w + x])
				{
					b.any = true;
					b.u0 = std::min(b.u0, x); b.u1 = std::max(b.u1, x + 1);
					b.v0 = std::min(b.v0, y); b.v1 = std::max(b.v1, y + 1);
				}
	}
	return b;
}

// A grid that holds everything the camera could have seen, at voxel size vs.
Grid GridFor(const VoxCamera &cam, const Bounds &b, double vs)
{
	Grid g;
	g.vs = vs;
	double R = std::max(cam.axisX - b.u0, b.u1 - cam.axisX) + vs;
	const double tilt = fabs(sin(cam.roll)) * (b.v1 - b.v0);
	R += tilt;
	if (cam.invDistance > 0) R *= 1.0 / std::max(0.3, 1.0 - R * cam.invDistance);
	R = std::max(R, 2 * vs);
	const double cp = std::max(0.2, cos(cam.elevation)), sp = fabs(sin(cam.elevation));
	double ylo = (cam.axisY - b.v1 - R * sp - tilt) / cp - vs, yhi = (cam.axisY - b.v0 + R * sp + tilt) / cp + vs;
	if (cam.invDistance > 0) { const double m = 1.0 / std::max(0.3, 1.0 - R * cam.invDistance); ylo *= m; yhi *= m; }
	g.nx = g.nz = 2 * (int)ceil(R / vs);
	g.x0 = g.z0 = -g.nx * vs / 2;
	g.y0 = floor(ylo / vs) * vs;
	g.ny = std::max(1, (int)ceil((yhi - g.y0) / vs));
	return g;
}

// Carve: solid[i] = 1 when the voxel's centre falls inside the mask of all
// but `tol` frames of the turn. Masks at scale f (cells of f film pixels).
void Carve(const Grid &g, const VoxCamera &cam, const uint8_t *masks, int mw, int mh, double f, int tol,
	std::vector<uint8_t> &solid)
{
	const size_t N = g.Size();
	std::vector<uint8_t> miss(N, 0);
	const int k0 = cam.firstFrame, k1 = cam.lastFrame;
	const double inv = 1.0 / f;
	for (int k = k0; k <= k1; k++)
	{
		FrameProj P; P.Set(cam, k);
		const uint8_t *m = masks + (size_t)k * mw * mh;
		for (int z = 0; z < g.nz; z++)
		{
			const double cz = g.Cz(z);
			for (int y = 0; y < g.ny; y++)
			{
				const double cy = g.Cy(y);
				uint8_t *row = miss.data() + g.Index(0, y, z);
				for (int x = 0; x < g.nx; x++)
				{
					if (row[x] > tol) continue;
					double u, v, d;
					P.Map(g.Cx(x), cy, cz, u, v, d);
					const int iu = (int)floor(u * inv), iv = (int)floor(v * inv);
					if (iu < 0 || iv < 0 || iu >= mw || iv >= mh || !m[iv * mw + iu]) row[x]++;
				}
			}
		}
	}
	solid.resize(N);
	for (size_t i = 0; i < N; i++) solid[i] = miss[i] <= tol;
}

bool IsSurface(const Grid &g, const std::vector<uint8_t> &s, int x, int y, int z)
{
	if (!s[g.Index(x, y, z)]) return false;
	if (x == 0 || y == 0 || z == 0 || x == g.nx - 1 || y == g.ny - 1 || z == g.nz - 1) return true;
	return !s[g.Index(x - 1, y, z)] || !s[g.Index(x + 1, y, z)] || !s[g.Index(x, y - 1, z)] ||
		!s[g.Index(x, y + 1, z)] || !s[g.Index(x, y, z - 1)] || !s[g.Index(x, y, z + 1)];
}

struct Pt { int x, y, z; };

std::vector<Pt> SurfaceList(const Grid &g, const std::vector<uint8_t> &s)
{
	std::vector<Pt> out;
	for (int z = 0; z < g.nz; z++)
		for (int y = 0; y < g.ny; y++)
			for (int x = 0; x < g.nx; x++)
				if (IsSurface(g, s, x, y, z)) out.push_back({ x, y, z });
	return out;
}

// Mean IoU over the turn between the masks (scale f) and the hull's
// projection (surface voxels splatted at 2 x 2 x 2 sub-points).
double ReprojectIoU(const Grid &g, const std::vector<Pt> &surf, const VoxCamera &cam, const uint8_t *masks,
	int mw, int mh, double f, std::vector<double> *perFrame)
{
	const int k0 = cam.firstFrame, k1 = cam.lastFrame;
	std::vector<uint8_t> img((size_t)mw * mh);
	double sum = 0;
	int count = 0;
	const double inv = 1.0 / f;
	const int sub = std::max(2, (int)ceil(g.vs * inv * 1.5));
	for (int k = k0; k <= k1; k++)
	{
		FrameProj P; P.Set(cam, k);
		std::fill(img.begin(), img.end(), 0);
		for (const Pt &p : surf)
		{
			for (int a = 0; a < sub; a++)
				for (int b = 0; b < sub; b++)
					for (int c = 0; c < sub; c++)
					{
						const double x = g.x0 + (p.x + (a + 0.5) / sub) * g.vs;
						const double y = g.y0 + (p.y + (b + 0.5) / sub) * g.vs;
						const double z = g.z0 + (p.z + (c + 0.5) / sub) * g.vs;
						double u, v, d;
						P.Map(x, y, z, u, v, d);
						const int iu = (int)floor(u * inv), iv = (int)floor(v * inv);
						if (iu >= 0 && iv >= 0 && iu < mw && iv < mh) img[iv * mw + iu] = 1;
					}
		}
		const uint8_t *m = masks + (size_t)k * mw * mh;
		size_t I = 0, U = 0;
		for (size_t i = 0; i < img.size(); i++) { I += img[i] & m[i]; U += img[i] | m[i]; }
		const double iou = U ? (double)I / (double)U : 0;
		if (perFrame) (*perFrame)[k] = iou;
		sum += iou; count++;
	}
	return count ? sum / count : 0;
}

double Score(const Level &L, const Bounds &b, const VoxCamera &cam, int *evals, int tol = 0, std::vector<double> *perFrame = nullptr)
{
	if (evals) (*evals)++;
	const Grid g = GridFor(cam, b, L.f);
	if (g.Size() > 40u * 1000 * 1000) return 0;
	std::vector<uint8_t> s;
	Carve(g, cam, L.m.data(), L.w, L.h, L.f, tol, s);
	const std::vector<Pt> surf = SurfaceList(g, s);
	if (surf.empty()) return 0;
	return ReprojectIoU(g, surf, cam, L.m.data(), L.w, L.h, L.f, perFrame);
}

// The axis column from the horizontal extents: under the turntable model the
// right edge at angle t and the left edge at t + 180 degrees are mirror
// images about the axis, whatever the elevation (the horizontal screen
// coordinate does not depend on it). Median over the turn.
double AxisFromExtents(const std::vector<uint8_t> &masks, int w, int h, int k0, int k1, double period)
{
	const int n = k1 - k0 + 1;
	std::vector<double> L(n, -1), R(n, -1);
	for (int k = 0; k < n; k++)
	{
		const uint8_t *m = masks.data() + (size_t)(k0 + k) * w * h;
		int lo = w, hi = -1;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				if (m[y * w + x]) { lo = std::min(lo, x); hi = std::max(hi, x); }
		if (hi >= 0) { L[k] = lo; R[k] = hi + 1; }
	}
	std::vector<double> est;
	for (int k = 0; k < n; k++)
	{
		if (L[k] < 0) continue;
		// R at k + period/2, interpolated, within the turn.
		const double t = fmod(k + period / 2.0, period);
		const int a = (int)floor(t), b2 = a + 1;
		const double fr = t - a;
		auto Rat = [&](int i) -> double { i = ((i % (int)ceil(period)) + (int)ceil(period)) % (int)ceil(period); return i < n ? R[i] : -1; };
		const double ra = Rat(a), rb = Rat(b2);
		if (ra < 0 || (fr > 0 && rb < 0)) continue;
		const double r = fr > 0 ? ra * (1 - fr) + rb * fr : ra;
		est.push_back((L[k] + r) / 2.0);
	}
	if (est.empty()) return w / 2.0;
	std::nth_element(est.begin(), est.begin() + est.size() / 2, est.end());
	return est[est.size() / 2];
}

// RGB spread of the surface across the frames that see it: the photo-
// consistency used to break silhouette ties.
double PhotoError(const VoxFilm &film, const std::vector<uint8_t> &masks, const VoxCamera &cam, double vs);

} // namespace

//==========================================================================
//
// The estimate.
//
//==========================================================================

VoxGeometry VoxEstimateTurntable(const VoxFilm &film, const std::vector<uint8_t> &masks, const VoxEstimateOptions &opt)
{
	VoxGeometry G;
	const int w = film.width, h = film.height;
	if (film.ground < 0) { G.note = "not an object on a black ground"; return G; }

	// The turn: the frames with something in them.
	int k0 = -1, k1 = -1;
	for (int k = 0; k < film.frames; k++)
	{
		const uint8_t *m = masks.data() + (size_t)k * w * h;
		bool any = false;
		for (int i = 0; i < w * h && !any; i++) any = m[i] != 0;
		if (any) { if (k0 < 0) k0 = k; k1 = k; }
	}
	if (k0 < 0) { G.note = "empty film"; return G; }
	{
		size_t on = 0;
		for (size_t i = 0; i < masks.size(); i++) on += masks[i];
		if ((double)on > 0.9 * (double)masks.size()) { G.note = "the mask fills the picture: no ground found"; return G; }
	}
	const int n = k1 - k0 + 1;
	if (n < 4) { G.note = "too few frames"; return G; }
	const Bounds b = MaskBounds(masks, w, h, k0, k1);

	const int longest = std::max(w, h);
	const Level coarse = MakeLevel(masks, w, h, film.frames, std::max(1, (longest + opt.coarseCells - 1) / opt.coarseCells));
	const Level fine = MakeLevel(masks, w, h, film.frames, std::max(1, (longest + opt.fineCells - 1) / opt.fineCells));

	// Candidate periods: the turn's frame count; one less (the last frame
	// repeats the first); double (a half turn); half (two turns).
	std::vector<double> periods = { (double)n, (double)(n - 1), 2.0 * n, 2.0 * (n - 1) };
	if (n % 2 == 0 && n >= 8) periods.push_back(n / 2.0);

	struct Cand { double score; VoxCamera cam; };
	std::vector<Cand> best;     // the best per (period, direction)
	for (double P : periods)
		for (int dir = 1; dir >= -1; dir -= 2)
		{
			VoxCamera cam;
			cam.period = P; cam.direction = dir; cam.firstFrame = k0; cam.lastFrame = k1;
			cam.axisY = h / 2.0;
			const double ax0 = AxisFromExtents(masks, w, h, k0, k1, P);
			Cand c{ -1, cam };
			for (double e = -5; e <= 75.01; e += 5)
				for (int da = -2; da <= 2; da++)
				{
					cam.elevation = Deg(e);
					cam.axisX = ax0 + da * coarse.f * 0.5;
					const double s = Score(coarse, b, cam, &G.evaluations);
					if (s > c.score) { c.score = s; c.cam = cam; }
				}
			best.push_back(c);
		}
	std::sort(best.begin(), best.end(), [](const Cand &a, const Cand &b2) { return a.score > b2.score; });

	// Refine the leaders at the fine scale: coordinate descent with shrinking
	// steps over elevation, axis column, roll and perspective.
	auto Refine = [&](VoxCamera cam) -> Cand
	{
		double s = Score(fine, b, cam, &G.evaluations);
		double stepE = Deg(4), stepA = fine.f * 1.0, stepR = Deg(1.0), stepP = 1.0 / (8.0 * longest);
		for (int round = 0; round < 4; round++)
		{
			bool moved = true;
			for (int it = 0; it < 6 && moved; it++)
			{
				moved = false;
				auto Try = [&](VoxCamera c2)
				{
					if (c2.elevation < Deg(-8) || c2.elevation > Deg(80) || c2.invDistance < 0 || c2.invDistance > 1.0 / longest) return;
					const double s2 = Score(fine, b, c2, &G.evaluations);
					if (s2 > s + 1e-5) { s = s2; cam = c2; moved = true; }
				};
				for (int sg = -1; sg <= 1; sg += 2)
				{
					VoxCamera c2 = cam; c2.elevation += sg * stepE; Try(c2);
					c2 = cam; c2.axisX += sg * stepA; Try(c2);
					if (opt.tryRoll) { c2 = cam; c2.roll += sg * stepR; Try(c2); }
					if (opt.tryPerspective) { c2 = cam; c2.invDistance += sg * stepP; Try(c2); }
					if (opt.tryPerspective && cam.invDistance > 0) { c2 = cam; c2.axisY += sg * stepA * 4; Try(c2); }
				}
			}
			stepE /= 2; stepA /= 2; stepR /= 2; stepP /= 2;
		}
		return Cand{ s, cam };
	};

	std::vector<Cand> refined;
	for (size_t i = 0; i < best.size() && i < 4; i++)
	{
		if (i >= 2 && best[i].score < best[0].score - 0.02) break;
		refined.push_back(Refine(best[i].cam));
	}
	std::sort(refined.begin(), refined.end(), [](const Cand &a, const Cand &b2) { return a.score > b2.score; });

	// Ties (a level camera's direction, a round item's period): the most
	// photo-consistent of those within 0.01.
	size_t pick = 0;
	{
		double bestPhoto = 1e9;
		for (size_t i = 0; i < refined.size(); i++)
		{
			if (refined[i].score < refined[0].score - 0.01) continue;
			const double pe = PhotoError(film, masks, refined[i].cam, std::max(1.0, longest / 100.0));
			if (i == 0) G.photoError = pe;
			if (pe < bestPhoto) { bestPhoto = pe; pick = i; G.photoError = pe; }
		}
	}
	G.camera = refined[pick].cam;
	G.meanIoU = refined[pick].score;
	for (size_t i = 0; i < refined.size(); i++)
		if (i != pick)
		{
			G.runnerUpIoU = refined[i].score;
			char t[96];
			snprintf(t, sizeof t, "P=%.0f dir=%+d elev=%.1f", refined[i].cam.period, refined[i].cam.direction, refined[i].cam.elevation * 180 / PI);
			G.runnerUp = t;
			break;
		}
	if (pick != 0) G.note += "picked by photo-consistency over a silhouette tie; ";

	// How well the elevation is pinned: where the score stays within 0.01.
	{
		const double e0 = G.camera.elevation;
		G.elevationLow = G.elevationHigh = e0;
		for (int sg = -1; sg <= 1; sg += 2)
			for (double d = 1; d <= 30; d += 1)
			{
				VoxCamera c2 = G.camera; c2.elevation = e0 + sg * Deg(d);
				if (c2.elevation < Deg(-8) || c2.elevation > Deg(80)) break;
				if (Score(fine, b, c2, &G.evaluations) < G.meanIoU - 0.01) break;
				if (sg < 0) G.elevationLow = c2.elevation; else G.elevationHigh = c2.elevation;
			}
	}
	// An elevation the silhouettes leave open: round items cast nearly the
	// same outline at every elevation (the Orb scores 0.970..0.986 from -5 to
	// 80 degrees), and the photo-consistency of their baked, glassy or
	// faceted surfaces was measured flat too (within 3% over the range for
	// the Orb, the Vial, the Gem and the Lamp). Open = the elevations within
	// 0.02 of the best score span more than 15 degrees. Those films take the
	// elevation the films whose silhouettes DO pin it settle on: the median
	// of the 62 films pinned within 6 degrees in the retail run is 20
	// degrees (quartiles 6 and 28.5; VOXELIZER.md), kept inside the open
	// range.
	{
		double lo = 1e9, hi = -1e9;
		for (double e = -5; e <= 80.01; e += 5)
		{
			VoxCamera c2 = G.camera; c2.elevation = Deg(e);
			if (Score(fine, b, c2, &G.evaluations) >= G.meanIoU - 0.02) { lo = std::min(lo, e); hi = std::max(hi, e); }
		}
		if (hi - lo > 15)
		{
			const double e = std::min(std::max(VOX_TYPICAL_ELEVATION_DEG, lo), hi);
			G.camera.elevation = Deg(e);
			G.elevationOpen = true;
			char t[160];
			snprintf(t, sizeof t, "elevation open (%.0f..%.0f within 0.02): the typical %.0f taken; ", lo, hi, e);
			G.note += t;
		}
	}

	std::vector<double> per(film.frames, -1.0);
	G.meanIoU = Score(fine, b, G.camera, &G.evaluations, 0, &per);
	G.minFrameIoU = 1;
	for (int k = k0; k <= k1; k++) G.minFrameIoU = std::min(G.minFrameIoU, per[k]);
	G.turntable = G.meanIoU >= opt.minIoU && G.minFrameIoU >= opt.minFrameIoU;
	if (!G.turntable) G.note += "silhouettes do not fit a rigid turntable; ";
	return G;
}

//==========================================================================
//
// Carving and colour.
//
//==========================================================================

bool VoxModel::Surface(int x, int y, int z) const
{
	if (!Solid(x, y, z)) return false;
	return !Solid(x - 1, y, z) || !Solid(x + 1, y, z) || !Solid(x, y - 1, z) || !Solid(x, y + 1, z) ||
		!Solid(x, y, z - 1) || !Solid(x, y, z + 1);
}

int VoxModel::Count() const
{
	int c = 0;
	for (uint8_t s : solid) c += s;
	return c;
}

int VoxModel::SurfaceCount() const
{
	int c = 0;
	for (int z = 0; z < sz; z++)
		for (int y = 0; y < sy; y++)
			for (int x = 0; x < sx; x++)
				c += Surface(x, y, z);
	return c;
}

namespace
{

Grid ModelGrid(const VoxModel &m)
{
	Grid g;
	g.nx = m.sx; g.ny = m.sy; g.nz = m.sz; g.vs = m.voxelPx; g.x0 = m.originX; g.y0 = m.originY; g.z0 = m.originZ;
	return g;
}

// Outward normals from a box-blurred occupancy (radius r).
void Normals(const Grid &g, const std::vector<uint8_t> &s, const std::vector<Pt> &surf, int r, std::vector<float> &nrm)
{
	// Summed-volume table for box sums.
	const int X = g.nx + 1, Y = g.ny + 1, Z = g.nz + 1;
	std::vector<int32_t> S((size_t)X * Y * Z, 0);
	auto at = [&](int x, int y, int z) -> int32_t & { return S[(size_t)x + (size_t)X * ((size_t)y + (size_t)Y * z)]; };
	for (int z = 1; z < Z; z++)
		for (int y = 1; y < Y; y++)
			for (int x = 1; x < X; x++)
				at(x, y, z) = s[g.Index(x - 1, y - 1, z - 1)] + at(x - 1, y, z) + at(x, y - 1, z) + at(x, y, z - 1)
					- at(x - 1, y - 1, z) - at(x - 1, y, z - 1) - at(x, y - 1, z - 1) + at(x - 1, y - 1, z - 1);
	auto box = [&](int x0, int y0, int z0, int x1, int y1, int z1) -> int32_t
	{
		x0 = std::max(x0, 0); y0 = std::max(y0, 0); z0 = std::max(z0, 0);
		x1 = std::min(x1, g.nx); y1 = std::min(y1, g.ny); z1 = std::min(z1, g.nz);
		if (x0 >= x1 || y0 >= y1 || z0 >= z1) return 0;
		return at(x1, y1, z1) - at(x0, y1, z1) - at(x1, y0, z1) - at(x1, y1, z0) + at(x0, y0, z1) + at(x0, y1, z0) + at(x1, y0, z0) - at(x0, y0, z0);
	};
	nrm.resize(surf.size() * 3);
	for (size_t i = 0; i < surf.size(); i++)
	{
		const Pt &p = surf[i];
		// Difference of the half-boxes on either side, per axis.
		double n[3];
		n[0] = box(p.x - r, p.y - r, p.z - r, p.x, p.y + r + 1, p.z + r + 1) - box(p.x + 1, p.y - r, p.z - r, p.x + r + 1, p.y + r + 1, p.z + r + 1);
		n[1] = box(p.x - r, p.y - r, p.z - r, p.x + r + 1, p.y, p.z + r + 1) - box(p.x - r, p.y + 1, p.z - r, p.x + r + 1, p.y + r + 1, p.z + r + 1);
		n[2] = box(p.x - r, p.y - r, p.z - r, p.x + r + 1, p.y + r + 1, p.z) - box(p.x - r, p.y - r, p.z + 1, p.x + r + 1, p.y + r + 1, p.z + r + 1);
		double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (l < 1e-9)
		{
			// Thin parts: the open faces.
			auto open = [&](int x, int y, int z) { return x < 0 || y < 0 || z < 0 || x >= g.nx || y >= g.ny || z >= g.nz || !s[g.Index(x, y, z)]; };
			n[0] = open(p.x + 1, p.y, p.z) - open(p.x - 1, p.y, p.z);
			n[1] = open(p.x, p.y + 1, p.z) - open(p.x, p.y - 1, p.z);
			n[2] = open(p.x, p.y, p.z + 1) - open(p.x, p.y, p.z - 1);
			l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (l < 1e-9) { n[0] = 0; n[1] = 1; n[2] = 0; l = 1; }
		}
		for (int a = 0; a < 3; a++) nrm[i * 3 + a] = (float)(n[a] / l);
	}
}

// Per frame: which surface voxels the camera sees, and the film pixel under
// each. Calls fn(voxel, frame, pixel index, facing).
//
// VISIBILITY IS RAY-MARCHED, not z-buffered: a voxel is seen when the line
// from its open face (the one turned most toward the camera) to the camera
// meets no solid voxel. A z-buffer at film pixels is wrong for the faces the
// films see at a grazing angle (a matchbox's top at 21 degrees: one pixel
// row spans three voxels of depth, so only the front one of every three
// "won" and the label was coloured from scattered frames).
template<class Fn>
void ForVisible(const Grid &g, const std::vector<uint8_t> &s, const std::vector<Pt> &surf, const std::vector<float> &nrm,
	const VoxCamera &cam, int w, int h, const std::vector<uint8_t> &masks, Fn fn)
{
	auto solid = [&](int x, int y, int z) { return x >= 0 && y >= 0 && z >= 0 && x < g.nx && y < g.ny && z < g.nz && s[g.Index(x, y, z)]; };
	const int fx[6] = { 1, -1, 0, 0, 0, 0 }, fy[6] = { 0, 0, 1, -1, 0, 0 }, fz[6] = { 0, 0, 0, 0, 1, -1 };
	for (int k = cam.firstFrame; k <= cam.lastFrame; k++)
	{
		FrameProj P; P.Set(cam, k);
		double vd[3]; cam.ViewDir(k, vd);
		// The camera in the object frame (perspective): on the view line
		// through the axis point the picture's axisY row shows.
		const double D = cam.invDistance > 0 ? 1.0 / cam.invDistance : 0;
		const uint8_t *m = masks.data() + (size_t)k * w * h;
		for (size_t i = 0; i < surf.size(); i++)
		{
			const Pt &p = surf[i];
			const double cx = g.Cx(p.x), cy = g.Cy(p.y), cz = g.Cz(p.z);
			double u, v, d;
			P.Map(cx, cy, cz, u, v, d);
			const int iu = (int)floor(u), iv = (int)floor(v);
			if (iu < 0 || iv < 0 || iu >= w || iv >= h || !m[iv * w + iu]) continue;
			// Direction to the camera, in voxel units.
			double dir[3] = { vd[0], vd[1], vd[2] };
			if (D > 0)
			{
				dir[0] = D * vd[0] - cx; dir[1] = D * vd[1] - cy; dir[2] = D * vd[2] - cz;
				const double l = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
				for (double &q : dir) q /= l;
			}
			int face = -1;
			double bestDot = 0;
			for (int f = 0; f < 6; f++)
			{
				if (solid(p.x + fx[f], p.y + fy[f], p.z + fz[f])) continue;
				const double dt = fx[f] * dir[0] + fy[f] * dir[1] + fz[f] * dir[2];
				if (dt > bestDot) { bestDot = dt; face = f; }
			}
			if (face < 0) continue;
			// March from just outside that face, in steps of 0.4 voxel.
			double px = p.x + 0.5 + 0.51 * fx[face], py = p.y + 0.5 + 0.51 * fy[face], pz = p.z + 0.5 + 0.51 * fz[face];
			bool blocked = false;
			for (int step = 0; step < 4 * (g.nx + g.ny + g.nz); step++)
			{
				const int X = (int)floor(px), Y = (int)floor(py), Z = (int)floor(pz);
				if (X < 0 || Y < 0 || Z < 0 || X >= g.nx || Y >= g.ny || Z >= g.nz) break;
				// The first voxel of travel is not tested: a one-voxel step in
				// the surface would otherwise hide a face the film plainly
				// shows at a grazing angle.
				if (step * 0.4 >= 1.2 && (X != p.x || Y != p.y || Z != p.z) && s[g.Index(X, Y, Z)]) { blocked = true; break; }
				px += 0.4 * dir[0]; py += 0.4 * dir[1]; pz += 0.4 * dir[2];
			}
			if (blocked) continue;
			const double facing = nrm[i * 3] * vd[0] + nrm[i * 3 + 1] * vd[1] + nrm[i * 3 + 2] * vd[2];
			fn(i, k, iv * w + iu, facing);
		}
	}
}

int Nearest(const uint8_t *pal, double r, double g, double b)
{
	int best = 0;
	double bd = 1e30;
	for (int i = 0; i < 256; i++)
	{
		const double dr = pal[i * 3] - r, dg = pal[i * 3 + 1] - g, db = pal[i * 3 + 2] - b;
		const double d = 2 * dr * dr + 4 * dg * dg + db * db;
		if (d < bd) { bd = d; best = i; }
	}
	return best;
}

double PhotoError(const VoxFilm &film, const std::vector<uint8_t> &masks, const VoxCamera &cam, double vs)
{
	const Bounds b = MaskBounds(masks, film.width, film.height, cam.firstFrame, cam.lastFrame);
	const Grid g = GridFor(cam, b, vs);
	std::vector<uint8_t> s;
	Carve(g, cam, masks.data(), film.width, film.height, 1.0, 0, s);
	const std::vector<Pt> surf = SurfaceList(g, s);
	if (surf.empty()) return 1e9;
	std::vector<float> nrm;
	Normals(g, s, surf, 2, nrm);
	// Per voxel: the sum and squared sum of RGB over the frames that see it
	// squarely enough.
	std::vector<double> acc(surf.size() * 7, 0.0);
	ForVisible(g, s, surf, nrm, cam, film.width, film.height, masks, [&](size_t i, int k, int pix, double facing)
	{
		if (facing < 0.3) return;
		const uint8_t *c = film.palette + film.Frame(k)[pix] * 3;
		double *a = &acc[i * 7];
		for (int j = 0; j < 3; j++) { a[j] += c[j]; a[3 + j] += (double)c[j] * c[j]; }
		a[6] += 1;
	});
	double sum = 0; int cnt = 0;
	for (size_t i = 0; i < surf.size(); i++)
	{
		const double *a = &acc[i * 7];
		if (a[6] < 2) continue;
		double var = 0;
		for (int j = 0; j < 3; j++) { const double mu = a[j] / a[6]; var += a[3 + j] / a[6] - mu * mu; }
		sum += sqrt(std::max(0.0, var / 3)); cnt++;
	}
	return cnt ? sum / cnt : 1e9;
}

// A 256-colour palette for a set of RGB colours: median cut, then two
// rounds of k-means.
void MedianCut(const std::vector<float> &cols, uint8_t pal[768])
{
	const size_t n = cols.size() / 3;
	memset(pal, 0, 768);
	if (n == 0) return;
	std::vector<uint32_t> idx(n);
	for (size_t i = 0; i < n; i++) idx[i] = (uint32_t)i;
	struct Box { size_t a, b; };
	std::vector<Box> boxes = { { 0, n } };
	auto range = [&](const Box &bx, int &axis) -> float
	{
		float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
		for (size_t i = bx.a; i < bx.b; i++)
			for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], cols[idx[i] * 3 + j]); hi[j] = std::max(hi[j], cols[idx[i] * 3 + j]); }
		axis = 0;
		for (int j = 1; j < 3; j++) if (hi[j] - lo[j] > hi[axis] - lo[axis]) axis = j;
		return hi[axis] - lo[axis];
	};
	while (boxes.size() < 256)
	{
		int bi = -1, axis = 0;
		double bestScore = 0;
		for (size_t i = 0; i < boxes.size(); i++)
		{
			if (boxes[i].b - boxes[i].a < 2) continue;
			int ax;
			const double r = range(boxes[i], ax);
			const double sc = r * sqrt((double)(boxes[i].b - boxes[i].a));
			if (sc > bestScore) { bestScore = sc; bi = (int)i; axis = ax; }
		}
		if (bi < 0 || bestScore <= 0) break;
		Box bx = boxes[bi];
		const size_t mid = (bx.a + bx.b) / 2;
		std::nth_element(idx.begin() + bx.a, idx.begin() + mid, idx.begin() + bx.b,
			[&](uint32_t p, uint32_t q) { return cols[p * 3 + axis] < cols[q * 3 + axis]; });
		boxes[bi] = { bx.a, mid };
		boxes.push_back({ mid, bx.b });
	}
	std::vector<double> c(boxes.size() * 3, 0.0);
	for (size_t bI = 0; bI < boxes.size(); bI++)
	{
		for (size_t i = boxes[bI].a; i < boxes[bI].b; i++) for (int j = 0; j < 3; j++) c[bI * 3 + j] += cols[idx[i] * 3 + j];
		for (int j = 0; j < 3; j++) c[bI * 3 + j] /= (double)(boxes[bI].b - boxes[bI].a);
	}
	const size_t K = boxes.size();
	for (int round = 0; round < 2; round++)
	{
		std::vector<double> sum(K * 3, 0.0), cnt(K, 0.0);
		for (size_t i = 0; i < n; i++)
		{
			size_t best = 0; double bd = 1e30;
			for (size_t k = 0; k < K; k++)
			{
				const double dr = cols[i * 3] - c[k * 3], dg = cols[i * 3 + 1] - c[k * 3 + 1], db = cols[i * 3 + 2] - c[k * 3 + 2];
				const double d = 2 * dr * dr + 4 * dg * dg + db * db;
				if (d < bd) { bd = d; best = k; }
			}
			for (int j = 0; j < 3; j++) sum[best * 3 + j] += cols[i * 3 + j];
			cnt[best] += 1;
		}
		for (size_t k = 0; k < K; k++) if (cnt[k] > 0) for (int j = 0; j < 3; j++) c[k * 3 + j] = sum[k * 3 + j] / cnt[k];
	}
	for (size_t k = 0; k < K; k++) for (int j = 0; j < 3; j++) pal[k * 3 + j] = (uint8_t)std::min(255.0, std::max(0.0, std::round(c[k * 3 + j])));
}

// SPACE CARVING by photo-consistency (Kutulakos and Seitz's idea, with the
// silhouettes kept): a surface voxel that the frames seeing it disagree about
// is not on the item -- the frames are looking at different things through
// it -- so it goes, and the voxel behind it is judged next round. Two guards:
// a voxel whose removal would leave a mask pixel of some frame with no voxel
// projecting onto it stays (the silhouettes are the measurement), and no
// voxel deeper than opt.carveDepth below the hull's surface is touched.
// Disagreement is measured twice, because the lighting is baked: the
// colours with each sample's brightness scaled to the voxel's mean
// (shading changes as the item turns under a fixed light; texture changes
// hue), and the raw colours with a looser bound (grey items).
int PhotoCarve(const Grid &g, std::vector<uint8_t> &s, const VoxCamera &cam, const std::vector<uint8_t> &masks, int w, int h,
	const std::vector<float> &rgb, const VoxCarveOptions &opt)
{
	const int k0 = cam.firstFrame, k1 = cam.lastFrame, nk = k1 - k0 + 1;
	const size_t fsz = (size_t)w * h;
	// Coverage: how many solid voxel centres project onto each mask pixel.
	std::vector<uint16_t> cover((size_t)nk * fsz, 0);
	std::vector<FrameProj> P(nk);
	for (int k = k0; k <= k1; k++) P[k - k0].Set(cam, k);
	auto pixelOf = [&](int kk, int x, int y, int z) -> int
	{
		double u, v, d;
		P[kk].Map(g.Cx(x), g.Cy(y), g.Cz(z), u, v, d);
		const int iu = (int)floor(u), iv = (int)floor(v);
		return (iu < 0 || iv < 0 || iu >= w || iv >= h) ? -1 : iv * w + iu;
	};
	for (int z = 0; z < g.nz; z++)
		for (int y = 0; y < g.ny; y++)
			for (int x = 0; x < g.nx; x++)
				if (s[g.Index(x, y, z)])
					for (int kk = 0; kk < nk; kk++)
					{
						const int p = pixelOf(kk, x, y, z);
						if (p >= 0 && cover[(size_t)kk * fsz + p] < 65535) cover[(size_t)kk * fsz + p]++;
					}
	// Depth below the hull's surface (6-connected steps).
	std::vector<uint16_t> depth(s.size(), 65535);
	{
		std::vector<size_t> front;
		for (const Pt &p : SurfaceList(g, s)) { const size_t i = g.Index(p.x, p.y, p.z); depth[i] = 0; front.push_back(i); }
		const int dx[6] = { 1, -1, 0, 0, 0, 0 }, dy[6] = { 0, 0, 1, -1, 0, 0 }, dz[6] = { 0, 0, 0, 0, 1, -1 };
		for (uint16_t dd = 0; !front.empty() && dd < 1000; dd++)
		{
			std::vector<size_t> next;
			for (size_t i : front)
			{
				const int x = (int)(i % g.nx), y = (int)((i / g.nx) % g.ny), z = (int)(i / ((size_t)g.nx * g.ny));
				for (int d = 0; d < 6; d++)
				{
					const int X = x + dx[d], Y = y + dy[d], Z = z + dz[d];
					if (X < 0 || Y < 0 || Z < 0 || X >= g.nx || Y >= g.ny || Z >= g.nz) continue;
					const size_t j = g.Index(X, Y, Z);
					if (s[j] && depth[j] == 65535) { depth[j] = (uint16_t)(dd + 1); next.push_back(j); }
				}
			}
			front.swap(next);
		}
	}
	int carved = 0;
	for (int iter = 0; iter < opt.carveRounds; iter++)
	{
		const std::vector<Pt> surf = SurfaceList(g, s);
		std::vector<float> nrm;
		Normals(g, s, surf, 2, nrm);
		// The frames that see each surface voxel.
		std::vector<std::vector<uint8_t>> seenBy(surf.size());
		ForVisible(g, s, surf, nrm, cam, w, h, masks, [&](size_t i, int k, int, double facing)
		{
			if (facing >= 0.1) seenBy[i].push_back((uint8_t)(k - k0));
		});
		// How much those frames disagree about the colour at a point: the
		// larger of the two spreads over their bounds (> 1 = inconsistent).
		auto spread = [&](const std::vector<uint8_t> &frames, double x, double y, double z) -> double
		{
			double c[64][3];
			int n = 0;
			for (uint8_t kk : frames)
			{
				double u, v, d;
				P[kk].Map(x, y, z, u, v, d);
				const int iu = (int)floor(u), iv = (int)floor(v);
				if (iu < 0 || iv < 0 || iu >= w || iv >= h || n >= 64) continue;
				const float *q = rgb.data() + ((size_t)(kk + k0) * fsz + (size_t)iv * w + iu) * 3;
				c[n][0] = q[0]; c[n][1] = q[1]; c[n][2] = q[2];
				n++;
			}
			if (n < 2) return 0;
			double mean[3] = { 0, 0, 0 }, lm = 0;
			for (int a2 = 0; a2 < n; a2++) for (int j = 0; j < 3; j++) mean[j] += c[a2][j];
			for (int j = 0; j < 3; j++) { mean[j] /= n; lm += mean[j]; }
			lm /= 3;
			double rawVar = 0, normVar = 0;
			for (int a2 = 0; a2 < n; a2++)
			{
				const double l = (c[a2][0] + c[a2][1] + c[a2][2]) / 3.0;
				const double sc = l > 12 && lm > 12 ? lm / l : 1.0;
				for (int j = 0; j < 3; j++)
				{
					rawVar += (c[a2][j] - mean[j]) * (c[a2][j] - mean[j]);
					const double e = c[a2][j] * sc - mean[j];
					normVar += e * e;
				}
			}
			return std::max(sqrt(normVar / (3.0 * n)) / opt.carveHueBound, sqrt(rawVar / (3.0 * n)) / opt.carveRawBound);
		};
		struct Cand { float bad; size_t i; };
		std::vector<Cand> cands;
		std::vector<uint8_t> marked(s.size(), 0);
		// Per surface voxel: the sweep's depth (-1 = not judged), and the gain.
		std::vector<int> tStar(surf.size(), -1);
		std::vector<float> gain(surf.size(), 0.0f);
		for (size_t i = 0; i < surf.size(); i++)
		{
			const std::vector<uint8_t> &fr = seenBy[i];
			if ((int)fr.size() < opt.carveMinViews) continue;
			const Pt &p = surf[i];
			const size_t gi = g.Index(p.x, p.y, p.z);
			if (depth[gi] > opt.carveDepth) continue;
			const double cx = g.Cx(p.x), cy = g.Cy(p.y), cz = g.Cz(p.z);
			const double bad0 = spread(fr, cx, cy, cz);
			tStar[i] = 0;
			if (bad0 > 1.0) { marked[gi] = 1; cands.push_back({ (float)(1.0 + bad0), gi }); continue; }
			// The depth sweep: the same frames, at points further in along
			// the normal. Where they agree clearly better, the surface is
			// there.
			const double nx = nrm[i * 3], ny = nrm[i * 3 + 1], nz = nrm[i * 3 + 2];
			double best = bad0;
			int bestT = 0;
			for (int t = 1; t <= opt.carveDepth; t++)
			{
				const double x = cx - t * g.vs * nx, y = cy - t * g.vs * ny, z = cz - t * g.vs * nz;
				const int X = (int)floor((x - g.x0) / g.vs), Y = (int)floor((y - g.y0) / g.vs), Z = (int)floor((z - g.z0) / g.vs);
				if (X < 0 || Y < 0 || Z < 0 || X >= g.nx || Y >= g.ny || Z >= g.nz || !s[g.Index(X, Y, Z)]) break;
				const double b2 = spread(fr, x, y, z);
				if (b2 < best) { best = b2; bestT = t; }
			}
			if (bestT > 0 && best <= opt.sweepGain * bad0) { tStar[i] = bestT; gain[i] = (float)(bad0 - best); }
		}
		// One sweep is noisy (texture-poor patches agree anywhere): each voxel
		// moves in by the MEDIAN depth of the judged surface voxels around it
		// (radius 2), so the new surface is as smooth as the old one.
		{
			std::map<size_t, size_t> where;
			for (size_t i = 0; i < surf.size(); i++) where[g.Index(surf[i].x, surf[i].y, surf[i].z)] = i;
			std::vector<int> around;
			for (size_t i = 0; i < surf.size(); i++)
			{
				if (tStar[i] < 0) continue;
				around.clear();
				const Pt &p = surf[i];
				for (int dz = -2; dz <= 2; dz++)
					for (int dy = -2; dy <= 2; dy++)
						for (int dx = -2; dx <= 2; dx++)
						{
							const int X = p.x + dx, Y = p.y + dy, Z = p.z + dz;
							if (X < 0 || Y < 0 || Z < 0 || X >= g.nx || Y >= g.ny || Z >= g.nz) continue;
							auto it = where.find(g.Index(X, Y, Z));
							if (it != where.end() && tStar[it->second] >= 0) around.push_back(tStar[it->second]);
						}
				if (around.size() < 5) continue;
				std::nth_element(around.begin(), around.begin() + around.size() / 2, around.end());
				const int t = around[around.size() / 2];
				if (t < 1) continue;
				const double cx = g.Cx(p.x), cy = g.Cy(p.y), cz = g.Cz(p.z);
				const double nx = nrm[i * 3], ny = nrm[i * 3 + 1], nz = nrm[i * 3 + 2];
				for (int q = 0; q < t; q++)
				{
					const int X = (int)floor((cx - q * g.vs * nx - g.x0) / g.vs), Y = (int)floor((cy - q * g.vs * ny - g.y0) / g.vs),
						Z = (int)floor((cz - q * g.vs * nz - g.z0) / g.vs);
					if (X < 0 || Y < 0 || Z < 0 || X >= g.nx || Y >= g.ny || Z >= g.nz) break;
					const size_t gj = g.Index(X, Y, Z);
					if (s[gj] && !marked[gj] && depth[gj] <= opt.carveDepth) { marked[gj] = 1; cands.push_back({ gain[i], gj }); }
				}
			}
		}
		if (getenv("VOX_DEBUG"))
		{
			int judged = 0;
			for (size_t i = 0; i < surf.size(); i++) if ((int)seenBy[i].size() >= opt.carveMinViews) judged++;
			fprintf(stderr, "iter %d surf %zu judged %d cands %zu\n", iter, surf.size(), judged, cands.size());
		}
		if (cands.empty()) break;
		std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.bad > b.bad; });
		int now = 0;
		std::vector<int> pix(nk);
		for (const Cand &c : cands)
		{
			const int x = (int)(c.i % g.nx), y = (int)((c.i / g.nx) % g.ny), z = (int)(c.i / ((size_t)g.nx * g.ny));
			bool keep = false;
			for (int kk = 0; kk < nk && !keep; kk++)
			{
				pix[kk] = pixelOf(kk, x, y, z);
				if (pix[kk] >= 0 && cover[(size_t)kk * fsz + pix[kk]] <= 1 && masks[(size_t)(kk + k0) * fsz + pix[kk]]) keep = true;
			}
			if (keep) continue;
			for (int kk = 0; kk < nk; kk++) if (pix[kk] >= 0 && cover[(size_t)kk * fsz + pix[kk]] > 0) cover[(size_t)kk * fsz + pix[kk]]--;
			s[c.i] = 0;
			now++;
		}
		carved += now;
		if (getenv("VOX_DEBUG")) fprintf(stderr, "   carved %d\n", now);
		if (now == 0) break;
	}
	return carved;
}

} // namespace

bool VoxCarve(const VoxFilm &film, const std::vector<uint8_t> &masks, const VoxCamera &cam, const VoxCarveOptions &opt, VoxModel &out)
{
	out = VoxModel();
	const int w = film.width, h = film.height;
	const Bounds b = MaskBounds(masks, w, h, cam.firstFrame, cam.lastFrame);
	if (!b.any) return false;
	double vs = opt.voxelPx;
	Grid g;
	std::vector<uint8_t> s;
	for (int attempt = 0; attempt < 8; attempt++)
	{
		g = GridFor(cam, b, vs);
		Carve(g, cam, masks.data(), w, h, 1.0, opt.missTolerance, s);
		// Drop tiny islands (under 8 voxels, 26-connected).
		{
			std::vector<uint8_t> seen(s.size(), 0);
			std::vector<size_t> stack, comp;
			for (size_t i = 0; i < s.size(); i++)
			{
				if (!s[i] || seen[i]) continue;
				comp.clear(); stack.assign(1, i); seen[i] = 1;
				while (!stack.empty())
				{
					const size_t q = stack.back(); stack.pop_back(); comp.push_back(q);
					const int qx = (int)(q % g.nx), qy = (int)((q / g.nx) % g.ny), qz = (int)(q / ((size_t)g.nx * g.ny));
					for (int dz = -1; dz <= 1; dz++)
						for (int dy = -1; dy <= 1; dy++)
							for (int dx = -1; dx <= 1; dx++)
							{
								const int x = qx + dx, y = qy + dy, z = qz + dz;
								if (x < 0 || y < 0 || z < 0 || x >= g.nx || y >= g.ny || z >= g.nz) continue;
								const size_t r = g.Index(x, y, z);
								if (s[r] && !seen[r]) { seen[r] = 1; stack.push_back(r); }
							}
				}
				if (comp.size() < 8) for (size_t q : comp) s[q] = 0;
			}
		}
		// Crop to the solid's box (one empty voxel of margin is not needed:
		// out-of-range reads as empty).
		int x0 = g.nx, x1 = -1, y0 = g.ny, y1 = -1, z0 = g.nz, z1 = -1;
		for (int z = 0; z < g.nz; z++)
			for (int y = 0; y < g.ny; y++)
				for (int x = 0; x < g.nx; x++)
					if (s[g.Index(x, y, z)])
					{
						x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
						z0 = std::min(z0, z); z1 = std::max(z1, z);
					}
		if (x1 < 0) return false;
		const int nx = x1 - x0 + 1, ny = y1 - y0 + 1, nz = z1 - z0 + 1;
		if (std::max(nx, std::max(ny, nz)) > opt.maxSide) { vs *= 1.25; continue; }
		Grid c;
		c.nx = nx; c.ny = ny; c.nz = nz; c.vs = vs;
		c.x0 = g.x0 + x0 * vs; c.y0 = g.y0 + y0 * vs; c.z0 = g.z0 + z0 * vs;
		std::vector<uint8_t> cs(c.Size());
		for (int z = 0; z < nz; z++)
			for (int y = 0; y < ny; y++)
				for (int x = 0; x < nx; x++)
					cs[c.Index(x, y, z)] = s[g.Index(x + x0, y + y0, z + z0)];
		g = c; s.swap(cs);
		break;
	}
	if (std::max(g.nx, std::max(g.ny, g.nz)) > opt.maxSide) return false;

	// The film in RGB, and (de-dithered) a [1 2 1] x [1 2 1] filter over the
	// item's own pixels: the renderer dithered its dark and blended parts in
	// checkerboards, which a single pixel per voxel turns into speckle.
	const size_t fsz = (size_t)w * h;
	std::vector<float> rgb((size_t)film.frames * fsz * 3);
	for (int k = cam.firstFrame; k <= cam.lastFrame; k++)
	{
		const uint8_t *f = film.Frame(k), *mk = masks.data() + (size_t)k * fsz;
		float *o = rgb.data() + (size_t)k * fsz * 3;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
			{
				float acc[3] = { 0, 0, 0 }, ws = 0;
				const int r = opt.deDither ? 1 : 0;
				for (int dy = -r; dy <= r; dy++)
					for (int dx = -r; dx <= r; dx++)
					{
						const int X = x + dx, Y = y + dy;
						if (X < 0 || Y < 0 || X >= w || Y >= h || !mk[Y * w + X]) continue;
						const float wt = (float)((2 - abs(dx)) * (2 - abs(dy)));
						const uint8_t *c = film.palette + f[Y * w + X] * 3;
						for (int j = 0; j < 3; j++) acc[j] += wt * c[j];
						ws += wt;
					}
				for (int j = 0; j < 3; j++) o[(y * w + x) * 3 + j] = ws > 0 ? acc[j] / ws : 0;
			}
	}

	// Photo-consistency: carve what the silhouettes could not (the wedges a
	// few views leave around flat and thin parts, the inside of a cage).
	if (opt.photoCarve) out.photoCarved = PhotoCarve(g, s, cam, masks, w, h, rgb, opt);

	out.sx = g.nx; out.sy = g.ny; out.sz = g.nz; out.voxelPx = g.vs;
	out.originX = g.x0; out.originY = g.y0; out.originZ = g.z0;
	out.solid = s;
	out.colour.assign(s.size(), 0);

	const std::vector<Pt> surf = SurfaceList(g, s);
	std::vector<float> nrm;
	Normals(g, s, surf, 2, nrm);

	// Each surface voxel's samples: (colour, facing) from every frame that
	// sees it front-most.
	struct Sample { float c[3]; float facing; };
	std::vector<std::vector<Sample>> samples(surf.size());
	ForVisible(g, s, surf, nrm, cam, w, h, masks, [&](size_t i, int k, int pix, double facing)
	{
		if (facing <= 0.05) return;
		const float *c = rgb.data() + ((size_t)k * fsz + pix) * 3;
		samples[i].push_back({ { c[0], c[1], c[2] }, (float)facing });
	});

	std::vector<float> col(surf.size() * 3, 0.0f);
	std::vector<uint8_t> have(surf.size(), 0);
	for (size_t i = 0; i < surf.size(); i++)
	{
		std::vector<Sample> &sm = samples[i];
		if (sm.empty()) continue;
		have[i] = 1;
		float fmax = 0;
		for (const Sample &q : sm) fmax = std::max(fmax, q.facing);
		if (opt.colourMode == VoxColourMode::Front)
		{
			for (const Sample &q : sm) if (q.facing == fmax) { memcpy(&col[i * 3], q.c, sizeof q.c); break; }
			continue;
		}
		std::vector<Sample> good;
		for (const Sample &q : sm) if (q.facing >= 0.7f * fmax) good.push_back(q);
		if (opt.colourMode == VoxColourMode::Median)
		{
			auto luma = [](const Sample &q) { return 0.299f * q.c[0] + 0.587f * q.c[1] + 0.114f * q.c[2]; };
			std::sort(good.begin(), good.end(), [&](const Sample &a2, const Sample &b2) { return luma(a2) < luma(b2); });
			memcpy(&col[i * 3], good[good.size() / 2].c, sizeof good[0].c);
		}
		else
		{
			double acc[3] = { 0, 0, 0 }, ws = 0;
			for (const Sample &q : good) { for (int j = 0; j < 3; j++) acc[j] += q.c[j] * q.facing; ws += q.facing; }
			for (int j = 0; j < 3; j++) col[i * 3 + j] = (float)(acc[j] / ws);
		}
	}
	// Voxels no frame saw (the underside, closed pockets): the colour of the
	// nearest seen neighbour, spreading over the surface.
	{
		std::map<size_t, size_t> where;
		for (size_t i = 0; i < surf.size(); i++) where[g.Index(surf[i].x, surf[i].y, surf[i].z)] = i;
		std::vector<size_t> front;
		for (size_t i = 0; i < surf.size(); i++) if (have[i]) front.push_back(i);
		while (!front.empty())
		{
			std::vector<size_t> next;
			for (size_t i : front)
			{
				const Pt &p = surf[i];
				for (int dz = -1; dz <= 1; dz++)
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++)
						{
							const int x = p.x + dx, y = p.y + dy, z = p.z + dz;
							if (x < 0 || y < 0 || z < 0 || x >= g.nx || y >= g.ny || z >= g.nz) continue;
							auto it = where.find(g.Index(x, y, z));
							if (it == where.end() || have[it->second]) continue;
							have[it->second] = 2;
							memcpy(&col[it->second * 3], &col[i * 3], 3 * sizeof(float));
							next.push_back(it->second);
						}
			}
			front.swap(next);
		}
	}

	// The palette: the film's own, or 256 colours cut from the model's.
	if (opt.ownPalette) MedianCut(col, out.palette);
	else memcpy(out.palette, film.palette, 768);
	for (size_t i = 0; i < surf.size(); i++)
		out.colour[g.Index(surf[i].x, surf[i].y, surf[i].z)] = (uint8_t)Nearest(out.palette, col[i * 3], col[i * 3 + 1], col[i * 3 + 2]);
	return true;
}

std::vector<double> VoxReprojectIoU(const VoxModel &m, const VoxCamera &cam, const std::vector<uint8_t> &masks, int width, int height, int frames)
{
	std::vector<double> per(frames, -1.0);
	const Grid g = ModelGrid(m);
	const std::vector<Pt> surf = SurfaceList(g, m.solid);
	ReprojectIoU(g, surf, cam, masks.data(), width, height, 1.0, &per);
	return per;
}

double VoxPhotometricError(const VoxModel &m, const VoxCamera &cam, const VoxFilm &film, const std::vector<uint8_t> &masks)
{
	const Grid g = ModelGrid(m);
	const std::vector<Pt> surf = SurfaceList(g, m.solid);
	const int w = film.width, h = film.height;
	std::vector<float> zb((size_t)w * h);
	std::vector<int> ci((size_t)w * h);
	const int sub = std::max(2, (int)ceil(g.vs * 1.5));
	double sum = 0;
	size_t cnt = 0;
	for (int k = cam.firstFrame; k <= cam.lastFrame; k++)
	{
		FrameProj P; P.Set(cam, k);
		std::fill(zb.begin(), zb.end(), -1e30f);
		std::fill(ci.begin(), ci.end(), -1);
		for (const Pt &p : surf)
		{
			const int c = m.colour[g.Index(p.x, p.y, p.z)];
			for (int a = 0; a < sub; a++)
				for (int b = 0; b < sub; b++)
					for (int cc = 0; cc < sub; cc++)
					{
						double u, v, d;
						P.Map(g.x0 + (p.x + (a + 0.5) / sub) * g.vs, g.y0 + (p.y + (b + 0.5) / sub) * g.vs, g.z0 + (p.z + (cc + 0.5) / sub) * g.vs, u, v, d);
						const int iu = (int)floor(u), iv = (int)floor(v);
						if (iu >= 0 && iv >= 0 && iu < w && iv < h && d > zb[iv * w + iu]) { zb[iv * w + iu] = (float)d; ci[iv * w + iu] = c; }
					}
		}
		const uint8_t *f = film.Frame(k);
		const uint8_t *mk = masks.data() + (size_t)k * w * h;
		for (int i = 0; i < w * h; i++)
		{
			if (ci[i] < 0 || !mk[i]) continue;
			const uint8_t *a = m.palette + ci[i] * 3, *b2 = film.palette + f[i] * 3;
			sum += (fabs((double)a[0] - b2[0]) + fabs((double)a[1] - b2[1]) + fabs((double)a[2] - b2[2])) / 3.0;
			cnt++;
		}
	}
	return cnt ? sum / (double)cnt : -1;
}

void VoxRender(const VoxModel &m, double angle, double elevation, double scale, int w, int h, std::vector<uint8_t> &rgb, const uint8_t bg[3])
{
	rgb.assign((size_t)w * h * 3, 0);
	for (int i = 0; i < w * h; i++) memcpy(&rgb[i * 3], bg, 3);
	std::vector<float> zb((size_t)w * h, -1e30f);
	const Grid g = ModelGrid(m);
	const std::vector<Pt> surf = SurfaceList(g, m.solid);
	// Centre the model's box in the picture.
	const double cx = m.originX + m.sx * m.voxelPx / 2, cy = m.originY + m.sy * m.voxelPx / 2, cz = m.originZ + m.sz * m.voxelPx / 2;
	FrameProj P;
	{
		// Angle(k) = k for period 2*pi: pass the angle as a fractional frame.
		const double th = angle, c = cos(th), s = sin(th), cp = cos(elevation), sp = sin(elevation);
		P.rx[0] = c; P.rx[1] = 0; P.rx[2] = -s;
		P.up[0] = -s * sp; P.up[1] = cp; P.up[2] = -c * sp;
		P.dp[0] = s * cp; P.dp[1] = sp; P.dp[2] = c * cp;
		P.invD = 0; P.cr = 1; P.sr = 0; P.ax = 0; P.ay = 0;
	}
	double u0, v0, d0;
	P.Map(cx, cy, cz, u0, v0, d0);
	const double k = scale / m.voxelPx;
	const int sub = std::max(2, (int)ceil(scale * 1.5));
	for (const Pt &p : surf)
	{
		const uint8_t *c = m.palette + m.colour[g.Index(p.x, p.y, p.z)] * 3;
		for (int a = 0; a < sub; a++)
			for (int b = 0; b < sub; b++)
				for (int cc = 0; cc < sub; cc++)
				{
					double u, v, d;
					P.Map(g.x0 + (p.x + (a + 0.5) / sub) * g.vs, g.y0 + (p.y + (b + 0.5) / sub) * g.vs, g.z0 + (p.z + (cc + 0.5) / sub) * g.vs, u, v, d);
					const int iu = (int)floor((u - u0) * k + w / 2.0), iv = (int)floor((v - v0) * k + h / 2.0);
					if (iu < 0 || iv < 0 || iu >= w || iv >= h || d <= zb[iv * w + iu]) continue;
					zb[iv * w + iu] = (float)d;
					memcpy(&rgb[(iv * w + iu) * 3], c, 3);
				}
	}
}

void VoxRenderFrame(const VoxModel &m, const VoxCamera &cam, int k, int w, int h, std::vector<uint8_t> &rgb, const uint8_t bg[3])
{
	rgb.assign((size_t)w * h * 3, 0);
	for (int i = 0; i < w * h; i++) memcpy(&rgb[i * 3], bg, 3);
	std::vector<float> zb((size_t)w * h, -1e30f);
	const Grid g = ModelGrid(m);
	const std::vector<Pt> surf = SurfaceList(g, m.solid);
	FrameProj P; P.Set(cam, k);
	const int sub = std::max(2, (int)ceil(g.vs * 1.5));
	for (const Pt &p : surf)
	{
		const uint8_t *c = m.palette + m.colour[g.Index(p.x, p.y, p.z)] * 3;
		for (int a = 0; a < sub; a++)
			for (int b = 0; b < sub; b++)
				for (int cc = 0; cc < sub; cc++)
				{
					double u, v, d;
					P.Map(g.x0 + (p.x + (a + 0.5) / sub) * g.vs, g.y0 + (p.y + (b + 0.5) / sub) * g.vs, g.z0 + (p.z + (cc + 0.5) / sub) * g.vs, u, v, d);
					const int iu = (int)floor(u), iv = (int)floor(v);
					if (iu < 0 || iv < 0 || iu >= w || iv >= h || d <= zb[iv * w + iu]) continue;
					zb[iv * w + iu] = (float)d;
					memcpy(&rgb[(iv * w + iu) * 3], c, 3);
				}
	}
}

//==========================================================================
//
// KVX.
//
//==========================================================================

std::vector<uint8_t> VoxBuildKvx(const VoxModel &m, std::string *error)
{
	std::vector<uint8_t> out;
	auto fail = [&](const char *why) { if (error) *error = why; return std::vector<uint8_t>(); };
	// KVX axes: kx = x, ky = z, kz = sy - 1 - y (down).
	const int xs = m.sx, ys = m.sz, zs = m.sy;
	if (xs <= 0 || ys <= 0 || zs <= 0) return fail("empty model");
	if (zs > 255) return fail("taller than 255 voxels");
	auto solid = [&](int kx, int ky, int kz) { return m.Solid(kx, zs - 1 - kz, ky); };
	auto colour = [&](int kx, int ky, int kz) { return m.colour[m.Index(kx, zs - 1 - kz, ky)]; };

	std::vector<int32_t> xoff(xs + 1);
	std::vector<int16_t> xyoff((size_t)xs * (ys + 1));
	std::vector<uint8_t> data;
	const int offsetSize = (xs + 1) * 4 + xs * (ys + 1) * 2;
	for (int kx = 0; kx < xs; kx++)
	{
		xoff[kx] = offsetSize + (int32_t)data.size();
		const size_t colStart = data.size();
		for (int ky = 0; ky < ys; ky++)
		{
			if (data.size() - colStart > 32767) return fail("an x column's slabs pass 32767 bytes");
			xyoff[(size_t)kx * (ys + 1) + ky] = (int16_t)(data.size() - colStart);
			int kz = 0;
			while (kz < zs)
			{
				auto side = [&](int z) -> int
				{
					return (!solid(kx - 1, ky, z) ? 1 : 0) | (!solid(kx + 1, ky, z) ? 2 : 0) | (!solid(kx, ky - 1, z) ? 4 : 0) | (!solid(kx, ky + 1, z) ? 8 : 0);
				};
				auto visible = [&](int z) { return solid(kx, ky, z) && (side(z) != 0 || !solid(kx, ky, z - 1) || !solid(kx, ky, z + 1)); };
				if (!visible(kz)) { kz++; continue; }
				// A slab: consecutive visible voxels with the same side faces,
				// cut where a voxel's top or bottom is open (only the ends of a
				// slab may carry those).
				const int start = kz, bits = side(kz);
				int end = kz;
				while (end + 1 < zs && end + 1 - start < 255 && visible(end + 1) && side(end + 1) == bits) end++;
				int cull = bits;
				if (!solid(kx, ky, start - 1)) cull |= 16;
				if (!solid(kx, ky, end + 1)) cull |= 32;
				data.push_back((uint8_t)start);
				data.push_back((uint8_t)(end - start + 1));
				data.push_back((uint8_t)cull);
				for (int z = start; z <= end; z++) data.push_back(colour(kx, ky, z));
				kz = end + 1;
			}
		}
		if (data.size() - colStart > 32767) return fail("an x column's slabs pass 32767 bytes");
		xyoff[(size_t)kx * (ys + 1) + ys] = (int16_t)(data.size() - colStart);
	}
	xoff[xs] = offsetSize + (int32_t)data.size();

	const int32_t numbytes = 24 + offsetSize + (int32_t)data.size();
	auto put32 = [&](int32_t v) { for (int i = 0; i < 4; i++) out.push_back((uint8_t)((uint32_t)v >> (8 * i))); };
	auto put16 = [&](int16_t v) { out.push_back((uint8_t)((uint16_t)v & 0xff)); out.push_back((uint8_t)((uint16_t)v >> 8)); };
	put32(numbytes);
	put32(xs); put32(ys); put32(zs);
	// The pivot: the turntable axis (object X = Z = 0) at the model's base, in
	// 1/256 voxel.
	put32((int32_t)lround(-m.originX / m.voxelPx * 256.0));
	put32((int32_t)lround(-m.originZ / m.voxelPx * 256.0));
	put32(zs * 256);
	for (int32_t v : xoff) put32(v);
	for (int16_t v : xyoff) put16(v);
	out.insert(out.end(), data.begin(), data.end());
	for (int i = 0; i < 768; i++) out.push_back((uint8_t)(m.palette[i] >> 2));
	return out;
}

//==========================================================================
//
// Everything for one film.
//
//==========================================================================

uint32_t VoxFilmCrc(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++)
	{
		c ^= p[i];
		for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
	}
	return ~c;
}

namespace
{
#include "roth_voxelize_table.h"
}

int VoxFittedCount()
{
	return (int)(sizeof(VOX_FITTED) / sizeof(VOX_FITTED[0])) - 1;     // the last entry is the end marker
}

const VoxFittedFilm *VoxFindFitted(uint32_t crc, uint32_t size)
{
	for (int i = 0; i < VoxFittedCount(); i++)
		if (VOX_FITTED[i].crc == crc && VOX_FITTED[i].size == size) return &VOX_FITTED[i];
	return nullptr;
}

VoxCamera VoxCameraFromFitted(const VoxFittedFilm &f)
{
	VoxCamera c;
	c.period = f.period; c.direction = f.direction; c.elevation = Deg(f.elevationDeg);
	c.axisX = f.axisX; c.axisY = f.axisY; c.roll = Deg(f.rollDeg);
	c.invDistance = f.distance > 0 ? 1.0 / f.distance : 0.0;
	c.firstFrame = f.firstFrame; c.lastFrame = f.lastFrame;
	return c;
}

VoxFittedFilm VoxFittedFromCamera(uint32_t crc, uint32_t size, const VoxCamera &c, bool open)
{
	VoxFittedFilm f;
	f.crc = crc; f.size = size; f.period = (float)c.period; f.direction = (int8_t)c.direction; f.elevationOpen = open ? 1 : 0;
	f.firstFrame = (int16_t)c.firstFrame; f.lastFrame = (int16_t)c.lastFrame;
	f.elevationDeg = (float)(c.elevation * 180 / PI); f.axisX = (float)c.axisX; f.axisY = (float)c.axisY; f.rollDeg = (float)(c.roll * 180 / PI);
	f.distance = c.invDistance > 0 ? (float)(1.0 / c.invDistance) : 0.0f;
	return f;
}

VoxResult VoxelizeCloseup(const uint8_t *gdv, size_t size, const VoxEstimateOptions &est, const VoxCarveOptions &carve, bool useTable)
{
	VoxResult r;
	VoxFilm film;
	if (!VoxDecodeFilm(gdv, size, film, &r.error)) return r;
	std::vector<uint8_t> masks;
	VoxBuildMasks(film, masks);
	const VoxFittedFilm *fit = useTable ? VoxFindFitted(VoxFilmCrc(gdv, size), (uint32_t)size) : nullptr;
	if (fit)
	{
		r.fromTable = true;
		r.geometry.turntable = true;
		r.geometry.camera = VoxCameraFromFitted(*fit);
		r.geometry.elevationOpen = fit->elevationOpen != 0;
	}
	else r.geometry = VoxEstimateTurntable(film, masks, est);
	if (!r.geometry.turntable) { r.error = r.geometry.note; return r; }
	if (!VoxCarve(film, masks, r.geometry.camera, carve, r.model)) { r.error = "carving left nothing"; return r; }
	r.ok = true;
	return r;
}

} // namespace roth
