#include "./symbolize.h"

#include <execinfo.h>

#include <cstdio>

__attribute__((flatten)) void Foo(int rec) {
  if (rec > 0) Foo(rec - 1);
  printf("foo %d\n", rec);
  if (rec == 0) {
    void* buf[20];
    int n = backtrace(buf, 20);

    char symbol[512];
    for (int i = 0; i < n; i++) {
      symbolize::Symbolize(reinterpret_cast<intptr_t>(buf[i]), symbol,
                           sizeof(symbol));
      printf("backtrace %d: %p %s\n", i, buf[i], symbol);
    }
  }
}

int main(int argc, char** argv) {
  Foo(5);
  return 0;
}
