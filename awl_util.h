/* See LICENSE for copyright and license details. */

void die(const char *fmt, ...);
void *ecalloc(size_t nmemb, size_t size);
int fd_set_nonblock(int fd);
/** environ with entries ("NAME=value", NULL terminated) in place of their
 * names', a new array (free() it) of environ's strings and entries; NULL
 * out of memory */
char **envwith(const char *const *entries);
/** whether a and b are the same string, or both NULL */
int streqnull(const char *a, const char *b);
/** strdup(), and NULL for NULL */
char *strdupnull(const char *s);
