/* See LICENSE for copyright and license details. */

void die(const char *fmt, ...);
void *ecalloc(size_t nmemb, size_t size);
int fd_set_nonblock(int fd);
/** environ with entries ("NAME=value", NULL terminated) in place of their
 * names', a new array (free() it) of environ's strings and entries; NULL
 * out of memory */
char **envwith(const char *const *entries);
/** execvp() with envp as the environment */
int execvpenv(const char *file, char *const argv[], char *const envp[]);
