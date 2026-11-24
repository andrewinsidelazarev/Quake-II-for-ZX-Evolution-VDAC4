/*
 * bz_crash.c — только для стенда на ПК: при падении ref_bz печатает место (функция, файл, строка)
 * и цепочку вызовов в stderr и в <q2m_out>.crash.txt. Отладчика на машине нет, а без этого
 * падение в 30 000 строк id и своего кода искать вслепую.
 */
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

static char crash_name[512];

static void bz_where (FILE *f, HANDLE proc, DWORD64 addr)
{
	char             buf[sizeof(SYMBOL_INFO) + 256];
	SYMBOL_INFO     *sym = (SYMBOL_INFO *)buf;
	IMAGEHLP_LINE64  line;
	DWORD64          disp = 0;
	DWORD            ldisp = 0;

	memset (buf, 0, sizeof(buf));
	sym->SizeOfStruct = sizeof(SYMBOL_INFO);
	sym->MaxNameLen = 255;
	memset (&line, 0, sizeof(line));
	line.SizeOfStruct = sizeof(line);
	if (SymFromAddr (proc, addr, &disp, sym))
	{
		if (SymGetLineFromAddr64 (proc, addr, &ldisp, &line))
			fprintf (f, "  %08llx %s+%llu  %s:%lu\n", addr, sym->Name, disp, line.FileName, line.LineNumber);
		else
			fprintf (f, "  %08llx %s+%llu\n", addr, sym->Name, disp);
	}
	else
		fprintf (f, "  %08llx ?\n", addr);
}

static LONG WINAPI bz_filter (EXCEPTION_POINTERS *ep)
{
	HANDLE       proc = GetCurrentProcess (), th = GetCurrentThread ();
	CONTEXT      ctx = *ep->ContextRecord;
	STACKFRAME64 sf;
	FILE        *f = crash_name[0] ? fopen (crash_name, "w") : NULL;
	FILE        *outs[2] = { stderr, f };
	int          i, k;

	SymSetOptions (SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
	SymInitialize (proc, NULL, TRUE);
	for (k = 0; k < 2; k++)
		if (outs[k])
			fprintf (outs[k], "bz: исключение %08lx по адресу %p\n", ep->ExceptionRecord->ExceptionCode,
			         ep->ExceptionRecord->ExceptionAddress);
	memset (&sf, 0, sizeof(sf));
	sf.AddrPC.Offset = ctx.Eip;
	sf.AddrPC.Mode = AddrModeFlat;
	sf.AddrFrame.Offset = ctx.Ebp;
	sf.AddrFrame.Mode = AddrModeFlat;
	sf.AddrStack.Offset = ctx.Esp;
	sf.AddrStack.Mode = AddrModeFlat;
	for (i = 0; i < 24; i++)
	{
		if (!StackWalk64 (IMAGE_FILE_MACHINE_I386, proc, th, &sf, &ctx, NULL, SymFunctionTableAccess64,
		                  SymGetModuleBase64, NULL) || !sf.AddrPC.Offset)
			break;
		for (k = 0; k < 2; k++)
			if (outs[k])
				bz_where (outs[k], proc, sf.AddrPC.Offset);
	}
	if (f)
		fclose (f);
	return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI bz_vectored (EXCEPTION_POINTERS *ep)
{
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
	    code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
		return EXCEPTION_CONTINUE_SEARCH;
	fprintf (stderr, "bz: crash %08lx at %p, ref_bz.dll base %p\n", code,
	         ep->ExceptionRecord->ExceptionAddress, (void *)GetModuleHandleA ("ref_bz.dll"));
	fflush (stderr);
	bz_filter (ep);
	fflush (stderr);
	ExitProcess (3);
	return EXCEPTION_CONTINUE_SEARCH;
}

void BZ_InstallCrashHandler (const char *out_prefix)
{
	if (out_prefix && out_prefix[0])
		_snprintf (crash_name, sizeof(crash_name) - 1, "%s.crash.txt", out_prefix);
	/* векторный обработчик зовётся первым, до фильтров CRT и exe */
	AddVectoredExceptionHandler (1, bz_vectored);
}
