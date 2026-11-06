#include <unistd.h>
int main(void) {
  write(1, "tiny: write works\n", 18);
  return 7;
}
