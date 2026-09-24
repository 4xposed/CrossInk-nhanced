#include <cstdlib>
#include <limits>

// Deliberately invalid test executable: the harness requires a sanitizer report.
int main(int argc, char**) {
  if (argc == 1) {
    volatile int* value = static_cast<int*>(std::malloc(sizeof(int)));
    value[1] = 42;
    std::free(const_cast<int*>(value));
  } else {
    volatile int value = std::numeric_limits<int>::max();
    return value + 1;
  }
  return 0;
}
