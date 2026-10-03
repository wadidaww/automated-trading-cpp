#include "futu_trader/infra/thread_tuning.hpp"

#include <pthread.h>
#include <sched.h>

namespace futu_trader::infra {

bool pinCurrentThreadToCpu(int cpu) {
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

bool setCurrentThreadRealtime(int priority) {
  if (priority < 1 || priority > 99) {
    return false;
  }
  sched_param param{};
  param.sched_priority = priority;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
}

}  // namespace futu_trader::infra
