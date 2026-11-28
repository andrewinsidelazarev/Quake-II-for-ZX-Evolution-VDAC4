// =====================================================================================================
// tb_raster.cpp — стенд растеризатора этапа B (bz_raster) против C-модели, на трассе Quake 2.
// =====================================================================================================
//
//   tb_raster.exe <трасса.bzcmd> [кадров] [задержки выхода 0/1]
//
// КАК ПРОВЕРЯЕТСЯ
//   C-модель (bomzh.c) выполняет кадр трассы как обычно, а крючки (bz_set_hooks) записывают каждый
//   многоугольник, который попал в её растеризатор (и из POLY, и из TRIS после постановки), и каждую его
//   точку: x, y, zi, s, t, l — в порядке выдачи. Потом тот же многоугольник подаётся на вход bz_raster, и
//   его точки сравниваются с записанными по одной: число точек, порядок и все значения — до бита.
//   Память здесь не нужна: растеризатор ничего не читает и не пишет.
//
// ЗАДЕРЖКИ ВЫХОДА
//   С ключом 1 стенд иногда не берёт точку (out_ready = 0) — так проверяется, что растеризатор держит
//   точку и не теряет и не повторяет точки, когда следующая ступень занята (а она будет занята при
//   промахах кэша текстур).
//
// Итог — «ПРОШЛО» с числом многоугольников, точек и тактов, или первое расхождение с подробностями.
#include "Vbz_raster.h"
#ifdef TB_DBG
#include "Vbz_raster___024root.h"   // внутренние сигналы (сборка с --public-flat-rw)
#endif
#include "verilated.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {
#include "bomzh.h"
}

double sc_time_stamp () { return 0; }

struct Pix
{
	int     x, y;
	int32_t zi, s, t, l;
};

struct Poly
{
	uint32_t xy[16];
	int      n, mode;
	int32_t  g[4][3];
	int      sx0, sy0, sx1, sy1;
	std::vector<Pix> pix;
};

static std::vector<Poly> polys;

static void hook_poly (void *, const uint32_t *xy, int n, int mode, const int32_t g[4][3], int sx0, int sy0,
                       int sx1, int sy1)
{
	Poly p;
	for (int i = 0; i < 16; i++)
		p.xy[i] = i < n ? xy[i] : 0;
	p.n = n;
	p.mode = mode;
	for (int a = 0; a < 4; a++)
		for (int b = 0; b < 3; b++)
			p.g[a][b] = g[a][b];
	p.sx0 = sx0; p.sy0 = sy0; p.sx1 = sx1; p.sy1 = sy1;
	polys.push_back (p);
}

static void hook_pixel (void *, int x, int y, int32_t zi, int32_t s, int32_t t, int32_t l)
{
	polys.back ().pix.push_back ({ x, y, zi, s, t, l });
}

