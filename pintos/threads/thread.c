#include "threads/thread.h"

#include <debug.h>
#include <random.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;
static struct list sleep_list;

void thread_sleep_until(int64_t wake_tick);
/* wakeup_tick 비교를 위한 함수(오름차순) */
bool wakeup_tick_less(const struct list_elem *a, const struct list_elem *b,
                      void *aux);
bool thread_priority_greater(const struct list_elem *a,
                             const struct list_elem *b, void *aux);
void thread_wake_expired(int64_t now_tick);

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  /* Reload the temporal gdt for the kernel
   * This gdt does not include the user context.
   * The kernel will rebuild the gdt with user context, in gdt_init (). */
  struct desc_ptr gdt_ds = {.size = sizeof(gdt) - 1, .address = (uint64_t)gdt};
  lgdt(&gdt_ds);

  /* Init the globla thread context */
  lock_init(&tid_lock);
  list_init(&ready_list);
  list_init(&sleep_list);
  list_init(&destruction_req);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void) {
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void) {
  struct thread *t = thread_current();

  /* Update statistics. */
  if (t == idle_thread) idle_ticks++;
#ifdef USERPROG
  else if (t->pml4 != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE) intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
         idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority, thread_func *function,
                    void *aux) {
  struct thread *t;
  tid_t tid;

  ASSERT(function != NULL);

  /* Allocate thread. */
  t = palloc_get_page(PAL_ZERO);
  if (t == NULL) return TID_ERROR;

  /* Initialize thread. */
  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();

  /* Call the kernel_thread if it scheduled.
   * Note) rdi is 1st argument, and rsi is 2nd argument. */
  t->tf.rip = (uintptr_t)kernel_thread;
  t->tf.R.rdi = (uint64_t)function;
  t->tf.R.rsi = (uint64_t)aux;
  t->tf.ds = SEL_KDSEG;
  t->tf.es = SEL_KDSEG;
  t->tf.ss = SEL_KDSEG;
  t->tf.cs = SEL_KCSEG;
  t->tf.eflags = FLAG_IF;

  /* Add to run queue. */
  thread_unblock(t);

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);
  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread *t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));
  /* 외부 인터럽트들을 막고 현재 상태를 old_level에 저장 */
  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED); /* BLOCKED 상태인지 확인 */
  /* priority 기준(내림차순) 삽입 정렬 */
  list_insert_ordered(&ready_list, &(t->elem), thread_priority_greater, NULL);
  /* 먼저 스레드의 상태를 READY로 바꿔줌 */
  t->status = THREAD_READY;
  /* 현재 실행중인 스레드와 깨우려는 스레드(t)의
  우선순위 비교 t의 우선순위가 더 크다면 yield*/
  if (thread_current()->priority < t->priority) {
    if (intr_context())
      /* 현재 바꾸려는 스레드가 interrupt라면
      intr_yield_on_return 으로 플래그를 설정해주고
      핸들러가 반환되기 직전에 yield 하게 해줌*/
      intr_yield_on_return();
    else if (thread_current() != idle_thread) {
      /* 인터럽트가 아니라면 yield()를 해준다.
        idle인지 체크해주지 않아서 틀림 -> idle은 yield 해주지 않는다
        thread_yield()를 호출하기 전에 intr_set_level을 해주어야 한다.
      */
      intr_set_level(old_level);
      thread_yield();
      return;
    }
  }
  intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *thread_name(void) { return thread_current()->name; }

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *thread_current(void) {
  struct thread *t = running_thread();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void) { return thread_current()->tid; }

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void) {
  ASSERT(!intr_context());

#ifdef USERPROG
  process_exit();
#endif

  /* Just set our status to dying and schedule another process.
     We will be destroyed during the call to schedule_tail(). */
  intr_disable();
  do_schedule(THREAD_DYING);
  NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void) {
  struct thread *curr = thread_current(); /* 현재 실행 중인 스레드 */
  enum intr_level old_level;

  ASSERT(!intr_context()); /* 인터럽트 컨텍스트에서는 호출 금지 */

  old_level = intr_disable(); /* ready_list 조작 전 인터럽트 비활성 */
  if (curr != idle_thread) /* idle 스레드는 큐에 넣지 않음. 현재 스레드를
                              READY로 바꾸고 스케줄링 */
    list_insert_ordered(&ready_list, &curr->elem, thread_priority_greater,
                        NULL);

  do_schedule(THREAD_READY); /* 상태를 READY로 바꾸고 스케줄링 */
  intr_set_level(old_level); /* 인터럽트를 이전 상태로 복원 */
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority) {
  struct thread *cur = thread_current();
  int old_priority = cur->priority;
  /* 현재 실행중인 스레드의 우선순위를 new_priority 로 변경 해준다. */
  // 만약 현재 스레드의 우선순위가 기부받은 우선순위라면? original_priority 만
  // 바꿔준다.
  cur->origin_priority = new_priority;

  // 기부받은 우선순위가 없다면
  if (list_empty(&cur->donors_list)) {
    cur->priority = new_priority;
  } else {
    int best_pri = cur->origin_priority;
    if (!list_empty(&cur->donors_list)) {
      int donor_pri =
          list_entry(list_front(&cur->donors_list), struct thread, donor_elem)
              ->priority;
      if (best_pri < donor_pri) best_pri = donor_pri;
    }
    cur->priority = best_pri;
  }

  if (old_priority > cur->priority) { /* 저장해 놓은 변경 전 우선순위가 더
                               크다면(우선순위가 작아졌다면) yield */
    thread_yield();
  }
}

