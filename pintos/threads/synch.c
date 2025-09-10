/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"

#include <stdio.h>
#include <string.h>

#include "threads/interrupt.h"
#include "threads/thread.h"
void donate_remove_lock(struct lock *lock);
bool sema_priority_greater(const struct list_elem *a, const struct list_elem *b,
                           void *aux) {
  const struct thread *A = list_entry(a, struct thread, elem);
  const struct thread *B = list_entry(b, struct thread, elem);
  return A->priority >= B->priority;
}
/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void sema_init(struct semaphore *sema, unsigned value) {
  ASSERT(sema != NULL);

  sema->value = value;
  list_init(&sema->waiters);
}

/* 세마포어 자원을 요청하고 획득하는 함수 (P 연산) */
void sema_down(struct semaphore *sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);
  ASSERT(!intr_context());

  old_level = intr_disable();
  // 자리가 날 때까지 반복해서 확인하며 잠든다.
  while (sema->value == 0) {
    // 현재 스레드를 대기 리스트에 추가
    list_insert_ordered(&sema->waiters, &thread_current()->elem,
                        thread_priority_greater, NULL);

    // 스레드를 BLOCKED 하고 스케줄러에 CPU를 양보
    thread_block();
  }
  // 획득 했으면 카운트 - 1
  sema->value--;
  intr_set_level(old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool sema_try_down(struct semaphore *sema) {
  enum intr_level old_level;
  bool success;

  ASSERT(sema != NULL);

  old_level = intr_disable();
  if (sema->value > 0) {
    sema->value--;
    success = true;
  } else
    success = false;
  intr_set_level(old_level);

  return success;
}

/* 사용이 끝난 세마포어 자원을 반납하는 함수 (V 연산) */
void sema_up(struct semaphore *sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);

  old_level = intr_disable();
  // 만약 대기 중인 스레드가 있다면,
  // 카운트 +1
  sema->value++;
  list_sort(&sema->waiters, sema_priority_greater, NULL);
  if (!list_empty(&sema->waiters))
    // 그 중 하나를 깨워서 ready_list로 보낸다
    thread_unblock(
        list_entry(list_pop_front(&sema->waiters), struct thread, elem));

  intr_set_level(old_level);
}