int main (int argc, char **argv)
{
	if (argc < 2) {
		fprintf (stderr, "tb_raster <трасса.bzcmd> [кадров] [задержки 0/1]\n");
		return 2;
	}
	FILE *f = fopen (argv[1], "rb");
	if (!f) {
		fprintf (stderr, "нет файла %s\n", argv[1]);
		return 2;
	}
	const int max_frames = argc > 2 ? atoi (argv[2]) : 1 << 30;
	const bool stalls = argc > 3 && atoi (argv[3]);

	Verilated::commandArgs (argc, argv);
	Vbz_raster *top = new Vbz_raster;
	bz_t *model = bz_create (8u << 20);
	bz_set_hooks (model, hook_poly, hook_pixel, nullptr);

	top->rst = 1;
	top->in_valid = 0;
	top->out_ready = 1;
	for (int i = 0; i < 4; i++) {
		top->clk = 0; top->eval ();
		top->clk = 1; top->eval ();
	}
	top->rst = 0;

	uint64_t cycles = 0, npoly = 0, npix = 0;
	uint64_t bubble[10] = { 0 };
	uint32_t lfsr = 0xACE1u;
	std::vector<uint32_t> words;
	int frame = 0;

	while (frame < max_frames) {
		uint32_t n;
		int32_t clr;
		if (fread (&n, 4, 1, f) != 1 || fread (&clr, 4, 1, f) != 1)
			break;
		words.resize (n);
		if (fread (words.data (), 4, n, f) != n)
			break;
		polys.clear ();
		if (bz_exec (model, words.data (), n)) {
			printf ("кадр %d: модель отвергла поток\n", frame);
			return 2;
		}
		uint64_t c0 = cycles;

		// Многоугольники кадра — потоком: следующий подаётся, как только растеризатор берёт прежний.
		// Точки идут общим потоком в том же порядке (pi — чей многоугольник сейчас на выходе, got — какая
		// его точка); за последней точкой каждого многоугольника (и у пустого) — метка конца out_end с его
		// меткой out_tag (стенд даёт метку = номер многоугольника & 1).
		// Многоугольник подаётся как в ядре: сначала его вершины и градиенты через порт загрузки (слово за
		// такт, пока in_ready = 1), потом in_valid. lpos — сколько слов текущего уже загружено.
		size_t feed = 0, pi = 0, got = 0;
		int lpos = 0;
		uint64_t guard = 0;
		auto load = [&] (size_t k) {
			const Poly &q = polys[k];
			top->in_n = q.n;
			top->in_mode = q.mode;
			top->in_sx0 = (uint16_t)q.sx0; top->in_sy0 = (uint16_t)q.sy0;
			top->in_sx1 = (uint16_t)q.sx1; top->in_sy1 = (uint16_t)q.sy1;
			top->in_tag = k & 1;
			top->in_valid = 0;
			lpos = 0;
		};
		auto load_word = [&] (size_t k, int w, uint32_t &idx, uint32_t &data) {
			const Poly &q = polys[k];
			if (w < q.n) {
				idx = (uint32_t)w;
				data = q.xy[w];
			} else {
				const int j = w - q.n;
				idx = 16u + (uint32_t)j;
				data = (uint32_t)q.g[j / 3][j % 3];
			}
		};
		if (!polys.empty ())
			load (0);
		for (;;) {
			if (feed == polys.size () && pi == polys.size () && top->idle)
				break;
			top->clk = 0;
			// загрузка слов многоугольника feed: n вершин и 12 градиентов, потом in_valid
			bool ld = false;
			top->ld_we = 0;
			if (feed < polys.size () && !top->in_valid) {
				if (lpos < polys[feed].n + 12) {
					uint32_t idx = 0, data = 0;
					load_word (feed, lpos, idx, data);
					top->ld_idx = idx;
					top->ld_data = data;
					top->ld_we = 1;
					ld = true;
				} else
					top->in_valid = 1;
			}
			if (stalls) {
				lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
				top->out_ready = (lfsr & 3) != 0;
			}
			top->eval ();
			const bool take_in = top->in_valid && top->in_ready;
			const bool take_ld = ld && top->in_ready;
			const bool take_out = top->out_valid && top->out_ready;
			if (take_out && top->out_end) {
				if (pi >= polys.size () || got != polys[pi].pix.size () || top->out_tag != (pi & 1)) {
					printf ("кадр %d: метка конца не к месту (многоугольник %zu из %zu, точка %zu, метка %d)\n", frame,
					        pi, polys.size (), got, (int)top->out_tag);
					if (pi < polys.size ()) {   // 27.11.2026: что за многоугольник (вершины Q12.4, ножницы, точек у модели)
						const Poly &q = polys[pi];
						printf ("  n %d, режим %04x, ножницы (%d,%d)-(%d,%d), точек у модели %zu\n", q.n, q.mode,
						        q.sx0, q.sy0, q.sx1, q.sy1, q.pix.size ());
						for (int v = 0; v < q.n; v++)
							printf ("  v%d: x %d (%.2f) y %d (%.2f)\n", v, (int)(int16_t)(q.xy[v] & 0xFFFF),
							        (int16_t)(q.xy[v] & 0xFFFF) / 16.0, (int)(int16_t)(q.xy[v] >> 16),
							        (int16_t)(q.xy[v] >> 16) / 16.0);
					}
					return 1;
				}
				npix += got;
				pi++;
				got = 0;
			} else if (take_out) {
				if (pi >= polys.size () || got >= polys[pi].pix.size () || top->out_tag != (pi & 1)) {
					printf ("кадр %d: лишняя точка (%d, %d) или чужая метка\n", frame, top->out_x, top->out_y);
					return 1;
				}
				const Poly &p = polys[pi];
				const Pix &e = p.pix[got];
				if ((int)(int16_t)top->out_x != e.x || (int)(int16_t)top->out_y != e.y ||
				    (int32_t)top->out_zi != e.zi || (int32_t)top->out_s != e.s || (int32_t)top->out_t != e.t ||
				    (int32_t)top->out_l != e.l) {
					printf ("кадр %d, многоугольник %zu (n %d, режим %04x), точка %zu из %zu: РАСХОЖДЕНИЕ\n"
					        "  модель: x %d y %d zi %08x s %08x t %08x l %08x\n"
					        "  RTL:    x %d y %d zi %08x s %08x t %08x l %08x\n",
					        frame, pi, p.n, p.mode, got, p.pix.size (), e.x, e.y, e.zi, e.s, e.t, e.l,
					        (int16_t)top->out_x, (int16_t)top->out_y, top->out_zi, top->out_s, top->out_t,
					        top->out_l);
					return 1;
				}
				got++;
			}
			// где теряются такты: точка не выходит — по состоянию ступеней (выход dbg)
			if (!(top->dbg & 0x80)) {
				const unsigned d = top->dbg, e = d >> 13, i = (d >> 11) & 3, c = (d >> 8) & 7;
				int k;
				if (c != 0)                     k = 0;   // C: умножения и шаг
				else if (!(d & 0x10))           k = 1;   // C ждёт частное делителя
				else if (i == 1)                k = 2;   // I: значения начала отрезка
				else if (i == 2 || i == 3)      k = 3;   // I ждёт делитель или место
				else if (!(d & 0x20))           k = 4;   // очередь отрезков не пуста (I вот-вот возьмёт)
				else if (e == 3 && (d & 0x0C))  k = 5;   // R1: деления начала ребра
				else if (e == 3)                k = 6;   // R1: строки, сдвиг цепочек
				else if (e == 1 || e == 2)      k = 7;   // R1: верх/низ, начало
				else if (e == 4)                k = 8;   // R1: конец многоугольника
				else                            k = 9;   // R1 ждёт вход (стенд или банк занят)
				bubble[k]++;
			}
			top->clk = 1;
			top->eval ();
			cycles++;
#ifdef TB_DBG
			// 27.11.2026: трасса регистров R1 на кадре RASTER_DBG (первые 400 тактов кадра)
			if (getenv ("RASTER_DBG") && frame == atoi (getenv ("RASTER_DBG")) && cycles - c0 < 400) {
				auto *r = top->rootp;
				printf ("t%llu e_st %d ey %d ey1 %d top %d bot %d a %d>%d b %d>%d ph %d/%d dv %d/%d de %d/%d dq %d/%d sp_v %d l %d r %d pn %d\n",
				        (unsigned long long)(cycles - c0), r->bz_raster__DOT__e_st, (int)r->bz_raster__DOT__ey, (int)r->bz_raster__DOT__ey1,
				        r->bz_raster__DOT__top, r->bz_raster__DOT__bot, r->bz_raster__DOT__a0, r->bz_raster__DOT__a1,
				        r->bz_raster__DOT__b0, r->bz_raster__DOT__b1, r->bz_raster__DOT__ph[0], r->bz_raster__DOT__ph[1],
				        r->bz_raster__DOT__dv[0], r->bz_raster__DOT__dv[1], r->bz_raster__DOT__de[0], r->bz_raster__DOT__de[1],
				        (int)r->bz_raster__DOT__dq[0], (int)r->bz_raster__DOT__dq[1], r->bz_raster__DOT__sp_v,
				        (int)(int16_t)r->bz_raster__DOT__sp_l, (int)(int16_t)r->bz_raster__DOT__sp_r, r->bz_raster__DOT__pn);
			}
#endif
			if (take_ld)
				lpos++;
			if (take_in) {
				feed++;
				npoly++;
				if (feed < polys.size ())
					load (feed);
				else
					top->in_valid = 0;
			}
			if (++guard > 500000000ull) {
				printf ("кадр %d: растеризатор завис (подано %zu из %zu, на выходе многоугольник %zu, точка %zu)\n",
				        frame, feed, polys.size (), pi, got);
				return 1;
			}
		}
		if (frame % 100 == 0)
			printf ("кадр %d: многоугольников %zu, тактов %llu — совпало\n", frame, polys.size (),
			        (unsigned long long)(cycles - c0));
		fflush (stdout);
		frame++;
	}
	printf ("ПРОШЛО: %d кадров, многоугольников %llu, точек %llu, тактов %llu (%.2f такта на точку)\n", frame,
	        (unsigned long long)npoly, (unsigned long long)npix, (unsigned long long)cycles,
	        npix ? (double)cycles / npix : 0.0);
	static const char *bn[10] = { "C: умножения/шаг", "C ждёт делитель", "I: начало отрезка",
	                              "I ждёт делитель/место", "очередь отрезков", "R1: деления ребра",
	                              "R1: строки", "R1: верх/низ", "R1: конец", "R1 ждёт вход/банк" };
	printf ("такты без точки на выходе (на точку):\n");
	for (int k = 0; k < 10; k++)
		printf ("  %-24s %.3f\n", bn[k], npix ? (double)bubble[k] / npix : 0.0);
	delete top;
	bz_free (model);
	return 0;
}