/* Returns the current thread's priority. */
int thread_get_priority(void) { return thread_current()->priority; }

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED) {
  /* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void) {
  /* TODO: Your implementation goes here */
  return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) {
  /* TODO: Your implementation goes here */
  return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
  /* TODO: Your implementation goes here */
  return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void idle(void *idle_started_ UNUSED) {
  struct semaphore *idle_started = idle_started_;

  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;) {
    /* Let someone else run. */
    intr_disable();
    thread_block();

    /* Re-enable interrupts and wait for the next one.

       The `sti' instruction disables interrupts until the
       completion of the next instruction, so these two
       instructions are executed atomically.  This atomicity is
       important; otherwise, an interrupt could be handled
       between re-enabling interrupts and waiting for the next
       one to occur, wasting as much as one clock tick worth of
       time.

       See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
       7.11.1 "HLT Instruction". */

    asm volatile("sti; hlt" : : : "memory");
    /*
    sti (Set Interrupt Flag): 인터럽트 수신을 다시 허용합니다.
    hlt (Halt): CPU의 동작을 멈춥니다.
    전력 소모를 줄이고 쉬는 상태에 들어가는 것이죠.
    CPU는 다음 인터럽트 신호가 들어올 때까지 이 상태로 계속 대기합니다.
    */
  }
}

