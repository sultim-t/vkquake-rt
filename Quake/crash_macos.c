/*
 * macOS crash reporter for Quake RT: on a fatal signal, print the faulting
 * address and a symbolized stack (walked from the crashing thread's frame
 * pointers) to stderr, which the launcher writes to quakert.log.
 */
#if defined(__APPLE__) && defined(__aarch64__)

#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static void crash_print_addr (int i, uintptr_t pc)
{
	Dl_info info;
	memset (&info, 0, sizeof (info));
	if (dladdr ((void *)pc, &info) && info.dli_fname)
	{
		const char *lib = strrchr (info.dli_fname, '/');
		lib = lib ? lib + 1 : info.dli_fname;
		if (info.dli_sname)
			fprintf (stderr, "  #%-2d %-24s %s + %lu\n", i, lib, info.dli_sname, (unsigned long)(pc - (uintptr_t)info.dli_saddr));
		else
			fprintf (stderr, "  #%-2d %-24s 0x%lx (base+0x%lx)\n", i, lib, (unsigned long)pc, (unsigned long)(pc - (uintptr_t)info.dli_fbase));
	}
	else
		fprintf (stderr, "  #%-2d ?? 0x%lx\n", i, (unsigned long)pc);
}

static void crash_handler (int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	fprintf (stderr, "\n=== CRASH: signal %d (%s), fault address %p ===\n", sig, strsignal (sig), si ? si->si_addr : NULL);
	if (uc && uc->uc_mcontext)
	{
		uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
		uintptr_t lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
		uintptr_t fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
		crash_print_addr (0, pc);
		crash_print_addr (1, lr);
		for (int i = 2; i < 64 && fp && (fp & 7) == 0; i++)
		{
			uintptr_t *frame = (uintptr_t *)fp;
			uintptr_t next = frame[0], ret = frame[1];
			if (!ret)
				break;
			crash_print_addr (i, ret);
			if (next <= fp)
				break;
			fp = next;
		}
	}
	else
	{
		void *bt[64];
		int   n = backtrace (bt, 64);
		backtrace_symbols_fd (bt, n, STDERR_FILENO);
	}
	fflush (stderr);
	signal (sig, SIG_DFL);
	raise (sig);
}

__attribute__ ((constructor)) static void crash_install (void)
{
	struct sigaction sa;
	memset (&sa, 0, sizeof (sa));
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigemptyset (&sa.sa_mask);
	sigaction (SIGSEGV, &sa, NULL);
	sigaction (SIGBUS, &sa, NULL);
	sigaction (SIGILL, &sa, NULL);
	sigaction (SIGFPE, &sa, NULL);
	sigaction (SIGABRT, &sa, NULL);
	sigaction (SIGTRAP, &sa, NULL);
}

#endif