static void sema_test_helper(void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void sema_self_test(void) {
  struct semaphore sema[2];
  int i;

  printf("Testing semaphores...");
  sema_init(&sema[0], 0);
  sema_init(&sema[1], 0);
  thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) {
    sema_up(&sema[0]);
    sema_down(&sema[1]);
  }
  printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void sema_test_helper(void *sema_) {
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) {
    sema_down(&sema[0]);
    sema_up(&sema[1]);
  }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init(struct lock *lock) {
  ASSERT(lock != NULL);

  lock->holder = NULL;
  sema_init(&lock->semaphore, 1);
}

/* 락을 획득하는 함수.
   내부적으로 값이 1인 세마포어에 대해 sema_down을 호출합니다.
  만약 다른 스레드가 이미 락을 가지고 있다면, sema_down에 의해 잠들게 됩니다.
  깨어나서 락을 획득하는 데 성공하면, 자신이 이 락의 주인(holder)임을
  기록합니다. */
void lock_acquire(struct lock *lock) {
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  // 이미 락을 가진 스레드가 또 요청하면 에러
  ASSERT(!lock_held_by_current_thread(lock));

  enum intr_level old_level = intr_disable();
  struct thread *cur = thread_current();

  // 1. 먼저 lock을 가진 애가 있는지 확인 -> donation ㄱㄱ
  if (lock->holder != NULL) {
    // 스레드에 기다리는 락 저장
    cur->waiting_for_lock = lock;
    // 현재 스레드를 기부자 목록에 넣어줌
    struct list *d_list = &(lock->holder->donors_list);
    list_insert_ordered(d_list, &cur->donor_elem, thread_priority_greater,
                        NULL);
    // 기부를 연쇄적으로 해주기 위해서 lock->holder 저장
    struct thread *done = lock->holder;
    while (done != NULL) {
      if (cur->priority > done->priority) {
        done->priority = cur->priority;
      }
      // 기부해 준 스레드가 기다리고 있는 lock이 있으면
      if (done->waiting_for_lock != NULL) {
        done = done->waiting_for_lock->holder;
      } else {
        break;
      }
    }
  }
  intr_set_level(old_level);
  // 내부 세마포어를 이용해 락을 요청합니다.
  sema_down(&lock->semaphore);

  old_level = intr_disable();
  // 락의 소유자를 현재 스레드로 설정합니다.
  lock->holder = cur;
  // 락을 획득 했으므로 기다리는 lock 을 비워준다.
  cur->waiting_for_lock = NULL;
  intr_set_level(old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool lock_try_acquire(struct lock *lock) {
  bool success;

  ASSERT(lock != NULL);
  ASSERT(!lock_held_by_current_thread(lock));

  success = sema_try_down(&lock->semaphore);
  if (success) lock->holder = thread_current();
  return success;
}

/* lock을 기다리는 기부자들 현재 스레드의 donors_list에서 제거*/
void donate_remove_lock(struct lock *lock) {
  struct thread *cur = thread_current();
  if (!list_empty(&lock->holder->donors_list)) {
    struct list_elem *e = NULL;
    for (e = list_begin(&lock->holder->donors_list);
         e != list_end(&lock->holder->donors_list);) {
      struct list_elem *next = list_next(e);
      struct thread *d = list_entry(e, struct thread, donor_elem);
      // lock == waiting_for_lock 일 때 빼준다.
      if (lock == d->waiting_for_lock) {
        list_remove(e);
      }
      e = next;
    }
  }
}

/* 우선순위 재조정*/
void donate_recalculate_priority(struct lock *lock) {
  struct thread *cur = thread_current();
  int best_pri = cur->origin_priority;
  if (!list_empty(&lock->holder->donors_list)) {
    int donor_pri = list_entry(list_front(&lock->holder->donors_list),
                               struct thread, donor_elem)
                        ->priority;
    if (best_pri < donor_pri) best_pri = donor_pri;
  }
  cur->priority = best_pri;
}

/* 락을 반납하는 함수.
    @param lock 반납하려는 락. 현재 스레드가 반드시 락의 소유자여야 함.
    먼저 자신이 주인(holder)이 아니라고 기록을 지우고,내부 세마포어에
        sema_up을 호출하여 기다리던 다음 스레드가 들어올 수 있게 합니다.
*/
void lock_release(struct lock *lock) {
  ASSERT(lock != NULL);
  // 이미 락을 가진 스레드가 또 요청하면 에러
  ASSERT(lock_held_by_current_thread(lock));
  enum intr_level old_level = intr_disable();
  donate_remove_lock(lock);
  donate_recalculate_priority(lock);
  // 락의 소유자 정보를 지웁니다.
  lock->holder = NULL;
  // 내부 세마포어에 자원을 반납하여 대기 중인 스레드를 깨웁니다.
  sema_up(&lock->semaphore);
  intr_set_level(old_level);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool lock_held_by_current_thread(const struct lock *lock) {
  ASSERT(lock != NULL);

  return lock->holder == thread_current();
}

/* One semaphore in a list. */
struct semaphore_elem {
  struct list_elem elem;      /* List element. */
  struct semaphore semaphore; /* This semaphore. */
  struct thread *waiter_thread;
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void cond_init(struct condition *cond) {
  ASSERT(cond != NULL);

  list_init(&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
// 특정 조건이 만족될 때까지 기다리는 함수.
void cond_wait(struct condition *cond, struct lock *lock) {
  struct semaphore_elem waiter;

  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));
  // 대기를 위한 개인용 세마포어를 준비
  sema_init(&waiter.semaphore, 0);
  // 스레드 넣어주기
  waiter.waiter_thread = thread_current();
  // 이 세마포어를 대기실(waiters) 리스트에 추가
  list_push_back(&cond->waiters, &waiter.elem);
  // 다른 스레드가 조건을 만족시킬 수 있도록 락을 잠시 풀어줌
  lock_release(lock);
  // 신호가 올 때까지 개인용 세마포어를 기다리며 잠듬
  sema_down(&waiter.semaphore);
  // 깨어난 후, 다시 락을 획득하고 계속 진행.
  lock_acquire(lock);
}

/* 조건이 만족되었음을 기다리는 스레드 중 하나에게 알리는(signal) 함수
        @param cond 신호를 보낼 조건 변수.
        @param lock 보호 락.*/
void cond_signal(struct condition *cond, struct lock *lock UNUSED) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  if (!list_empty(&cond->waiters)) {
    struct list_elem *e;
    struct semaphore_elem *max_waiter =
        list_entry(list_begin(&cond->waiters), struct semaphore_elem, elem);
    for (e = list_begin(&cond->waiters); e != list_end(&cond->waiters);
         e = list_next(e)) {
      struct semaphore_elem *cur_waiter =
          list_entry(e, struct semaphore_elem, elem);
      if (cur_waiter->waiter_thread->priority >
          max_waiter->waiter_thread->priority) {
        max_waiter = cur_waiter;
      }
    }
    list_remove(&max_waiter->elem);
    sema_up(&(max_waiter->semaphore));
  }
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_broadcast(struct condition *cond, struct lock *lock) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);

  while (!list_empty(&cond->waiters)) cond_signal(cond, lock);
}
