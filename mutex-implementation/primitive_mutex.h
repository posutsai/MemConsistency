#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

/* Try to implement mutex and condition variable referring
 * https://locklessinc.com/articles/mutex_cv_futex/
 * https://gist.github.com/adambarta/3091107
 * Since the experiment is designed to be done in the userspace,
 * the code here should avoid depend on kernel lib. The atomic
 * operations here rely on gcc builtin functions.
 */

#define ATTEMPT_TIMES 100

// Refer to implementation in Linux kernel.
// https://c9x.me/x86/html/file_module_x86_id_232.html
// https://elixir.bootlin.com/linux/latest/source/arch/x86/include/asm/processor.h#L653
// Note:
// The "memory" argument here is called "Clobber register". Refer to Intel manual.
// https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html#Clobbers-and-Scratch-Registers
#define cpu_relax() 																		\
	__asm__ __volatile__ ( "pause\n" : : : "memory")

// Referring to "Futex is Tricky" paper, it defines three states
// of the mutexes. At the any moment of mutex lifecycle, mutex can
// only transfer among these 3 states.
// ==============================================================
// 0 -> unlocked
// 1 -> locked
// 2 -> locked and contended
// ==============================================================
//
typedef int mutex;
int sys_futex(void *addr1, int op, int val1, struct timespec *timeout, void *addr2, int val3);
int mutex_init(mutex *m, const pthread_mutexattr_t attr);
int mutex_destroy(mutex *m);

// Refer to Linux kernel implementation
// https://elixir.bootlin.com/linux/latest/source/arch/x86/include/asm/cmpxchg.h#L85
static inline mutex cmpxchg(mutex *ptr, mutex old_v, mutex new_v) {
	mutex ret;
// Use keyword "lock" to declare its atomicity.
	asm volatile("lock\n" "cmpxchgl %2,%1\n"
					: "=a" (ret), "+m" (*(mutex *)ptr)
					: "r" (new_v), "0" (old_v)
					: "memory");

	return ret;
}

static inline mutex xchg(mutex *ptr, mutex x) {
	asm volatile("lock\n" "xchgl %0,%1\n"
					:"=r" (x), "+m" (*(mutex*)ptr)
					:"0" (x)
					:"memory");
	return x;
}

// Since there is no wrapper of futex syscall, referring to man
// page, we write our own.
int sys_futex(void *addr1, int op, int val1, struct timespec *timeout, void *addr2, int val3) {
	return syscall(SYS_futex, addr1, op, val1, timeout, addr2, val3);
}

// Our implementation shares the same interface as pthread for
// compatibility. However, since the implementation is much more
// simplified compared to pthread version, we simply ignore the
// second argument.
int mutex_init(mutex *m, const pthread_mutexattr_t attr) {
	(void) attr; // small tip to suppress warning.
	*m = 0; // the state is initialized as unlocked.
	return 0;
}

int mutex_destroy(mutex *m) {
	// do nothing
	(void) m;
	return 0;
}

// Note:
// I use gcc builtin function "__sync_bool_compare_and_swap"
// here. However the "cmpxchg" function is able to be done by
// assembly, referring to adambarta's code.
int mutex_lock(mutex *m) {
	int i, output;
	for (i = 0; i < ATTEMPT_TIMES; i++) {
		output = cmpxchg(m, 0, 1);
		if (!output)
			return 0;
		cpu_relax();
	}
	if (output == 1)
		output = xchg(m, 2);
	while (output){
		syscall(SYS_futex, m, FUTEX_WAIT, 2, NULL, NULL, 0);
		output = xchg(m, 2);
	}
	return;
}

void unlock_mutex(mutex *m) {
	int i;
	if ((*m) == 2) {
		(*m) = 0;
	} else if (xchg(m, 0) == 1) {
		return;
	}

	for (i=0; i<200; i++){
		if ((*m)){
			if (cmpxchg(m, 1, 2)){
					return;
			}
		}
		cpu_relax();
	}

	syscall(SYS_futex, m, FUTEX_WAKE, 1, NULL, NULL, 0);

	return;
}
