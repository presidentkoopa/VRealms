// roth_wicks.cpp -- see roth_wicks.h. Engine-free.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roth_wicks.h"
#include "roth_das.h"
#include "roth_raw.h"

#include <math.h>
#include <algorithm>
#include <map>
#include <set>

namespace roth
{

static constexpr double kTwoPi = 6.28318530717958647692;

WickPicture WickPictureFromImage(const Image &img, const AnchorOffset &anchor)
{
	WickPicture p;
	if (!img.ok()) return p;
	// ReadSpriteModifiers (roth_texture_object.cpp), the same reading.
	if (img.modifier & IM_HALF_SIZE)
	{
		const int s = (img.imageType >> 5) & 3;
		p.upp = s == 0 ? 1.0 : 2.0 * double(1 << s);
	}
	p.hang = (img.modifier & IM_HANG) != 0;
	p.shift = double((int16_t)(uint16_t)((uint16_t)((img.modifier & 0x0f) * 2) + (uint16_t)anchor.y));
	// The quarter turn: stored rows are the drawn picture's columns (frames[f][col * img.width + row]).
	p.W = img.height;
	p.H = img.width;
	for (const auto &fr : img.frames)
	{
		if (fr.size() < (size_t)p.W * p.H) continue;
		std::vector<uint8_t> v((size_t)p.W * p.H);
		for (int x = 0; x < p.W; x++)
			for (int y = 0; y < p.H; y++)
				v[(size_t)y * p.W + x] = fr[(size_t)x * img.width + y];
		p.frames.push_back(std::move(v));
	}
	return p;
}

namespace
{

struct Group { std::vector<int> texels; int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; };

std::vector<Group> Connected(const std::vector<uint8_t> &mask, int W, int H)
{
	std::vector<int> lab((size_t)W * H, -1);
	std::vector<Group> out;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			const int i0 = y * W + x;
			if (!mask[i0] || lab[i0] >= 0) continue;
			const int id = (int)out.size();
			out.push_back({});
			std::vector<int> st = { i0 };
			lab[i0] = id;
			while (!st.empty())
			{
				const int i = st.back(); st.pop_back();
				out[id].texels.push_back(i);
				const int cx = i % W, cy = i / W;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++)
					{
						const int nx = cx + dx, ny = cy + dy;
						if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
						const int j = ny * W + nx;
						if (!mask[j] || lab[j] >= 0) continue;
						lab[j] = id;
						st.push_back(j);
					}
			}
		}
	for (Group &g : out)
		for (int i : g.texels)
		{
			const int x = i % W, y = i / W;
			g.x0 = std::min(g.x0, x); g.x1 = std::max(g.x1, x); g.y0 = std::min(g.y0, y); g.y1 = std::max(g.y1, y);
		}
	return out;
}

} // namespace

