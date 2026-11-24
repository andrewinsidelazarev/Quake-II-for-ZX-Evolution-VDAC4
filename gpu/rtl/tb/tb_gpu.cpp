// =====================================================================================================
// tb_gpu.cpp — стенд этапа B: весь «бомж-3D» (bz_gpu + мост sd_bridge) против C-модели (bomzh.c).
// =====================================================================================================
//
//   tb_gpu.exe <трасса.bzcmd> [кадров] [с_какого_кадра_сравнивать] [зерно]
//
// ЧТО ПРОВЕРЯЕТСЯ
//   Как у стенда этапа A (tb_bz.cpp): кадр из трассы Quake 2 выполняет C-модель (эталон) и RTL, после кадра
//   вся память RTL (8 МБ) сравнивается с памятью модели — до байта. Разница с этапом A: память здесь не
//   «идеальная», а настоящий мост SDRAM sd_bridge (из FT-ядра 1.45) с моделью контроллера на C++: шесть
//   клиентов этапа B (тексели, куски z и цвета, alphamap, lightmap, ядро) идут через его очереди, как на
//   плате. Задержки контроллера случайные (зерно — аргумент), плюс «регенерация» SDRAM время от времени —
//   так проверяется, что результат не зависит от того, кто из клиентов успел раньше.
//
// ДВА ТАКТА
//   Мост живёт в двух тактах: clk (60,75 МГц — ядро) и clk_sd (121,5 МГц — контроллер). Стенд «качает»
//   clk_sd, а clk меняется на каждом втором его фронте — фронты clk совпадают с фронтами clk_sd, как у
//   PLL на плате.
//
// МОДЕЛЬ КОНТРОЛЛЕРА (интерфейс sdram_burst, сторона c_* моста)
//   Свободен и есть c_req — c_ack в этом такте; запись: через 2 такта 8·(len + 1) тактов c_wr_take,
//   слово c_wdata/c_wmask берётся в тот же такт; чтение: через 5…8 тактов 8·(len + 1) тактов c_rd_valid
//   со словами памяти и меткой c_rd_tag (тексель или нет — её дал мост в c_tag). После операции — пауза
//   2…3 такта (закрытие строки). Каждые ~1560 тактов — 8 тактов регенерации.
#include "Vbz_gpu_sim.h"
#ifdef TB_DBG
#include "Vbz_gpu_sim___024root.h"     // внутренние сигналы (сборка с --public-flat-rw)
#endif
#include "verilated.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "bomzh.h"
}

static const uint32_t MEM = 8u << 20;

double sc_time_stamp () { return 0; }

struct Ctl
{
	enum { IDLE, WGAP, WDATA, RGAP, RDATA, REC } st = IDLE;
	int      wait = 0, n = 0, idx = 0, refresh = 0;
	bool     tag = false;
	uint32_t a = 0, lfsr = 1;
	uint64_t t = 0, ops = 0, busy = 0;

	int rnd (int m)
	{
		lfsr = lfsr * 1103515245u + 12345u;
		return (int)((lfsr >> 16) % (uint32_t)m);
	}

	// выходы модели на этот такт clk_sd (вызывается после фронта, до eval)
	void outputs (Vbz_gpu_sim *top, const std::vector<uint8_t> &mem)
	{
		top->c_ack = 0;
		top->c_wr_take = 0;
		top->c_rd_valid = 0;
		t++;
		if (t % 1560 == 0)
			refresh = 8;
		if (st != IDLE)
			busy++;
		switch (st) {
		case IDLE:
			if (refresh > 0) {
				refresh--;
				break;
			}
			if (top->c_req) {
				top->c_ack = 1;
				a = top->c_addr & ~7u;
				n = 8 * (top->c_len + 1);
				tag = top->c_tag;
				idx = 0;
				ops++;
				if (top->c_we) {
					st = WGAP;
					wait = 2;
				} else {
					st = RGAP;
					wait = 5 + rnd (4);
				}
			}
			break;
		case WGAP:
			if (--wait == 0)
				st = WDATA;
			break;
		case WDATA:
			top->c_wr_take = 1;          // слово — после eval (sample)
			break;
		case RGAP:
			if (--wait == 0)
				st = RDATA;
			break;
		case RDATA: {
			const uint32_t b = ((a + (uint32_t)idx) * 4u) & (MEM - 1);
			top->c_rd_valid = 1;
			top->c_rd_data = mem[b] | (mem[b + 1] << 8) | (mem[b + 2] << 16) | ((uint32_t)mem[b + 3] << 24);
			top->c_rd_tag = tag;
			if (++idx == n) {
				st = REC;
				wait = 2 + rnd (2);
			}
			break;
		}
		case REC:
			if (--wait == 0)
				st = IDLE;
			break;
		}
	}

