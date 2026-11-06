// symlinks, junctions and windows pretending
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

int main(void) {
  const char* dir = "/tmp/linktest";
  char path[256], buf[256];
  mkdir(dir, 0700);
  snprintf(path, sizeof path, "%s/target", dir);
  mkdir(path, 0700);
  snprintf(path, sizeof path, "%s/target/file.txt", dir);
  FILE* f = fopen(path, "w");
  fputs("hello\n", f);
  fclose(f);

  unlink("/tmp/linktest/link");
  unlink("/tmp/linktest/link.next");
  check(symlink("/tmp/linktest/target", "/tmp/linktest/link") == 0, "symlink to a directory");
  ssize_t n = readlink("/tmp/linktest/link", buf, sizeof buf - 1);
  buf[n > 0 ? n : 0] = 0;
  check(n > 0 && strcmp(buf, "/tmp/linktest/target") == 0, "readlink gives the target");
  printf("     readlink: %s\n", buf);
  struct stat st;
  check(lstat("/tmp/linktest/link", &st) == 0 && S_ISLNK(st.st_mode), "lstat sees a link");
  check(stat("/tmp/linktest/link", &st) == 0 && S_ISDIR(st.st_mode), "stat follows it");
  f = fopen("/tmp/linktest/link/file.txt", "r");
  check(f && fgets(buf, sizeof buf, f) && strcmp(buf, "hello\n") == 0, "a file through the link");
  if (f) fclose(f);

  check(symlink("/tmp/linktest/target", "/tmp/linktest/link.next") == 0, "second link");
  check(rename("/tmp/linktest/link.next", "/tmp/linktest/link") == 0, "rename over the first link");
  check(lstat("/tmp/linktest/link", &st) == 0 && S_ISLNK(st.st_mode), "still a link after rename");

  int saw_link = 0;
  DIR* d = opendir("/tmp/linktest");
  for (struct dirent* e; d && (e = readdir(d));) saw_link |= strcmp(e->d_name, "link") == 0 && e->d_type == DT_LNK;
  if (d) closedir(d);
  check(saw_link, "readdir reports DT_LNK");

  check(unlink("/tmp/linktest/link") == 0, "unlink the link");
  check(access("/tmp/linktest/target/file.txt", F_OK) == 0, "the target survives");
  check(readlink("/tmp/linktest/target", buf, sizeof buf) < 0 && errno == EINVAL, "readlink of a directory is EINVAL");

  unlink("/tmp/linktest/flink");
  unlink("/tmp/linktest/flink.next");
  unlink("/tmp/linktest/rlink");
  check(symlink("/tmp/linktest/target/file.txt", "/tmp/linktest/flink") == 0, "symlink to a file");
  n = readlink("/tmp/linktest/flink", buf, sizeof buf - 1);
  buf[n > 0 ? n : 0] = 0;
  check(n > 0 && strcmp(buf, "/tmp/linktest/target/file.txt") == 0, "readlink of the file link");
  check(lstat("/tmp/linktest/flink", &st) == 0 && S_ISLNK(st.st_mode), "lstat sees the file link");
  check(stat("/tmp/linktest/flink", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 6, "stat follows the file link");
  f = fopen("/tmp/linktest/flink", "r");
  check(f && fgets(buf, sizeof buf, f) && strcmp(buf, "hello\n") == 0, "read through the file link");
  if (f) fclose(f);
  check(symlink("target/file.txt", "/tmp/linktest/rlink") == 0, "relative file link");
  f = fopen("/tmp/linktest/rlink", "r");
  check(f && fgets(buf, sizeof buf, f) && strcmp(buf, "hello\n") == 0, "read through the relative link");
  if (f) fclose(f);
  check(symlink("/tmp/linktest/target/file.txt", "/tmp/linktest/flink.next") == 0 &&
            rename("/tmp/linktest/flink.next", "/tmp/linktest/flink") == 0,
        "rename a file link over another");
  check(lstat("/tmp/linktest/flink", &st) == 0 && S_ISLNK(st.st_mode), "still a file link after rename");
  check(unlink("/tmp/linktest/flink") == 0 && unlink("/tmp/linktest/rlink") == 0, "unlink the file links");
  check(access("/tmp/linktest/target/file.txt", F_OK) == 0, "the file survives");
  printf("%d failures\n", failures);
  return failures != 0;
}