WickModel BuildWickModel(const WickPicture &un, const WickPicture &lit)
{
	WickModel m;
	if (!un.ok() || !lit.ok()) { m.why = "a picture did not decode"; return m; }
	if (un.frames.size() != 1) { m.why = "the unlit picture animates"; return m; }
	if (lit.frames.size() < 2) { m.why = "the lit picture does not animate"; return m; }
	if (un.upp != lit.upp) { m.why = "different texel sizes"; return m; }
	if (un.shift != lit.shift) { m.why = "different anchor shifts"; return m; }
	if (un.hang || lit.hang) { m.why = "a hanging picture"; return m; }

	m.W = lit.W; m.H = lit.H; m.frames = (int)lit.frames.size(); m.upp = lit.upp;
	// Rule 1: the engine's offsets are W / 2 (integer) across and H down (roth_texture_object.cpp).
	m.dx = lit.W / 2 - un.W / 2;
	m.dy = lit.H - un.H;
	const size_t N = (size_t)m.W * m.H;
	m.unlit.assign(N, 0);
	for (int y = 0; y < un.H; y++)
		for (int x = 0; x < un.W; x++)
		{
			const uint8_t v = un.At(0, x, y);
			const int lx = x + m.dx, ly = y + m.dy;
			if (lx < 0 || ly < 0 || lx >= m.W || ly >= m.H) { if (v != 0) { m.why = "an unlit texel falls outside the lit picture"; return m; } continue; }
			m.unlit[(size_t)ly * m.W + lx] = v;
		}
	m.lit = lit.frames;

	// Rule 2 and 3: changed, flickering.
	std::vector<uint8_t> changed(N, 0), flick(N, 0);
	for (size_t i = 0; i < N; i++)
	{
		for (int f = 0; f < m.frames; f++)
		{
			if (m.lit[f][i] != m.unlit[i]) changed[i] = 1;
			if (f > 0 && m.lit[f][i] != m.lit[0][i]) flick[i] = 1;
		}
		m.changed += changed[i];
		m.flicker += flick[i];
	}
	for (size_t i = 0; i < N; i++)
		if (flick[i] && !changed[i]) { m.why = "a flickering texel equal to the unlit picture"; return m; }   // cannot happen: a flicker is a difference

	std::vector<Group> g = Connected(flick, m.W, m.H);
	if (g.empty()) { m.why = "no flames"; return m; }
	// Groups whose boxes meet are one flame.
	std::vector<int> root(g.size());
	for (size_t i = 0; i < g.size(); i++) root[i] = (int)i;
	auto find = [&](int i) { while (root[i] != i) i = root[i] = root[root[i]]; return i; };
	for (size_t i = 0; i < g.size(); i++)
		for (size_t j = i + 1; j < g.size(); j++)
			if (g[i].x0 <= g[j].x1 && g[j].x0 <= g[i].x1 && g[i].y0 <= g[j].y1 && g[j].y0 <= g[i].y1) root[find((int)i)] = find((int)j);
	std::map<int, std::vector<int>> by;
	for (size_t i = 0; i < g.size(); i++)
		for (int t : g[i].texels) by[find((int)i)].push_back(t);
	for (auto &kv : by)
	{
		WickFlame f;
		f.x0 = 1 << 30; f.y0 = 1 << 30; f.x1 = -1; f.y1 = -1;
		for (int t : kv.second)
		{
			const int x = t % m.W, y = t / m.W;
			f.x0 = std::min(f.x0, x); f.x1 = std::max(f.x1, x); f.y0 = std::min(f.y0, y); f.y1 = std::max(f.y1, y);
			f.cx += x + 0.5; f.cy += y + 0.5; f.texels++;
		}
		f.cx /= f.texels; f.cy /= f.texels;
		int wn = 0;
		for (int t : kv.second) if (t / m.W == f.y1) { f.wx += t % m.W + 0.5; wn++; }
		f.wx /= wn;
		f.wy = f.y1 + 1;
		const int left = m.W / 2;
		f.across = (f.wx - left) * m.upp;
		f.up = (m.H - f.wy) * m.upp;
		f.flameAcross = (f.cx - left) * m.upp;
		f.flameUp = (m.H - f.cy) * m.upp;
		m.flames.push_back(f);
	}
	std::sort(m.flames.begin(), m.flames.end(), [](const WickFlame &a, const WickFlame &b) { return a.x0 != b.x0 ? a.x0 < b.x0 : a.y0 < b.y0; });
	if ((int)m.flames.size() > kMaxWicks) { m.why = "more flames than kMaxWicks"; m.flames.clear(); return m; }

	// Rule 4: every changed texel to exactly one flame.
	m.owner.assign(N, -1);
	m.perFlame.assign(m.flames.size(), 0);
	for (size_t i = 0; i < N; i++)
	{
		if (!changed[i]) continue;
		const int x = (int)(i % m.W), y = (int)(i / m.W);
		int in = -1, nIn = 0;
		for (size_t k = 0; k < m.flames.size(); k++)
			if (x >= m.flames[k].x0 && x <= m.flames[k].x1) { in = (int)k; nIn++; }
		int who = -1;
		if (nIn == 1) { who = in; m.inColumn++; }
		else
		{
			double best = 1e30, bestV = 1e30;
			int atBest = 0;
			for (size_t k = 0; k < m.flames.size(); k++)
			{
				if (nIn > 1 && !(x >= m.flames[k].x0 && x <= m.flames[k].x1)) continue;   // within the spans that hold it
				const double av = fabs(x + 0.5 - m.flames[k].wx), vv = fabs(y + 0.5 - m.flames[k].wy);
				if (av < best - 1e-9) { best = av; bestV = vv; who = (int)k; atBest = 1; }
				else if (av <= best + 1e-9)
				{
					atBest++;
					if (vv < bestV - 1e-9) { bestV = vv; who = (int)k; }   // equal across: nearer vertically; still equal: the left one stays
				}
			}
			if (atBest > 1) m.ties++;
			m.byNearest++;
		}
		m.owner[i] = (int8_t)who;
		m.perFlame[who]++;
	}
	m.ok = true;
	return m;
}

