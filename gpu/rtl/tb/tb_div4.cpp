// =====================================================================================================
// tb_div4.cpp — проверка делителя bz_div4 против деления в C: случайные пары и крайние случаи.
// =====================================================================================================
//   tb_div4.exe [пар]
// Числа берутся разной длины (делимое 1…48 бит, делитель 1…32 бит), чтобы попадать во все ветки:
// частное 0, частное из одной цифры, нечётное число цифр, полное 48-битное частное. Плюс случаи,
// которые бывают в растеризаторе: 2^46 / zi (zi от 2^15) и края рёбер. Итог — «ПРОШЛО» или первая ошибка.
#include "Vbz_div4.h"
#include "verilated.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

double sc_time_stamp () { return 0; }

static Vbz_div4 *top;
static uint64_t  cycles;

static void tick ()
{
	top->clk = 0; top->eval ();
	top->clk = 1; top->eval ();
	cycles++;
}

static bool check (uint64_t n, uint32_t d)
{
	top->num = n;
	top->den = d;
	top->start = 1;
	tick ();
	top->start = 0;
	int guard = 0;
	while (!top->done) {
		tick ();
		if (++guard > 100) {
			printf ("завис: %llu / %u\n", (unsigned long long)n, d);
			return false;
		}
	}
	const uint64_t q = n / d, r = n % d;
	if (top->quot != q || top->rem != r) {
		printf ("ОШИБКА: %llu / %u: надо %llu ост %llu, RTL %llu ост %u\n", (unsigned long long)n, d,
		        (unsigned long long)q, (unsigned long long)r, (unsigned long long)top->quot, top->rem);
		return false;
	}
	return true;
}

int main (int argc, char **argv)
{
	const long pairs = argc > 1 ? atol (argv[1]) : 1000000;
	Verilated::commandArgs (argc, argv);
	top = new Vbz_div4;
	top->rst = 1;
	tick ();
	tick ();
	top->rst = 0;

	// крайние случаи
	const uint64_t M48 = (1ull << 48) - 1;
	const uint64_t edge_n[] = { 0, 1, 2, 3, 4, 7, 8, 1ull << 46, (1ull << 46) - 1, M48, M48 - 1, 1ull << 47 };
	const uint32_t edge_d[] = { 1, 2, 3, 4, 5, 7, 16, 32768, 32767, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu };
	for (uint64_t n : edge_n)
		for (uint32_t d : edge_d)
			if (!check (n, d))
				return 1;

	std::mt19937_64 rng (12345);
	uint64_t c0 = cycles;
	for (long i = 0; i < pairs; i++) {
		const int nb = 1 + (int)(rng () % 48), db = 1 + (int)(rng () % 32);
		uint64_t n = rng () & ((nb == 64) ? ~0ull : ((1ull << nb) - 1));
		uint32_t d = (uint32_t)(rng () & ((1ull << db) - 1));
		if (!d)
			d = 1;
		if (i % 4 == 0) {                    // перспектива: 2^46 / zi, zi ≥ 2^15
			n = 1ull << 46;
			d = 32768u + (uint32_t)(rng () % 0x7FFF8000u);
		}
		if (!check (n, d))
			return 1;
	}
	printf ("ПРОШЛО: %ld пар и крайние случаи, в среднем %.1f такта на деление\n", pairs,
	        (double)(cycles - c0) / pairs);
	delete top;
	return 0;
}
