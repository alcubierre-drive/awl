// Passwords: typed into an mlock()ed buffer (auth_handle_key()), and checked
// with PAM in a child process forked at the start (auth_start()). The two
// talk over two pipes (password_fd, reply_fd); the reply's fd is in awlock's
// loop.
//
// PAM uses an existing service, LOCK_PAM_SERVICE (meson option
// lock_pam_service, system-auth by default), so awlock needs no file in
// /etc/pam.d. It needs no root: PAM's unix_chkpwd checks the user's own
// password.
#define _POSIX_C_SOURCE 200809L
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "awlock.h"

// Password buffers: page aligned and mlock()ed, so they stay out of swap,
// and cleared before they are freed. A failed mlock() is logged, and awlock
// locks anyway: not locking would be worse.

// memset(), but not optimised away for a buffer that is freed next: the
// writes are volatile (explicit_bzero() and memset_explicit() need
// _DEFAULT_SOURCE or C23)
static void clear_buffer(void *buf, size_t size) {
	volatile unsigned char *p = buf;
	while (size--) {
		*p++ = 0;
	}
}

static char *password_buffer_create(size_t size) {
	void *buffer;
	int result = posix_memalign(&buffer, sysconf(_SC_PAGESIZE), size);
	if (result) {
		errno = result; // posix_memalign() doesn't set it
		awl_err_printf("awlock: failed to alloc password buffer: %s\n", strerror(errno));
		return NULL;
	}
	if (mlock(buffer, size) != 0) {
		awl_err_printf("awlock: mlock() failed, a password may be swapped: %s\n", strerror(errno));
	}
	return buffer;
}

static void password_buffer_destroy(char *buffer, size_t size) {
	clear_buffer(buffer, size);
	munlock(buffer, size);
	free(buffer);
}

static void clear_password_buffer(struct awlock_password *pw) {
	clear_buffer(pw->buffer, PASSWORD_SIZE);
	pw->len = 0;
}

// The pipes. A password goes to the child as its size, then its bytes (the
// size counts the closing NUL); the reply is a bool, true when it was right.
// The ends this process keeps: awlock writes to password_fd and reads
// reply_fd, the child the other way round.

static int password_fd = -1, reply_fd = -1;

static bool write_all(int fd, const void *buf, size_t size) {
	for (size_t done = 0; done < size; ) {
		ssize_t n = write(fd, (const char *)buf + done, size - done);
		if (n < 0) {
			return false;
		}
		done += n;
	}
	return true;
}

// false at the end of the pipe too (errno then 0)
static bool read_all(int fd, void *buf, size_t size) {
	for (size_t done = 0; done < size; ) {
		errno = 0;
		ssize_t n = read(fd, (char *)buf + done, size - done);
		if (n <= 0) {
			return false;
		}
		done += n;
	}
	return true;
}

// awlock: the password to the child; it is cleared, whatever happens
static bool send_password(struct awlock_password *pw) {
	size_t size = pw->len + 1;
	bool sent = write_all(password_fd, &size, sizeof(size)) &&
		write_all(password_fd, pw->buffer, size);
	if (!sent) {
		awl_err_printf("awlock: Failed to send the password: %s\n", strerror(errno));
	}
	clear_password_buffer(pw);
	return sent;
}

// awlock: whether the child found the password right
static bool receive_reply(void) {
	bool right;
	if (!read_all(reply_fd, &right, sizeof(right))) {
		awl_err_printf("awlock: Failed to read the password's check: %s\n", strerror(errno));
		return false;
	}
	return right;
}

// The child: PAM