void ComposeWicks(const WickModel &m, uint32_t mask, int frame, std::vector<uint8_t> &out)
{
	const size_t N = (size_t)m.W * m.H;
	out.assign(N, 0);
	if (!m.ok || frame < 0 || frame >= m.frames) return;
	const std::vector<uint8_t> &L = m.lit[frame];
	for (size_t i = 0; i < N; i++)
	{
		const int o = m.owner[i];
		out[i] = (o >= 0 && (mask & (1u << o))) ? L[i] : m.unlit[i];
	}
}

std::vector<LightableObject> FindLightableObjects(const Map &m)
{
	std::vector<LightableObject> out;
	for (size_t ci = 0; ci < m.commands.size(); ci++)
	{
		const Command &t = m.commands[ci];
		if ((t.opcode & 0x7f) != 0x08 || !t.isTrigger || t.key == 0) continue;
		// The chain from its start, NEXT by NEXT, as far as the first 0x0d with key 0 (the clicked object's own
		// picture). An 0x27 before it names the item the use needs.
		int item = 0;
		uint16_t lit = 0;
		bool found = false;
		std::set<int> seen;
		int i = t.chainStart;
		for (int steps = 0; i > 0 && i <= (int)m.commands.size() && steps < 128 && seen.insert(i).second; steps++)
		{
			const Command &c = m.commands[i - 1];
			const uint8_t op = c.opcode & 0x7f;
			if (op == 0x27 && item == 0) item = c.key;
			if (op == 0x0d && c.key == 0) { lit = c.Word(0x0c); found = true; break; }
			if (c.isTrigger) break;
			i = c.nextIndex;
		}
		if (!found) continue;
		for (size_t s = 0; s < m.objects.size(); s++)
			for (size_t k = 0; k < m.objects[s].size(); k++)
			{
				const Object &o = m.objects[s][k];
				if (o.commandID != t.key) continue;
				LightableObject L;
				L.sector = (int)s; L.index = (int)k;
				L.unlitWord = (uint16_t)(o.textureIndex | (o.textureSource << 8));
				L.litWord = lit;
				L.clickId = t.key;
				L.trigger = (uint16_t)(ci + 1);
				L.item = item;
				if (L.unlitWord != L.litWord) out.push_back(L);
			}
	}
	return out;
}

int WickLighting::Flames(uint32_t near)
{
	near &= Full();
	const uint32_t edges = near & ~mNear;
	mNear = near;
	if (!mStarted)
	{
		for (int k = 0; k < mWicks; k++)
			if (edges & (1u << k)) { mUsesAsked++; return k; }
		return -1;
	}
	// Started: every unlit wick a flame is at catches (an edge is not needed: a flame resting on a wick
	// when the lighting starts lights it on the next tic).
	for (int k = 0; k < mWicks; k++)
		if ((near & (1u << k)) && !(mMask & (1u << k))) { mMask |= 1u << k; mLitAt[k] = mTics; }
	return -1;
}

void WickLighting::Started(int k)
{
	if (mStarted || k < 0 || k >= mWicks) return;
	mStarted = true;
	mTics = 0;
	mMask = 1u << k;
	mLitAt[k] = 0;
}

void WickLighting::Tick()
{
	if (!mStarted) return;
	mTics++;
	if (mTics >= CATCH_TICS && mMask != Full())
	{
		for (int k = 0; k < mWicks; k++) if (!(mMask & (1u << k))) mLitAt[k] = mTics;
		mMask = Full();
		mCaught = true;
	}
}

double WickFlicker(double now, int k)
{
	const double ph = 1.7 * k + 0.4;
	const double v = 0.5 + 0.25 * sin(kTwoPi * 5.3 * now + ph) + 0.15 * sin(kTwoPi * 8.9 * now + 2.1 * ph) + 0.10 * sin(kTwoPi * 2.3 * now + 3.3 * ph);
	return 0.78 + 0.22 * std::clamp(v, 0.0, 1.0);
}

bool CandleLight(const WickModel &m, const double *litAgeSec, double now, CandleLightOut &out)
{
	out = CandleLightOut();
	double wsum = 0, ramped = 0, ax = 0, au = 0;
	for (int k = 0; k < m.Wicks(); k++)
	{
		if (litAgeSec[k] < 0) continue;
		const double ramp = std::min(1.0, litAgeSec[k] / kLightRampSec);
		const double w = ramp * WickFlicker(now, k);
		ax += w * m.flames[k].flameAcross;
		au += w * m.flames[k].flameUp;
		wsum += w;
		ramped += ramp;
		out.lit++;
	}
	if (wsum <= 1e-6) return false;
	out.across = ax / wsum;
	out.up = au / wsum;
	out.intensity = wsum;
	out.radius = std::max(1.0, kLightRadius * sqrt(ramped));
	return true;
}

} // namespace roth
