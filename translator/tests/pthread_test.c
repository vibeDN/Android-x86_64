/*
 * Copyright (C) 2026 The Android-x86_64 Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* pthread: create/join, mutex, condvar, TLS (__thread + pthread_key), atomics. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define N 4
#define ITERS 20000
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter; static atomic_long acounter; static int ready;
static __thread int tls_val; static pthread_key_t key;

static void* worker(void* arg) {
  long id = (long)arg;
  tls_val = (int)id * 100; pthread_setspecific(key, (void*)(id + 1));
  pthread_mutex_lock(&mu);
  while (!ready) pthread_cond_wait(&cv, &mu);
  pthread_mutex_unlock(&mu);
  for (int i = 0; i < ITERS; i++) {
    pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu);
    atomic_fetch_add(&acounter, 1);
  }
  if (tls_val != id * 100 || (long)pthread_getspecific(key) != id + 1) return (void*)1;
  return (void*)0;
}

int main(void) {
  pthread_t t[N]; long bad = 0;
  pthread_key_create(&key, NULL);
  for (long i = 0; i < N; i++) pthread_create(&t[i], NULL, worker, (void*)i);
  pthread_mutex_lock(&mu); ready = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
  for (int i = 0; i < N; i++) { void* r; pthread_join(t[i], &r); bad += (long)r; }
  printf("counter=%ld atomic=%ld tls_bad=%ld main_tls=%d\n", counter, (long)acounter, bad, tls_val);
  return (counter == N * ITERS && acounter == N * ITERS && !bad) ? 0 : 1;
}
