#include "signalfd_ctx.h"

#include <sys/types.h>

#include <sys/event.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#ifdef __OpenBSD__
#define sigisemptyset(sigs) (*(sigs) == 0)
#define sigandset(sd, sl, sr) ((*(sd) = *(sl) & *(sr)), 0)
#elif defined(__NetBSD__)
#define sigisemptyset(sigs)                                                   \
	({                                                                    \
		sigset_t e;                                                   \
		__sigemptyset(&e);                                            \
		__sigsetequal(sigs, &e);                                      \
	})
#define sigandset(sd, sl, sr)                                                 \
	({                                                                    \
		memcpy((sd), (sl), sizeof(sigset_t));                         \
		__sigandset((sr), (sd));                                      \
		0;                                                            \
	})
#endif

static errno_t
signalfd_has_pending(SignalFDCtx const *signalfd, bool *has_pending,
    sigset_t *pending)
{
	sigset_t pending_sigs;

	if (sigpending(&pending_sigs) < 0 ||
	    sigandset(&pending_sigs, &pending_sigs, &signalfd->sigs) < 0) {
		return errno;
	}

	*has_pending = !sigisemptyset(&pending_sigs);
	if (pending) {
		*pending = pending_sigs;
	}
	return 0;
}

static errno_t
signalfd_ctx_trigger_manually(SignalFDCtx *signalfd)
{
	return kqueue_event_trigger(&signalfd->kqueue_event, signalfd->kq);
}

errno_t
signalfd_ctx_init(SignalFDCtx *signalfd, int kq, const sigset_t *sigs)
{
	errno_t ec;

	assert(sigs != NULL);

	*signalfd = (SignalFDCtx){.kq = kq, .sigs = *sigs};

	if ((ec = pthread_mutex_init(&signalfd->mutex, NULL)) != 0) {
		return ec;
	}

#ifndef _SIG_MAXSIG
#define _SIG_MAXSIG (8 * sizeof(sigset_t))
#endif

	struct kevent kevs[_SIG_MAXSIG + 2];
	int n = 0;

	if ((ec = kqueue_event_init(&signalfd->kqueue_event, kevs, &n,
		 false)) != 0) {
		goto out2;
	}

	for (int i = 1; i <= _SIG_MAXSIG; ++i) {
		if (sigismember(&signalfd->sigs, i)) {
			EV_SET(&kevs[n++], i, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
		}
	}

	n = kevent(signalfd->kq, kevs, n, NULL, 0, NULL);
	if (n < 0) {
		ec = errno;
		goto out;
	}

	bool has_pending;
	if ((ec = signalfd_has_pending(signalfd, &has_pending, NULL)) != 0) {
		goto out;
	}
	if (has_pending) {
		if ((ec = signalfd_ctx_trigger_manually(signalfd)) != 0) {
			goto out;
		}
	}

	return 0;

out:
	(void)kqueue_event_terminate(&signalfd->kqueue_event);
out2:
	pthread_mutex_destroy(&signalfd->mutex);
	return ec;
}

errno_t
signalfd_ctx_terminate(SignalFDCtx *signalfd)
{
	errno_t ec = 0;
	errno_t ec_local;

	ec_local = kqueue_event_terminate(&signalfd->kqueue_event);
	ec = ec != 0 ? ec : ec_local;
	ec_local = pthread_mutex_destroy(&signalfd->mutex);
	ec = ec != 0 ? ec : ec_local;

	return ec;
}

static errno_t
signalfd_ctx_read_impl(SignalFDCtx *signalfd, uint32_t *ident)
{
	/*
	 * EVFILT_SIGNAL is an "observer". It does not hook into the
	 * signal disposition mechanism. On the other hand, `signalfd` does.
	 * Therefore, to properly emulate `signalfd`, `sigtimedwait` must be
	 * called.
	 */

	int s;
#ifdef __OpenBSD__
	for (;;) {
		errno_t ec;
		bool has_pending;
		sigset_t pending_sigs;
		if ((ec = signalfd_has_pending(signalfd, &has_pending,
			 &pending_sigs)) != 0) {
			return ec;
		}
		if (!has_pending) {
			return EAGAIN;
		}

		/*
		 * sigwait does not behave nicely when multiple signals
		 * are pending (as of OpenBSD 6.8). So, only try to
		 * grab one.
		 */
		int signum = __builtin_ffsll((long long)pending_sigs);
		sigset_t mask = sigmask(signum);

		extern int __thrsigdivert(sigset_t set, siginfo_t * info,
		    struct timespec const *timeout);

		/* `&(struct timespec){0, 0}` returns EAGAIN but spams
		 * the dmesg log. Let's do it with an invalid timespec
		 * and EINVAL. */
		s = __thrsigdivert(mask, NULL, &(struct timespec){0, -1});
		if (s < 0 && (errno == EINVAL || errno == EAGAIN)) {
			/* We must retry because we only checked for
			 * one signal. There may be others pending. */
			continue;
		}
		break;
	}
#else
	s = sigtimedwait(&signalfd->sigs, NULL, &(struct timespec){0, 0});
#endif
	if (s < 0) {
		return errno;
	}

	*ident = (uint32_t)s;
	return 0;
}

static bool
signalfd_ctx_clear_signal(SignalFDCtx *signalfd, bool was_triggered)
{
	if (was_triggered) {
		/*
		 * When there are other signals pending we can keep the kq
		 * readable and therefore don't need to clear it.
		 */
		bool has_pending;
		if (signalfd_has_pending(signalfd, &has_pending, NULL) != 0 ||
		    has_pending) {
			return true;
		}
	}

	/*
	 * Clear the kq. Signals can arrive here, leading to a race.
	 */

	kqueue_event_clear(&signalfd->kqueue_event, signalfd->kq);

	/*
	 * Because of the race, we must recheck and manually trigger if
	 * necessary.
	 */
	bool has_pending;
	if (signalfd_has_pending(signalfd, &has_pending, NULL) != 0 ||
	    has_pending) {
		(void)signalfd_ctx_trigger_manually(signalfd);
		return true;
	}
	return false;
}

errno_t
signalfd_ctx_read(SignalFDCtx *signalfd, uint32_t *ident)
{
	errno_t ec;

	(void)pthread_mutex_lock(&signalfd->mutex);
	ec = signalfd_ctx_read_impl(signalfd, ident);
	if (ec == 0 || ec == EAGAIN || ec == EWOULDBLOCK) {
		(void)signalfd_ctx_clear_signal(signalfd, false);
	}
	(void)pthread_mutex_unlock(&signalfd->mutex);

	return ec;
}

void
signalfd_ctx_poll(SignalFDCtx *signalfd, uint32_t *revents)
{
	(void)pthread_mutex_lock(&signalfd->mutex);

	bool pending = signalfd_ctx_clear_signal(signalfd, revents != NULL);
	if (revents) {
		*revents = pending ? POLLIN : 0;
	}

	(void)pthread_mutex_unlock(&signalfd->mutex);
}
