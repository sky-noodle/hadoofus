// This will fail horribly if asserts are disabled:
#ifdef NDEBUG
# undef NDEBUG
#endif

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hadoofus/lowlevel.h>

#include "net.h"
#include "objects.h"
#include "pthread_wrappers.h"

static struct hdfs_namenode *	_namenode_copyref_unlocked(struct hdfs_namenode *);
static void			_namenode_decref(struct hdfs_namenode *);
static bool			_namenode_recv(struct hdfs_rpc_response_future *);

static void	_namenode_pending_insert_unlocked(struct hdfs_namenode *n,
		int64_t msgno, struct hdfs_rpc_response_future *future);

static struct hdfs_rpc_response_future *
		_namenode_pending_remove(struct hdfs_namenode *n, int64_t msgno);

static void	_future_complete(struct hdfs_rpc_response_future *future,
		struct hdfs_object *obj);
static void	_future_complete_unlocked(struct hdfs_rpc_response_future *f,
		struct hdfs_object *o);

void
hdfs_namenode_init(struct hdfs_namenode *n)
{
	n->nn_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	n->nn_sendlock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	n->nn_refs = 1;
	n->nn_sock = -1;
	n->nn_dead = false;
	n->nn_authed = false;
	n->nn_msgno = 0;
	n->nn_destroy_cb = NULL;
	n->nn_pending = NULL;
	n->nn_pending_len = 0;
	n->nn_worked = false;

	n->nn_recvbuf = NULL;
	n->nn_recvbuf_used = 0;
	n->nn_recvbuf_size = 0;
}

const char *
hdfs_namenode_connect(struct hdfs_namenode *n, const char *host, const char *port)
{
	const char *err = NULL;

	_lock(&n->nn_lock);
	assert(n->nn_sock == -1);

	err = _connect(&n->nn_sock, host, port);
	if (err)
		goto out;

out:
	_unlock(&n->nn_lock);
	return err;
}

const char *
hdfs_namenode_authenticate(struct hdfs_namenode *n, const char *username)
{
	const char *err = NULL, *preamble = "hrpc\x04\x50";
	struct hdfs_object *header;
	struct hdfs_heap_buf hbuf = { 0 };

	_lock(&n->nn_lock);
	assert(!n->nn_authed);
	assert(n->nn_sock != -1);

	// Create / serialize the connection header object
	header = hdfs_authheader_new(username);
	hdfs_object_serialize(&hbuf, header);
	hdfs_object_free(header);

	// Prefix the header object with the protocol preamble
	hbuf.buf = realloc(hbuf.buf, hbuf.size + strlen(preamble));
	assert(hbuf.buf);
	memmove(hbuf.buf + strlen(preamble), hbuf.buf, hbuf.used);
	memcpy(hbuf.buf, preamble, strlen(preamble));
	hbuf.used += strlen(preamble);
	hbuf.size += strlen(preamble);

	// Write the entire thing to the socket. Honestly, the first write
	// should succeed (we have the entire outbound sockbuf) but we call it
	// in a loop for correctness.
	err = _write_all(n->nn_sock, hbuf.buf, hbuf.used);
	n->nn_authed = true;

	_unlock(&n->nn_lock);
	free(hbuf.buf);

	return err;
}

const char *
hdfs_namenode_invoke(struct hdfs_namenode *n, struct hdfs_object *rpc,
	struct hdfs_rpc_response_future *future)
{
	const char *err = NULL;
	bool nnlocked;
	int64_t msgno;
	struct hdfs_heap_buf hbuf = { 0 };

	assert(future);
	assert(!future->fu_namenode);

	assert(rpc);
	assert(rpc->ob_type == H_RPC_INVOCATION);

	_lock(&n->nn_lock);
	nnlocked = true;

	assert(n->nn_refs >= 1);
	assert(!n->nn_dead);

	if (n->nn_sock == -1) {
		err = "Not connected";
		goto out;
	}
	if (!n->nn_authed) {
		err = "Not authenticated";
		goto out;
	}

	// Take a number
	msgno = n->nn_msgno;
	n->nn_msgno++;

	future->fu_namenode = _namenode_copyref_unlocked(n);
	_namenode_pending_insert_unlocked(n, msgno, future);

	_unlock(&n->nn_lock);
	nnlocked = false;

