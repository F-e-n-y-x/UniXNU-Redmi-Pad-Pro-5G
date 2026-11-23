// Runs each instruction that macOS arm64e code may use on every CPU; reports OK or SIGILL.
#define _GNU_SOURCE
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static sigjmp_buf jb;
static void onill(int s) { (void)s; siglongjmp(jb, 1); }
static unsigned long mem[4] = {1, 2, 3, 4};

#define T(name, word) static void name(void) { __asm__ volatile("mov x1, %0\n.inst " #word :: "r"(mem) : "x0", "x1", "x2", "v0", "v1", "v2", "memory", "cc"); }
T(t_paciasp_hint, 0xD503233F)   // PACIASP (hint space)
T(t_autiasp_hint, 0xD50323BF)   // AUTIASP (hint space)
T(t_pacia,   0xDAC10020)        // PACIA x0,x1
T(t_autia,   0xDAC11020)        // AUTIA x0,x1
T(t_xpaci,   0xDAC143E0)        // XPACI x0
T(t_ldraa,   0xF8200420)        // LDRAA x0,[x1]
T(t_fjcvtzs, 0x1E7E0000)        // FJCVTZS w0,d0   (v8.3 JSCVT)
T(t_fcmla,   0x6E82C420)        // FCMLA v0.4s,v1.4s,v2.4s,#0 (v8.3 FCMA)
T(t_ldapur,  0xD9400020)        // LDAPUR x0,[x1]  (v8.4 LRCPC2)
T(t_cfinv,   0xD500401F)        // CFINV (v8.4 FlagM)
T(t_axflag,  0xD500405F)        // AXFLAG (v8.5 FlagM2)
T(t_frint32z,0x1E284020)        // FRINT32Z s0,s1 (v8.5)
T(t_sb,      0xD50330FF)        // SB (v8.5)
T(t_ldapr,   0xF8BFC020)        // LDAPR x0,[x1]   (v8.3 LRCPC)
T(t_casal,   0xC8E0FC20)        // CASAL x0,x0,[x1] (v8.1 LSE)
static void t_pacga_real(void) { __asm__ volatile(".inst 0x9AC23020" ::: "x0"); } // PACGA x0,x1,x2

struct { const char *n; void (*f)(void); } tests[] = {
  {"PACIASP(hint)", t_paciasp_hint}, {"AUTIASP(hint)", t_autiasp_hint},
  {"PACIA", t_pacia}, {"AUTIA", t_autia}, {"XPACI", t_xpaci}, {"PACGA", t_pacga_real},
  {"LDRAA", t_ldraa}, {"FJCVTZS", t_fjcvtzs}, {"FCMLA", t_fcmla}, {"LDAPR", t_ldapr},
  {"LDAPUR", t_ldapur}, {"CFINV", t_cfinv}, {"AXFLAG", t_axflag}, {"FRINT32Z", t_frint32z},
  {"SB", t_sb}, {"CASAL", t_casal},
};

int main(void) {
  signal(SIGILL, onill);
  long n = sysconf(_SC_NPROCESSORS_CONF);
  for (int c = 0; c < n; c++) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(c, &s);
    if (sched_setaffinity(0, sizeof s, &s)) { printf("cpu%d: offline\n", c); continue; }
    printf("cpu%d:", c);
    for (unsigned i = 0; i < sizeof tests / sizeof *tests; i++) {
      if (sigsetjmp(jb, 1) == 0) { tests[i].f(); printf(" %s=OK", tests[i].n); }
      else printf(" %s=SIGILL", tests[i].n);
    }
    printf("\n");
  }
  return 0;
}