	// после eval: слово записи этого такта
	void sample (Vbz_gpu_sim *top, std::vector<uint8_t> &mem)
	{
		if (st == WDATA && top->c_wr_take) {        // (в такт перехода WGAP → WDATA слова ещё нет)
			const uint32_t b = ((a + (uint32_t)idx) * 4u) & (MEM - 1);
			for (int k = 0; k < 4; k++)
				if (top->c_wmask & (1 << k))
					mem[b + k] = (uint8_t)(top->c_wdata >> (8 * k));
			if (++idx == n) {
				st = REC;
				wait = 3;
			}
		}
	}
};

int main (int argc, char **argv)
{
	if (argc < 2) {
		fprintf (stderr, "tb_gpu <трасса.bzcmd> [кадров] [сравнивать_с] [зерно]\n");
		return 2;
	}
	FILE *f = fopen (argv[1], "rb");
	if (!f) {
		fprintf (stderr, "нет файла %s\n", argv[1]);
		return 2;
	}
	const int max_frames = argc > 2 ? atoi (argv[2]) : 1 << 30;
	const int cmp_from = argc > 3 ? atoi (argv[3]) : 0;
	Ctl ctl;
	ctl.lfsr = argc > 4 ? (uint32_t)atoi (argv[4]) : 1u;

	Verilated::commandArgs (argc, argv);
	Vbz_gpu_sim *top = new Vbz_gpu_sim;
	bz_t *model = bz_create (MEM);
	std::vector<uint8_t> mem (MEM, 0);
	std::vector<uint32_t> words;

	uint64_t clk_cycles = 0;
	int ph = 0;
	// один такт clk_sd; clk — на каждом втором. Возвращает true, если был фронт clk
	auto tick = [&] () -> bool {
		const bool rise = ph == 0;
		top->clk_sd = 1;
		top->clk = rise ? 1 : 0;
		top->eval ();
		ctl.outputs (top, mem);
		top->eval ();
		ctl.sample (top, mem);
		top->clk_sd = 0;
		top->eval ();
		ph ^= 1;
		if (rise)
			clk_cycles++;
		return rise;
	};

	top->swap_ack = 1;
	top->rst = 1;
	top->cmd_valid = 0;
	top->c_ack = 0; top->c_wr_take = 0; top->c_rd_valid = 0;
	for (int i = 0; i < 16; i++)
		tick ();
	top->rst = 0;

	// счётчики «стопа» конвейера точки (dbg_pix): ступень D (тексель), E (z), F (цвет)
	uint64_t st_d = 0, st_e = 0, st_f = 0, px_out = 0;
	uint64_t tot_px = 0;
	int frame = 0;

	// SPEC 0.4 (24.11.2026): копия SDRAM в окно (bz_rdwin) посреди рисования. Что копировать — кусок,
	// записанный WRITE в прошлых порциях и не задетый этой (рисование пишет только кадры и z, WRITE — нет);
	// ждём — его байты из памяти модели до этой порции.
	struct Span { uint32_t a, n; };
	std::vector<Span> wr_prev;               // куски WRITE прошлой порции
	uint64_t rd_checks = 0, rd_bytes = 0;
	top->rd_go = 0; top->rd_addr = 0; top->rd_len = 0; top->win_raddr = 0;

	while (frame < max_frames) {
		uint32_t n;
		int32_t clr;
		if (fread (&n, 4, 1, f) != 1 || fread (&clr, 4, 1, f) != 1)
			break;
		words.resize (n);
		if (fread (words.data (), 4, n, f) != n)
			break;

		if (clr >= 0) {
			uint32_t fb = bz_draw_fb (model), npx = (uint32_t)(bz_stride (model) * bz_height (model));
			bz_debug_clear (model, (uint8_t)clr);
			for (uint32_t k = 0; k < npx; k++)
				mem[(fb + k) & (MEM - 1)] = (uint8_t)clr;
		}
		// куски WRITE этой порции (разбор длины — как bz_exec; неверное слово найдёт сам bz_exec ниже)
		std::vector<Span> wr_now;
		for (uint32_t i = 0; i < n;) {
			const uint32_t w0 = words[i], op = w0 >> 24;
			uint32_t len;
			switch (op) {
			case BZ_TEX: len = 3; break;   case BZ_LMAP: len = 5; break;
			case BZ_POLY: len = 1 + (w0 & 0xFF) + 9 + ((((w0 >> 8) & 0xFFFF) & BZ_M_LIGHT) == BZ_L_VERT ? 3 : 0); break;
			case BZ_POINT: len = 3; break; case BZ_PIC: len = 7; break;   case BZ_FILL: len = 4; break;
			case BZ_FRAME: len = 2; break; case BZ_TIME: len = 2; break;  case BZ_SETUP: len = 9; break;
			case BZ_SCISSOR: len = 3; break; case BZ_MARK: len = 2; break;
			case BZ_WRITE: len = 2 + (w0 & 0xFFFFFF); if (i + 1 < n) wr_now.push_back ({ words[i + 1], 4 * (w0 & 0xFFFFFF) }); break;
			case BZ_VBUF: len = 1 + 3 * (w0 & 0x3FF); break; case BZ_STBUF: len = 1 + (w0 & 0x3FF); break;
			case BZ_TRIS: len = 1 + 2 * (w0 & 0xFF); break;
			default: len = n; break;
			}
			i += len;
		}
		// что копировать в окно в этой порции: последний кусок прошлой порции, не задетый этой
		uint32_t rd_a = 0, rd_n = 0;           // адрес (кратен 32) и пачек
		std::vector<uint8_t> rd_expect;
		for (auto it = wr_prev.rbegin (); it != wr_prev.rend () && !rd_n; ++it) {
			const uint32_t a0 = (it->a + 31) & ~31u, a1 = (it->a + it->n) & ~31u;
			if (a1 <= a0)
				continue;
			uint32_t nb = (a1 - a0) / 32;
			if (nb > 128)
				nb = 128;
			bool hit = false;
			for (const Span &s : wr_now)
				if (s.a < a0 + nb * 32 && a0 < s.a + s.n)
					hit = true;
			if (hit)
				continue;
			rd_a = a0;
			rd_n = nb;
			rd_expect.assign (bz_mem (model) + a0, bz_mem (model) + a0 + nb * 32);
		}
		wr_prev = wr_now;
		bool rd_started = false;

		bz_stats_reset (model);
		uint32_t err = bz_exec (model, words.data (), n);
		if (err) {
			printf ("кадр %d: модель отвергла слово %u — трасса испорчена\n", frame, err);
			return 2;
		}
		tot_px += bz_stats (model)->px_raster;

		uint32_t idx = 0;
		int quiet = 0;
		bool take = false;
		const uint64_t c0 = clk_cycles;
		uint64_t last = clk_cycles;          // когда ядро последний раз взяло слово команды
		for (;;) {
			// вход команд меняется только после фронта clk
			const bool rise = tick ();
			if (!rise)
				continue;
			if (take) {
				idx++;
				last = clk_cycles;
			}
			// копия в окно — когда ядро взяло половину порции (рисование идёт, мост делят); go — один такт
			top->rd_go = 0;
			if (rd_n && !rd_started && idx >= n / 2) {
				top->rd_addr = rd_a;
				top->rd_len = rd_n;
				top->rd_go = 1;
				rd_started = true;
			}
			top->cmd_valid = idx < n;
			top->cmd_data = idx < n ? words[idx] : 0;
			top->eval ();
			take = top->cmd_valid && top->cmd_ready;
			const unsigned d = top->dbg_pix;
			if (d & 0x04) st_d++;
			if (d & 0x08) st_e++;
			if (d & 0x10) st_f++;
			if (d & 0x01) px_out++;
			if (idx >= n && top->idle && !top->m_req && !top->c_req && ctl.st == Ctl::IDLE && !top->rd_busy &&
			    (rd_started || !rd_n)) {
				if (++quiet > 64)
					break;
			} else
				quiet = 0;
			// завис: ни одного слова команды за 20 млн тактов (самая длинная команда — заливка z, ~10^5)
			if (clk_cycles - last > (getenv ("TB_HANG") ? 2000000ull : 20000000ull)) {
				printf ("кадр %d: RTL завис (слово %u из %u)\n", frame, idx, n);
#ifdef TB_DBG
				auto *r = top->rootp;
				printf ("  ядро %d, memif %d, mem_req %d, m_req %d m_tag %d, c_req %d, модель %d\n",
				        (int)r->bz_gpu_sim__DOT__gpu__DOT__core__DOT__state,
				        (int)r->bz_gpu_sim__DOT__gpu__DOT__memif__DOT__st,
				        (int)r->bz_gpu_sim__DOT__gpu__DOT__core__DOT__mem_req, (int)top->m_req,
				        (int)r->bz_gpu_sim__DOT__gpu__DOT__m_tag, (int)top->c_req, (int)ctl.st);
				printf ("  мост: cq_w %d cq_r %d w_resv %d w_pend %d wf_w %d wf_r %d wf_av %d ri_w %d ri_r %d\n",
				        (int)r->bz_gpu_sim__DOT__bridge__DOT__cq_w, (int)r->bz_gpu_sim__DOT__bridge__DOT__cq_r,
				        (int)r->bz_gpu_sim__DOT__bridge__DOT__w_resv, (int)r->bz_gpu_sim__DOT__bridge__DOT__w_pend,
				        (int)r->bz_gpu_sim__DOT__bridge__DOT__wf_w, (int)r->bz_gpu_sim__DOT__bridge__DOT__wf_r,
				        (int)r->bz_gpu_sim__DOT__bridge__DOT__wf_av, (int)r->bz_gpu_sim__DOT__bridge__DOT__ri_w,
				        (int)r->bz_gpu_sim__DOT__bridge__DOT__ri_r);
#endif
				return 1;
			}
		}
		if (top->bad_cmd) {
			printf ("кадр %d: RTL увидел неверную команду\n", frame);
			return 1;
		}
		// MARK (SPEC 0.4): регистр ядра — как у модели
		if (top->mark != bz_mark (model)) {
			printf ("кадр %d: MARK RTL %u, модель %u\n", frame, (unsigned)top->mark, (unsigned)bz_mark (model));
			return 1;
		}
		// окно: слово за словом (окно отвечает к следующему фронту clk)
		if (rd_started) {
			for (uint32_t k = 0; k < rd_n * 8; k++) {
				top->win_raddr = k;
				while (!tick ())
					;
				const uint32_t got = top->win_rdata;
				const uint32_t want = (uint32_t)rd_expect[4 * k] | ((uint32_t)rd_expect[4 * k + 1] << 8) |
				                      ((uint32_t)rd_expect[4 * k + 2] << 16) | ((uint32_t)rd_expect[4 * k + 3] << 24);
				if (got != want) {
					printf ("кадр %d: окно чтения SDRAM, слово %u (адрес %06x): RTL %08x, модель %08x\n", frame, k,
					        rd_a + 4 * k, got, want);
					return 1;
				}
			}
			rd_checks++;
			rd_bytes += rd_n * 32;
		}

		if (frame >= cmp_from) {
			const uint8_t *mm = bz_mem (model);
			if (memcmp (mm, mem.data (), MEM)) {
				uint32_t first = 0, cnt = 0;
				for (uint32_t a = 0; a < MEM; a++)
					if (mm[a] != mem[a]) {
						if (!cnt)
							first = a;
						cnt++;
					}
				printf ("кадр %d: РАСХОЖДЕНИЕ %u байт, первый адрес %06x (модель %02x, RTL %02x)\n", frame, cnt,
				        first, mm[first], mem[first]);
				int shown = 0;
				for (uint32_t a = first; a < MEM && shown < 16; a++)
					if (mm[a] != mem[a]) {
						printf ("  %06x: модель %02x RTL %02x\n", a, mm[a], mem[a]);
						shown++;
					}
				printf ("  кадр в модели: %06x, шаг %d\n", bz_draw_fb (model), bz_stride (model));
				return 1;
			}
		}
		if (frame % 50 == 0)
			printf ("кадр %d: слов %u, тактов %llu (%.2f мс на 60,75 МГц), всего %llu — совпало\n", frame, n,
			        (unsigned long long)(clk_cycles - c0), (clk_cycles - c0) / 60750.0,
			        (unsigned long long)clk_cycles);
		fflush (stdout);
		frame++;
	}
	printf ("ПРОШЛО: %d кадров, память RTL = память модели; тактов %llu (%.2f мс на кадр), точек растра %llu\n",
	        frame, (unsigned long long)clk_cycles, frame ? clk_cycles / 60750.0 / frame : 0.0,
	        (unsigned long long)tot_px);
	printf ("такт на точку растра %.2f; стоп конвейера (тактов на точку): тексель %.3f, z %.3f, цвет %.3f\n",
	        tot_px ? (double)clk_cycles / tot_px : 0.0, tot_px ? (double)st_d / tot_px : 0.0,
	        tot_px ? (double)st_e / tot_px : 0.0, tot_px ? (double)st_f / tot_px : 0.0);
	printf ("контроллер: операций %llu, занят %.1f %% тактов clk_sd\n", (unsigned long long)ctl.ops,
	        ctl.t ? 100.0 * ctl.busy / ctl.t : 0.0);
	printf ("копий SDRAM в окно посреди рисования (SPEC 0.4): %llu, %llu байт — все совпали с моделью\n",
	        (unsigned long long)rd_checks, (unsigned long long)rd_bytes);
	top->final ();
	delete top;
	bz_free (model);
	return 0;
}