	// Serialize rpc and transmit
	_rpc_invocation_set_msgno(rpc, msgno);
	hdfs_object_serialize(&hbuf, rpc);

	_lock(&n->nn_sendlock);
	err = _write_all(n->nn_sock, hbuf.buf, hbuf.used);
	_unlock(&n->nn_sendlock);

	free(hbuf.buf);

out:
	if (nnlocked)
		_unlock(&n->nn_lock);
	return err;
}

void
hdfs_future_get(struct hdfs_rpc_response_future *future, struct hdfs_object **object)
{
	bool found;

	_lock(&future->fu_lock);
	while (!future->fu_res) {
		found = _namenode_recv(future);
		if (found) {
			assert(future->fu_res);
			break;
		}

		/* we can't miss the wake-up here because we're holding fu_lock */

		_wait(&future->fu_lock, &future->fu_cond);
	}
	_unlock(&future->fu_lock);
	*object = future->fu_res;

	_namenode_decref(future->fu_namenode);
	memset(future, 0, sizeof *future);
}

// Caller *must not* use the namenode object after this. If 'cb' is non-null,
// they will receive a callback when the memory is no longer used, which they
// can use to free memory or whatever.
void
hdfs_namenode_destroy(struct hdfs_namenode *n, hdfs_namenode_destroy_cb cb)
{
	_lock(&n->nn_lock);
	assert(n->nn_refs >= 1);
	assert(!n->nn_dead);
	n->nn_dead = true;
	n->nn_destroy_cb = cb;
	_unlock(&n->nn_lock);

	_namenode_decref(n);
}

// Caller must have n->nn_lock.
static struct hdfs_namenode *
_namenode_copyref_unlocked(struct hdfs_namenode *n)
{
	assert(n->nn_refs >= 1);
	n->nn_refs++;
	return n;
}

static void
_namenode_decref(struct hdfs_namenode *n)
{
	bool lastref = false;

	_lock(&n->nn_lock);
	assert(n->nn_refs >= 1);
	n->nn_refs--;
	if (n->nn_refs == 0)
		lastref = true;
	_unlock(&n->nn_lock);

	if (lastref) {
		hdfs_namenode_destroy_cb dcb = NULL;
		if (n->nn_sock != -1)
			close(n->nn_sock);
		if (n->nn_pending)
			free(n->nn_pending);
		if (n->nn_recvbuf)
			free(n->nn_recvbuf);
		if (n->nn_destroy_cb)
			dcb = n->nn_destroy_cb;
		memset(n, 0, sizeof *n);
		if (dcb)
			dcb(n);
	}
}

struct _hdfs_pending {
	int64_t pd_msgno;
	struct hdfs_rpc_response_future *pd_future;
};