// the child: the next password, in a password buffer of *size; NULL when
// awlock has gone (it exits on an error)
static char *receive_password(size_t *size) {
	if (!read_all(password_fd, size, sizeof(*size))) {
		if (errno == 0) {
			return NULL;
		}
		awl_err_printf("awlock: Failed to read a password: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (*size == 0 || *size > PASSWORD_SIZE) {
		awl_err_printf("awlock: A password of %zu bytes\n", *size);
		exit(EXIT_FAILURE);
	}
	char *password = password_buffer_create(*size);
	if (!password) {
		exit(EXIT_FAILURE);
	}
	if (!read_all(password_fd, password, *size) || password[*size - 1] != '\0') {
		awl_err_printf("awlock: Failed to read a password: %s\n", strerror(errno));
		password_buffer_destroy(password, *size);
		exit(EXIT_FAILURE);
	}
	return password;
}

// PAM's questions: the password (data: the char * being checked) for each
// prompt; PAM clears and frees the copies
static int converse(int num_msg, const struct pam_message **msg,
		struct pam_response **resp, void *data) {
	const char *password = *(char **)data;
	struct pam_response *replies = calloc(num_msg, sizeof(*replies));
	if (!replies) {
		return PAM_BUF_ERR;
	}
	for (int i = 0; i < num_msg; i++) {
		if (msg[i]->msg_style != PAM_PROMPT_ECHO_OFF &&
				msg[i]->msg_style != PAM_PROMPT_ECHO_ON) {
			continue; // PAM_ERROR_MSG, PAM_TEXT_INFO: nothing to answer
		}
		if (!(replies[i].resp = strdup(password))) {
			// PAM takes no replies on an error: the copies so far go
			for (int j = 0; j < i; j++) {
				if (replies[j].resp) {
					clear_buffer(replies[j].resp, strlen(replies[j].resp));
					free(replies[j].resp);
				}
			}
			free(replies);
			return PAM_BUF_ERR;
		}
	}
	*resp = replies;
	return PAM_SUCCESS;
}

// checks passwords until awlock closes its end, then exits
static _Noreturn void child_run(void) {
	struct passwd *passwd = getpwuid(getuid());
	if (!passwd) {
		awl_err_printf("awlock: getpwuid failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	char *password = NULL;
	const struct pam_conv conv = {converse, &password};
	pam_handle_t *pam = NULL;
	if (pam_start(LOCK_PAM_SERVICE, passwd->pw_name, &conv, &pam) != PAM_SUCCESS) {
		awl_err_printf("awlock: pam_start failed\n");
		exit(EXIT_FAILURE);
	}
	awl_vrb_printf("awlock: Checking passwords of %s\n", passwd->pw_name);

	int status = PAM_SUCCESS;
	size_t size;
	while ((password = receive_password(&size))) {
		status = pam_authenticate(pam, 0);
		password_buffer_destroy(password, size);
		password = NULL;
		if (status != PAM_SUCCESS) {
			awl_err_printf("awlock: pam_authenticate failed: %s\n",
				pam_strerror(pam, status));
		}
		bool right = status == PAM_SUCCESS;
		if (!write_all(reply_fd, &right, sizeof(right))) {
			awl_err_printf("awlock: Failed to reply: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	// awlock has gone (no one waits for this child)
	pam_setcred(pam, PAM_REFRESH_CRED);
	pam_end(pam, status);
	exit(EXIT_SUCCESS);
}

// the pipes, and the child; each keeps its two ends. The child closes
// wayland_fd, the locker's connection to awl.
static bool child_start(int wayland_fd) {
	int passwords[2], replies[2]; // [0] read, [1] written
	if (pipe(passwords) != 0 || pipe(replies) != 0) {
		awl_err_printf("awlock: failed to create pipe: %s\n", strerror(errno));
		return false;
	}
	pid_t child = fork();
	if (child < 0) {
		awl_err_printf("awlock: failed to fork: %s\n", strerror(errno));
		return false;
	}
	bool in_child = child == 0;
	password_fd = passwords[in_child ? 0 : 1];
	reply_fd = replies[in_child ? 1 : 0];
	close(passwords[in_child ? 1 : 0]);
	close(replies[in_child ? 0 : 1]);
	if (in_child) {
		// awl sees the locker go when its last copy closes
		close(wayland_fd);
		child_run();
	}
	return true;
}

// Typing

// the last UTF-8 character off
static bool backspace(struct awlock_password *pw) {
	if (pw->len == 0) {
		return false;
	}
	size_t end = pw->len;
	do {
		pw->len--;
	} while (pw->len > 0 && (pw->buffer[pw->len] & 0xc0) == 0x80);
	clear_buffer(pw->buffer + pw->len, end - pw->len);
	return true;
}

// a full buffer drops what is typed
static void append(struct awlock_password *pw, const char *text) {
	size_t n = strlen(text);
	if (pw->len + n < PASSWORD_SIZE) {
		memcpy(pw->buffer + pw->len, text, n + 1);
		pw->len += n;
	}
}

static void set_auth_idle(void *data) {
	struct awlock_state *state = data;
	state->auth_idle_timer = NULL;
	state->auth_state = AUTH_STATE_IDLE;
	damage_state(state);
}

void schedule_auth_idle(struct awlock_state *state) {
	loop_remove_timer(state->auth_idle_timer);
	state->auth_idle_timer = loop_add_timer(3000, set_auth_idle, state);
}

static void cancel_password_clear(struct awlock_state *state) {
	loop_remove_timer(state->clear_password_timer);
	state->clear_password_timer = NULL;
}

// the password forgotten: typed (Escape, Ctrl-U), or left (after 10 s)
static void clear_password(struct awlock_state *state) {
	cancel_password_clear(state);
	clear_password_buffer(&state->password);
	damage_state(state);
}

static void clear_password_timer(void *data) {
	struct awlock_state *state = data;
	state->clear_password_timer = NULL;
	clear_password(state);
}

static void schedule_password_clear(struct awlock_state *state) {
	loop_remove_timer(state->clear_password_timer);
	state->clear_password_timer = loop_add_timer(10000, clear_password_timer, state);
}

static void submit_password(struct awlock_state *state) {
	// nothing to check: not a wrong password for PAM (pam_faillock)
	if (state->password.len == 0) {
		return;
	}

	state->auth_state = AUTH_STATE_VALIDATING;
	cancel_password_clear(state);

	if (!send_password(&state->password)) {
		state->auth_state = AUTH_STATE_INVALID;
		schedule_auth_idle(state);
	}

	damage_state(state);
}

void auth_handle_key(struct awlock_state *state,
		xkb_keysym_t keysym, const char *text) {
	switch (keysym) {
	case XKB_KEY_KP_Enter:
	case XKB_KEY_Return:
		submit_password(state);
		break;
	case XKB_KEY_Delete:
	case XKB_KEY_BackSpace:
		if (state->xkb.control) {
			clear_password(state);
		} else if (backspace(&state->password)) {
			schedule_password_clear(state);
			damage_state(state);
		} else {
			cancel_password_clear(state);
		}
		break;
	case XKB_KEY_Escape:
		clear_password(state);
		break;
	case XKB_KEY_Caps_Lock:
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
		// nothing to see (Caps Lock redraws through keyboard_modifiers())
		schedule_password_clear(state);
		break;
	case XKB_KEY_m:
	case XKB_KEY_d:
	case XKB_KEY_j:
		if (state->xkb.control) {
			submit_password(state);
			break;
		}
		// fallthrough
	case XKB_KEY_c:
	case XKB_KEY_u:
		if (state->xkb.control) {
			clear_password(state);
			break;
		}
		// fallthrough
	default:
		if (*text) {
			append(&state->password, text);
			schedule_password_clear(state);
			damage_state(state);
		}
		break;
	}
}

// the child's reply: unlocks, or shows the password as wrong
static void reply_in(void *data) {
	struct awlock_state *state = data;
	if (receive_reply()) {
		state->running = false;
	} else {
		state->auth_state = AUTH_STATE_INVALID;
		schedule_auth_idle(state);
		damage_state(state);
	}
}

void auth_start(struct awlock_state *state) {
	// first: the child gets no password buffer
	if (!child_start(state->wayland_fd)) {
		exit(EXIT_FAILURE);
	}
	state->password.len = 0;
	state->password.buffer = password_buffer_create(PASSWORD_SIZE);
	if (!state->password.buffer) {
		exit(EXIT_FAILURE);
	}
	loop_add_fd(reply_fd, reply_in, state);
}
