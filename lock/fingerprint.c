// Fingerprints through fprintd (net.reactivated.Fprint) on the system bus,
// with sd-bus: the bus's fd is one of awlock's loop's, so nothing polls.
//
// GetDefaultDevice, then Claim the reader for this user, then
// VerifyStart("any"). Then come VerifyStatus signals: verify-match unlocks,
// verify-no-match shows the logo as wrong, and a finished attempt starts
// over (VerifyStop, VerifyStart). The retry-* results are only logged.
//
// If someone else holds the reader (AlreadyInUse), it is claimed again
// after a while. A reader that disconnects (unplugged, suspend) is released
// and looked up again after a while: it may come back under a new path. If
// there is no reader or no fprintd, fingerprints are off until the next lock.
// fprintd releases the reader when awlock's connection closes, so nothing
// needs releasing on the way out.
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include "awlock.h"

#define FPRINT "net.reactivated.Fprint"
#define FPRINT_MANAGER FPRINT ".Manager"
#define FPRINT_DEVICE FPRINT ".Device"
#define FPRINT_MANAGER_PATH "/net/reactivated/Fprint/Manager"
#define FPRINT_IN_USE FPRINT ".Error.AlreadyInUse"
// claiming again, verifying again after an error
#define RETRY_MS 2000

struct fingerprint {
	struct awlock_state *state;
	sd_bus *bus;
	int fd; // the bus's, in the event loop; -1 once removed
	char *device; // the reader's object path, once known
	sd_bus_slot *status; // the match for its VerifyStatus
	// VerifyStart has been answered: VerifyStatus signals are ours, not
	// those of another client's verification
	bool verifying;
	struct loop_timer *retry;
};

static void verify_start(struct fingerprint *fp);
static void find_reader(void *data);

// a wrong finger shows as wrong, as a wrong password does, unless a
// password is being checked
static void show_wrong(struct awlock_state *state) {
	if (state->auth_state == AUTH_STATE_VALIDATING) {
		return;
	}
	state->auth_state = AUTH_STATE_INVALID;
	damage_state(state);
	schedule_auth_idle(state);
}

static void retry_later(struct fingerprint *fp, void (*callback)(void *data)) {
	loop_remove_timer(fp->retry);
	fp->retry = loop_add_timer(RETRY_MS, callback, fp);
}

// a method of the reader, answered by reply
static void call_device(struct fingerprint *fp, const char *method,
		sd_bus_message_handler_t reply, const char *types, const char *arg) {
	int r = sd_bus_call_method_async(fp->bus, NULL, FPRINT, fp->device,
		FPRINT_DEVICE, method, reply, fp, types, arg);
	if (r < 0) {
		awl_err_printf("awlock: Fingerprint: %s: %s\n", method, strerror(-r));
	}
}

static const char *error_name(sd_bus_message *m) {
	const sd_bus_error *e = sd_bus_message_get_error(m);
	return e ? e->name : NULL;
}

static void log_error(const char *what, sd_bus_message *m) {
	const sd_bus_error *e = sd_bus_message_get_error(m);
	awl_err_printf("awlock: Fingerprint: %s: %s\n", what,
		e && e->message ? e->message : "failed");
}

static int verify_stopped(sd_bus_message *m, void *data, sd_bus_error *err) {
	// an error too: it wasn't verifying any more, start again anyway
	verify_start(data);
	return 0;
}

static void restart(struct fingerprint *fp) {
	fp->verifying = false;
	call_device(fp, "VerifyStop", verify_stopped, "", NULL);
}

// restart() and claim() from a retry_later() timer
static void restart_timer(void *data) {
	struct fingerprint *fp = data;
	fp->retry = NULL;
	restart(fp);
}

static int verify_started(sd_bus_message *m, void *data, sd_bus_error *err) {
	struct fingerprint *fp = data;
	if (sd_bus_message_is_method_error(m, NULL)) {
		log_error("VerifyStart", m);
		retry_later(fp, restart_timer);
		return 0;
	}
	awl_vrb_printf("awlock: Fingerprint: verifying\n");
	fp->verifying = true;
	return 0;
}

static void verify_start(struct fingerprint *fp) {
	fp->verifying = false;
	call_device(fp, "VerifyStart", verify_started, "s", "any");
}

static void claim_timer(void *data);

static int claimed(sd_bus_message *m, void *data, sd_bus_error *err) {
	struct fingerprint *fp = data;
	if (sd_bus_message_is_method_error(m, NULL)) {
		const char *name = error_name(m);
		if (name && strcmp(name, FPRINT_IN_USE) == 0) {
			// e.g. a login prompt has it: later
			awl_vrb_printf("awlock: Fingerprint: the reader is in use\n");
			retry_later(fp, claim_timer);
		} else {
			log_error("Claim", m);
		}
		return 0;
	}
	verify_start(fp);
	return 0;
}