static bool
_namenode_recv(struct hdfs_rpc_response_future *goal_future)
{
	const size_t RESIZE = 16*1024;

	bool nnlocked = false, res = false, nnworked;
	int sock, obj_size;

	struct _hdfs_result *result;
	struct hdfs_rpc_response_future *future;
	struct hdfs_namenode *n = goal_future->fu_namenode;

	_lock(&n->nn_lock);
	nnworked = n->nn_worked;
	if (!nnworked)
		n->nn_worked = true;
	_unlock(&n->nn_lock);

	// someone else is working this connection for now
	if (nnworked)
		goto out;

	// only thread should be able to enter this section at a time:
	while (true) {
		_lock(&n->nn_lock);
		nnlocked = true;
		sock = n->nn_sock;

		// If hdfs_namenode_destroy() happened, die:
		if (n->nn_dead || n->nn_refs == 1)
			break;

		_unlock(&n->nn_lock);
		nnlocked = false;

		assert(sock != -1);

		result = _hdfs_result_deserialize(n->nn_recvbuf, n->nn_recvbuf_used, &obj_size);
		if (!result) {
			int r, remain = n->nn_recvbuf_size - n->nn_recvbuf_used;

			if (remain < 4*1024) {
				n->nn_recvbuf_size += RESIZE;
				n->nn_recvbuf = realloc(n->nn_recvbuf, n->nn_recvbuf_size);
				assert(n->nn_recvbuf);
				remain = n->nn_recvbuf_size - n->nn_recvbuf_used;
			}

			while (true) {
				r = read(sock, n->nn_recvbuf + n->nn_recvbuf_used, remain);
				if (r == 0)
					goto out;
				if (r > 0) {
					n->nn_recvbuf_used += r;
					break;
				}

				// bail on socket errors
				_lock(&n->nn_lock);
				close(n->nn_sock);
				n->nn_sock = -1;
				n->nn_dead = true;
				_unlock(&n->nn_lock);

				// We need to do something more intelligent if
				// we want to handle socket errors gracefully.
				assert(false);

				break;
			}

			continue;
		}

		if (result == _HDFS_INVALID_PROTO) {
			// bail on protocol errors
			_lock(&n->nn_lock);
			close(n->nn_sock);
			n->nn_sock = -1;
			n->nn_dead = true;
			_unlock(&n->nn_lock);

			// We should do something more intelligent if we want
			// to handle bad protocol data gracefully.
			assert(false);
			break;
		}

		// if we got here, we have read a valid / complete hdfs result
		// off the wire; skip the buffer forward:

		n->nn_recvbuf_used -= obj_size;
		if (n->nn_recvbuf_used)
			memmove(n->nn_recvbuf, n->nn_recvbuf + obj_size, n->nn_recvbuf_used);

		future = _namenode_pending_remove(n, result->rs_msgno);
		assert(future); // got a response to a msgno we didn't request

		if (future == goal_future)
			_future_complete_unlocked(future, result->rs_obj);
		else
			_future_complete(future, result->rs_obj);

		// don't free the object we just handed the user:
		result->rs_obj = NULL;
		_hdfs_result_free(result);

		// all done here, we found our response
		if (future == goal_future) {
			res = true;
			break;
		}
	}

	// End the critical section:
	if (!nnlocked)
		_lock(&n->nn_lock);
	assert(n->nn_worked);
	n->nn_worked = false;

	// If any thread is pending, wake it
	if (n->nn_pending_len > 0) {
		for (int i = 0; i < n->nn_pending_len; i++) {
			struct hdfs_rpc_response_future *pf = n->nn_pending[0].pd_future;
			if (pf == goal_future)
				continue;
			_lock(&pf->fu_lock);
			_notifyall(&pf->fu_cond);
			_unlock(&pf->fu_lock);
			break;
		}
	}
	_unlock(&n->nn_lock);
	nnlocked = false;

out:
	if (nnlocked)
		_unlock(&n->nn_lock);

	return res;
}

static void
_namenode_pending_insert_unlocked(struct hdfs_namenode *n, int64_t msgno,
	struct hdfs_rpc_response_future *future)
{
	const int RESIZE_FACTOR = 16;

	if (n->nn_pending_len % RESIZE_FACTOR == 0) {
		n->nn_pending = realloc(n->nn_pending,
		    (n->nn_pending_len + RESIZE_FACTOR) * sizeof *n->nn_pending);
		assert(n->nn_pending);
	}

	n->nn_pending[n->nn_pending_len].pd_msgno = msgno;
	n->nn_pending[n->nn_pending_len].pd_future = future;
	n->nn_pending_len++;
}

static struct hdfs_rpc_response_future *
_namenode_pending_remove(struct hdfs_namenode *n, int64_t msgno)
{
	struct hdfs_rpc_response_future *res = NULL;

	_lock(&n->nn_lock);

	for (int i = 0; i < n->nn_pending_len; i++) {
		if (n->nn_pending[i].pd_msgno == msgno) {
			res = n->nn_pending[i].pd_future;
			if (i != n->nn_pending_len - 1)
				n->nn_pending[i] = n->nn_pending[n->nn_pending_len - 1];
			n->nn_pending_len--;
			break;
		}
	}

	_unlock(&n->nn_lock);

	return res;
}

static void
_future_complete(struct hdfs_rpc_response_future *f, struct hdfs_object *o)
{
	assert(o);

	_lock(&f->fu_lock);

	assert(!f->fu_res);
	f->fu_res = o;
	_notifyall(&f->fu_cond);

	_unlock(&f->fu_lock);
}

static void
_future_complete_unlocked(struct hdfs_rpc_response_future *f, struct hdfs_object *o)
{
	assert(o);

	assert(!f->fu_res);
	f->fu_res = o;
}
