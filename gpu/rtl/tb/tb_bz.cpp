// =====================================================================================================
// tb_bz.cpp — стенд: Verilog растеризатора (bz_core) против C-модели (bomzh.c) на потоке Quake 2.
// =====================================================================================================
//
//   tb_bz.exe <трасса.bzcmd> [кадров] [задержка_памяти] [с_какого_кадра_сравнивать]
//
// ЧТО ТАКОЕ СТЕНД И VERILATOR
//   Стенд (testbench) — программа, которая подаёт схеме входы и проверяет выходы. Verilator переводит
//   Verilog в C++: получается класс Vbz_core, у которого входы и выходы — поля, а один шаг схемы — вызов
//   eval(). Мы сами «качаем» такт: clk = 0, eval(), clk = 1, eval() — это один такт ПЛИС.
//
// ЧТО ПРОВЕРЯЕТСЯ
//   Трассу пишет ref_bz (+set bz_trace 1): на кадр — число слов, цвет отладочной заливки (−1 — нет) и сами
//   слова команд. Каждый кадр выполняется дважды: в C-модели (эталон, bz_exec) и в RTL через порт
//   команд, с идеальной памятью 8 МБ (ответ через «задержку» тактов). После кадра вся память RTL
//   сравнивается с памятью модели — кадры, z, загруженные текстуры: совпасть должно всё до байта.
//   Итог — «ПРОШЛО» или первые расхождения с адресами; код возврата 0 только при полном совпадении.
//
// ПОРТЫ, КОТОРЫЕ ИГРАЕТ СТЕНД
//   команды: cmd_valid/cmd_data/cmd_ready — слово уходит, когда ядро готово (cmd_ready = 1);
//   память: на mem_req стенд через latency тактов отвечает mem_ack (для чтения — со словом);
//   swap_ack = 1 всегда — вывода нет, смена кадра подтверждается сразу.
#include "Vbz_core.h"
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

double sc_time_stamp () { return 0; }      // нужен библиотеке Verilator без SystemC

int main (int argc, char **argv)
{
	if (argc < 2) {
		fprintf (stderr, "tb_bz <трасса.bzcmd> [кадров] [задержка] [сравнивать_с]\n");
		return 2;
	}
	FILE *f = fopen (argv[1], "rb");
	if (!f) {
		fprintf (stderr, "нет файла %s\n", argv[1]);
		return 2;
	}
	const int max_frames = argc > 2 ? atoi (argv[2]) : 1 << 30;
	const int latency = argc > 3 ? atoi (argv[3]) : 0;
	const int cmp_from = argc > 4 ? atoi (argv[4]) : 0;

	Verilated::commandArgs (argc, argv);
	Vbz_core *top = new Vbz_core;
	bz_t *model = bz_create (MEM);
	std::vector<uint8_t> mem (MEM, 0);
	std::vector<uint32_t> words;

	top->swap_ack = 1;          // вывода нет: смена кадра подтверждается сразу
	top->rst = 1;
	top->cmd_valid = 0;
	top->mem_ack = 0;
	for (int i = 0; i < 4; i++) {
		top->clk = 0; top->eval ();
		top->clk = 1; top->eval ();
	}
	top->rst = 0;

	uint64_t cycles = 0, accesses = 0;
	int frame = 0, fails = 0;
	bool acked = false;
	int wait = 0;

	while (frame < max_frames) {
		uint32_t n;
		int32_t clr;
		if (fread (&n, 4, 1, f) != 1 || fread (&clr, 4, 1, f) != 1)
			break;
		words.resize (n);
		if (fread (words.data (), 4, n, f) != n)
			break;

		// отладочная заливка рисуемого кадра — и в модели, и в памяти RTL (ref_bz делает так же)
		if (clr >= 0) {
			uint32_t fb = bz_draw_fb (model), npx = (uint32_t)(bz_stride (model) * bz_height (model));
			bz_debug_clear (model, (uint8_t)clr);
			for (uint32_t k = 0; k < npx; k++)
				mem[(fb + k) & (MEM - 1)] = (uint8_t)clr;
		}
		uint32_t err = bz_exec (model, words.data (), n);
		if (err) {
			printf ("кадр %d: модель отвергла слово %u — трасса испорчена\n", frame, err);
			return 2;
		}

		// RTL: подать слова и дождаться покоя
		uint32_t idx = 0;
		int quiet = 0;
		uint64_t c0 = cycles;
		for (;;) {
			top->clk = 0;
			top->cmd_valid = idx < n;
			top->cmd_data = idx < n ? words[idx] : 0;
			// память: ответ на запрос через latency тактов, не два раза подряд
			top->mem_ack = 0;
			if (top->mem_req && !acked) {
				if (wait < latency)
					wait++;
				else {
					uint32_t a = top->mem_addr & ~3u & (MEM - 1);
					if (top->mem_we) {
						for (int b = 0; b < 4; b++)
							if (top->mem_be & (1 << b))
								mem[a + b] = (uint8_t)(top->mem_wdata >> (8 * b));
					}
					top->mem_rdata = mem[a] | (mem[a + 1] << 8) | (mem[a + 2] << 16) | ((uint32_t)mem[a + 3] << 24);
					top->mem_ack = 1;
					wait = 0;
					accesses++;
				}
			}
			top->eval ();
			const bool take = top->cmd_ready && top->cmd_valid;
			acked = top->mem_ack;
			top->clk = 1;
			top->eval ();
			cycles++;
			if (take)
				idx++;
			if (idx >= n && top->idle && !top->mem_req) {
				if (++quiet > 4)
					break;
			} else
				quiet = 0;
			if (cycles - c0 > 4000000000ull) {
				printf ("кадр %d: RTL завис (слово %u из %u)\n", frame, idx, n);
				return 1;
			}
		}
		if (top->bad_cmd) {
			printf ("кадр %d: RTL увидел неизвестную команду\n", frame);
			return 1;
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
				for (uint32_t a = first; a < MEM && shown < 12; a++)
					if (mm[a] != mem[a]) {
						printf ("  %06x: модель %02x RTL %02x\n", a, mm[a], mem[a]);
						shown++;
					}
				if (++fails >= 1)
					return 1;
				// после расхождения памяти разные — продолжать бессмысленно
			}
		}
		if (frame % 50 == 0)
			printf ("кадр %d: слов %u, тактов %llu, всего тактов %llu, обращений %llu — совпало\n", frame, n,
			        (unsigned long long)(cycles - c0), (unsigned long long)cycles, (unsigned long long)accesses);
		fflush (stdout);
		frame++;
	}
	printf ("ПРОШЛО: %d кадров, память RTL = память модели; тактов %llu, обращений к памяти %llu\n", frame,
	        (unsigned long long)cycles, (unsigned long long)accesses);
	top->final ();
	delete top;
	bz_free (model);
	return 0;
}