static void claim(struct fingerprint *fp) {
	call_device(fp, "Claim", claimed, "s", ""); // "": this user
}

static void claim_timer(void *data) {
	struct fingerprint *fp = data;
	fp->retry = NULL;
	claim(fp);
}

static int verify_status(sd_bus_message *m, void *data, sd_bus_error *err) {
	struct fingerprint *fp = data;
	const char *result;
	int done;
	if (!fp->verifying || sd_bus_message_read(m, "sb", &result, &done) < 0) {
		return 0;
	}
	awl_log_printf("awlock: Fingerprint: %s%s\n", result, done ? "" : " (go on)");
	if (strcmp(result, "verify-match") == 0) {
		fp->state->running = false; // unlocks, as a right password does
	} else if (strcmp(result, "verify-disconnected") == 0) {
		// the reader went: let it go, and find it (or another) later
		fp->verifying = false;
		call_device(fp, "Release", NULL, "", NULL);
		fp->status = sd_bus_slot_unref(fp->status);
		free(fp->device);
		fp->device = NULL;
		retry_later(fp, find_reader);
	} else if (done) {
		// verify-no-match and the errors end an attempt: the next one
		if (strcmp(result, "verify-no-match") == 0) {
			show_wrong(fp->state);
		}
		restart(fp);
	}
	return 0;
}

static int got_device(sd_bus_message *m, void *data, sd_bus_error *err) {
	struct fingerprint *fp = data;
	const char *path;
	if (sd_bus_message_is_method_error(m, NULL)) {
		// no fprintd, or no reader: no fingerprints, said once
		const char *name = error_name(m);
		if (name && strcmp(name, FPRINT ".Error.NoSuchDevice") == 0) {
			awl_err_printf("awlock: Fingerprint: no reader, passwords only\n");
		} else if (name && (strcmp(name, SD_BUS_ERROR_SERVICE_UNKNOWN) == 0 ||
				strcmp(name, SD_BUS_ERROR_NAME_HAS_NO_OWNER) == 0)) {
			awl_err_printf("awlock: Fingerprint: no fprintd, passwords only\n");
		} else {
			log_error("GetDefaultDevice", m);
		}
		return 0;
	}
	if (sd_bus_message_read(m, "o", &path) < 0 || !(fp->device = strdup(path))) {
		return 0;
	}
	awl_vrb_printf("awlock: Fingerprint: reader %s\n", fp->device);
	int r = sd_bus_match_signal_async(fp->bus, &fp->status, FPRINT, fp->device,
		FPRINT_DEVICE, "VerifyStatus", verify_status, NULL, fp);
	if (r < 0) {
		awl_err_printf("awlock: Fingerprint: VerifyStatus: %s\n", strerror(-r));
		return 0;
	}
	claim(fp);
	return 0;
}

// GetDefaultDevice: at the start, and from a timer after a disconnect
static void find_reader(void *data) {
	struct fingerprint *fp = data;
	fp->retry = NULL;
	int r = sd_bus_call_method_async(fp->bus, NULL, FPRINT, FPRINT_MANAGER_PATH,
		FPRINT_MANAGER, "GetDefaultDevice", got_device, fp, "");
	if (r < 0) {
		awl_err_printf("awlock: Fingerprint: GetDefaultDevice: %s\n", strerror(-r));
	}
}

static void bus_in(void *data) {
	struct fingerprint *fp = data;
	int r;
	while ((r = sd_bus_process(fp->bus, NULL)) > 0) {
	}
	if (r < 0) {
		// the bus is gone: fingerprints are off
		awl_err_printf("awlock: Fingerprint: system bus: %s\n", strerror(-r));
		loop_remove_fd(fp->fd);
		fp->fd = -1;
	}
}

struct fingerprint *fingerprint_start(struct awlock_state *state) {
	struct fingerprint *fp = calloc(1, sizeof(*fp));
	if (!fp) {
		return NULL;
	}
	fp->state = state;
	fp->fd = -1;
	int r = sd_bus_open_system(&fp->bus);
	if (r < 0) {
		awl_err_printf("awlock: Fingerprint: no system bus (%s), passwords only\n",
			strerror(-r));
		free(fp);
		return NULL;
	}
	// sd-bus answers on its fd; a message it couldn't write at once goes
	// out with the next one it processes (they are a few bytes)
	fp->fd = sd_bus_get_fd(fp->bus);
	loop_add_fd(fp->fd, bus_in, fp);
	find_reader(fp);
	return fp;
}

void fingerprint_stop(struct fingerprint *fp) {
	if (!fp) {
		return;
	}
	loop_remove_timer(fp->retry);
	loop_remove_fd(fp->fd);
	sd_bus_slot_unref(fp->status);
	// fprintd releases the reader with the connection
	sd_bus_flush_close_unref(fp->bus);
	free(fp->device);
	free(fp);
}