/* Function used as the basis for a kernel thread. */
static void kernel_thread(thread_func *function, void *aux) {
  ASSERT(function != NULL);

  intr_enable(); /* The scheduler runs with interrupts off. */
  function(aux); /* Execute the thread function. */
  thread_exit(); /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void init_thread(struct thread *t, const char *name, int priority) {
  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy(t->name, name, sizeof t->name);
  t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
  t->priority = priority;
  t->origin_priority = priority;
  t->magic = THREAD_MAGIC;
  list_init(&t->donors_list);
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *next_thread_to_run(void) {
  if (list_empty(&ready_list))
    return idle_thread;
  else
    return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf) {
  __asm __volatile(
      "movq %0, %%rsp\n"
      "movq 0(%%rsp),%%r15\n"
      "movq 8(%%rsp),%%r14\n"
      "movq 16(%%rsp),%%r13\n"
      "movq 24(%%rsp),%%r12\n"
      "movq 32(%%rsp),%%r11\n"
      "movq 40(%%rsp),%%r10\n"
      "movq 48(%%rsp),%%r9\n"
      "movq 56(%%rsp),%%r8\n"
      "movq 64(%%rsp),%%rsi\n"
      "movq 72(%%rsp),%%rdi\n"
      "movq 80(%%rsp),%%rbp\n"
      "movq 88(%%rsp),%%rdx\n"
      "movq 96(%%rsp),%%rcx\n"
      "movq 104(%%rsp),%%rbx\n"
      "movq 112(%%rsp),%%rax\n"
      "addq $120,%%rsp\n"
      "movw 8(%%rsp),%%ds\n"
      "movw (%%rsp),%%es\n"
      "addq $32, %%rsp\n"
      "iretq"
      :
      : "g"((uint64_t)tf)
      : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void thread_launch(struct thread *th) {
  uint64_t tf_cur = (uint64_t)&running_thread()->tf;
  uint64_t tf = (uint64_t)&th->tf;
  ASSERT(intr_get_level() == INTR_OFF);

  /* The main switching logic.
   * We first restore the whole execution context into the intr_frame
   * and then switching to the next thread by calling do_iret.
   * Note that, we SHOULD NOT use any stack from here
   * until switching is done. */
  __asm __volatile(
      /* Store registers that will be used. */
      "push %%rax\n"
      "push %%rbx\n"
      "push %%rcx\n"
      /* Fetch input once */
      "movq %0, %%rax\n"
      "movq %1, %%rcx\n"
      "movq %%r15, 0(%%rax)\n"
      "movq %%r14, 8(%%rax)\n"
      "movq %%r13, 16(%%rax)\n"
      "movq %%r12, 24(%%rax)\n"
      "movq %%r11, 32(%%rax)\n"
      "movq %%r10, 40(%%rax)\n"
      "movq %%r9, 48(%%rax)\n"
      "movq %%r8, 56(%%rax)\n"
      "movq %%rsi, 64(%%rax)\n"
      "movq %%rdi, 72(%%rax)\n"
      "movq %%rbp, 80(%%rax)\n"
      "movq %%rdx, 88(%%rax)\n"
      "pop %%rbx\n"  // Saved rcx
      "movq %%rbx, 96(%%rax)\n"
      "pop %%rbx\n"  // Saved rbx
      "movq %%rbx, 104(%%rax)\n"
      "pop %%rbx\n"  // Saved rax
      "movq %%rbx, 112(%%rax)\n"
      "addq $120, %%rax\n"
      "movw %%es, (%%rax)\n"
      "movw %%ds, 8(%%rax)\n"
      "addq $32, %%rax\n"
      "call __next\n"  // read the current rip.
      "__next:\n"
      "pop %%rbx\n"
      "addq $(out_iret -  __next), %%rbx\n"
      "movq %%rbx, 0(%%rax)\n"  // rip
      "movw %%cs, 8(%%rax)\n"   // cs
      "pushfq\n"
      "popq %%rbx\n"
      "mov %%rbx, 16(%%rax)\n"  // eflags
      "mov %%rsp, 24(%%rax)\n"  // rsp
      "movw %%ss, 32(%%rax)\n"
      "mov %%rcx, %%rdi\n"
      "call do_iret\n"
      "out_iret:\n"
      :
      : "g"(tf_cur), "g"(tf)
      : "memory");
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void do_schedule(int status) {
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(thread_current()->status == THREAD_RUNNING);
  while (!list_empty(&destruction_req)) {
    struct thread *victim =
        list_entry(list_pop_front(&destruction_req), struct thread, elem);
    palloc_free_page(victim);
  }
  thread_current()->status = status;
  schedule();
}

static void schedule(void) {
  struct thread *curr = running_thread();
  struct thread *next = next_thread_to_run();

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(curr->status != THREAD_RUNNING);
  ASSERT(is_thread(next));
  /* Mark us as running. */
  next->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate(next);
#endif

  if (curr != next) {
    /* If the thread we switched from is dying, destroy its struct
       thread. This must happen late so that thread_exit() doesn't
       pull out the rug under itself.
       We just queuing the page free reqeust here because the page is
       currently used by the stack.
       The real destruction logic will be called at the beginning of the
       schedule(). */
    if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
      ASSERT(curr != next);
      list_push_back(&destruction_req, &curr->elem);
    }

    /* Before switching the thread, we first save the information
     * of current running. */
    thread_launch(next);
  }
}

/* Returns a tid to use for a new thread. */
static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

bool wakeup_tick_less(const struct list_elem *a, const struct list_elem *b,
                      void *aux) {
  const struct thread *A = list_entry(a, struct thread, elem);
  const struct thread *B = list_entry(b, struct thread, elem);
  return A->wakeup_tick < B->wakeup_tick;
}

bool thread_priority_greater(const struct list_elem *a,
                             const struct list_elem *b, void *aux) {
  const struct thread *A = list_entry(a, struct thread, elem);
  const struct thread *B = list_entry(b, struct thread, elem);
  return A->priority > B->priority;
}

/* 주어진 tick 시각까지 현재 스레드를 재우는 함수 */
void thread_sleep_until(int64_t wake_tick) {
  // 인터럽트 비활성화: 크리티컬 섹션 보호
  enum intr_level old_level = intr_disable();

  struct thread *cur_thr = thread_current();

  // idle thread는 절대 sleep 대상이 될 수 없음
  ASSERT(cur_thr != idle_thread);

  cur_thr->wakeup_tick = wake_tick;
  // wakeup_tick 기준으로 오름차순 정렬된 sleep_list에 삽입
  list_insert_ordered(&sleep_list, &(cur_thr->elem), wakeup_tick_less, NULL);
  thread_block();

  // 이전 인터럽트 상태로 복원
  intr_set_level(old_level);
}

/* now_tick 이상인 잠자는 스레드를 깨우는 함수(오름차순 sleep_list 가정) */
void thread_wake_expired(int64_t now_tick) {
  enum intr_level old_level = intr_disable();

  while (!list_empty(&sleep_list)) {
    struct thread *t = list_entry(list_front(&sleep_list), struct thread, elem);
    if (now_tick < t->wakeup_tick) {
      break;
    }
    list_pop_front(&sleep_list); /* 만기 스레드 제거 */
    thread_unblock(t);           /* READY로 전환 */
  }
  intr_set_level(old_level);
}