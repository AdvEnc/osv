#include "af_local.h"
#include "osv/file.h"
#include "osv/mutex.h"
#include "osv/types.h"
#include <deque>
#include <atomic>
#include <memory>
#include <boost/intrusive_ptr.hpp>
#include <sys/stat.h>
#include <sys/socket.h>
#include <osv/fcntl.h>
#include <osv/condvar.h>
#include "fs/fs.hh"
#include "fs/unsupported.h"
#include "libc.hh"
#include <osv/poll.h>
#include <debug.hh>
#include <unistd.h>
#include <fcntl.h>

struct af_local_buffer {
    static constexpr size_t max_buf = 8192;
public:
    af_local_buffer() = default;
    af_local_buffer(const af_local_buffer&) = delete;
    int read(uio* data);
    int write(uio* data);
    int read_events();
    int write_events();
    void detach_sender();
    void detach_receiver();
    void attach_sender(struct file *f);
    void attach_receiver(struct file *f);
private:
    int read_events_unlocked();
    int write_events_unlocked();
private:
    mutex mtx;
    std::deque<char> q;
    struct file *receiver = nullptr;
    struct file *sender = nullptr;
    std::atomic<unsigned> refs = {};
    condvar may_read;
    condvar may_write;
    friend void intrusive_ptr_add_ref(af_local_buffer* p) {
        p->refs.fetch_add(1, std::memory_order_relaxed);
    }
    friend void intrusive_ptr_release(af_local_buffer* p) {
        if (p->refs.fetch_add(-1, std::memory_order_acquire) == 1) {
            delete p;
        }
    }
};

typedef boost::intrusive_ptr<af_local_buffer> af_local_buffer_ref;

void af_local_buffer::detach_sender()
{
    std::lock_guard<mutex> guard(mtx);
    if (sender) {
        sender = nullptr;
        if (receiver)
            poll_wake(receiver, POLLRDHUP);
        may_read.wake_all();
    }
}

void af_local_buffer::detach_receiver()
{
    std::lock_guard<mutex> guard(mtx);
    if (receiver) {
        receiver = nullptr;
        if (sender)
            poll_wake(sender, POLLHUP);
        may_write.wake_all();
    }
}

void af_local_buffer::attach_sender(struct file *f)
{
    assert(sender == nullptr);
    sender = f;
}

void af_local_buffer::attach_receiver(struct file *f)
{
    assert(receiver == nullptr);
    receiver = f;
}

int af_local_buffer::read_events_unlocked()
{
    int ret = 0;
    ret |= !q.empty() ? POLLIN : 0;
    ret |= !sender ? POLLRDHUP : 0;
    return ret;
}

int af_local_buffer::write_events_unlocked()
{
    if (!receiver) {
        return POLLHUP;
    }
    int ret = 0;
    ret |= q.size() < max_buf ? POLLOUT : 0;
    return ret;
}

int af_local_buffer::read_events()
{
    return with_lock(mtx, [=] { return read_events_unlocked(); });
}

int af_local_buffer::write_events()
{
    return with_lock(mtx, [=] { return write_events_unlocked(); });
}

// Copy from the pipe into the given iovec array, until the array is full
// or the queue is empty. Decrements uio->uio_resid.
static void copy_to_uio(std::deque<char> &q, uio *uio)
{
    for (int i = 0; i < uio->uio_iovcnt && !q.empty(); i++) {
        auto &iov = uio->uio_iov[i];
        auto n = std::min(q.size(), iov.iov_len);
        char* p = static_cast<char*>(iov.iov_base);
        std::copy(q.begin(), q.begin() + n, p);
        q.erase(q.begin(), q.begin() + n);
        uio->uio_resid -= n;
    }
}

int af_local_buffer::read(uio* data)
{
    if (!data->uio_resid) {
        return 0;
    }
    with_lock(mtx, [&] {
        int r;
        while ((r = read_events_unlocked()) == 0) {
            may_read.wait(&mtx);
        }
        if (!(r & POLLIN)) {
            assert(r & POLLRDHUP);
            return;
        }
        copy_to_uio(q, data);
        if (write_events_unlocked() & POLLOUT)
            poll_wake(sender, (POLLOUT | POLLWRNORM));
    });
    may_write.wake_all();
    return 0;
}

// Copy from a certain iovec array into a pipe, starting at a given index
// and offset, until the buffer or the array ends. Decrements uio->uio_resid,
// and modifies ind and offset to where the copy stopped.
static void copy_from_uio(uio *uio, size_t *ind, size_t *offset,
        std::deque<char> &buf, size_t bufsize)
{
    int i = *ind;
    size_t off = *offset;

    while (i < uio->uio_iovcnt && buf.size() < bufsize) {
        auto &iov = uio->uio_iov[i];
        auto n = std::min(bufsize - buf.size(), iov.iov_len - off);
        char* p = static_cast<char*>(iov.iov_base) + off;
        std::copy(p, p + n, std::back_inserter(buf));
        uio->uio_resid -= n;
        off += n;
        if (off == iov.iov_len) {
            ++i;
            off = 0;
        }
    }

    *offset = off;
    *ind = i;
}

int af_local_buffer::write(uio* data)
{
    if (!data->uio_resid) {
        return 0;
    }
    int err = 0;
    with_lock(mtx, [&] {
        // FIXME: Should support also non-blocking operation (O_NONBLOCK).
        // A write() smaller than PIPE_BUF (=4096 in Linux) will not be split
        // (i.e., will be "atomic"): For such a small write, we need to wait
        // until there's enough room for all it in the buffer.
        int needroom = data->uio_resid <= 4096 ? data->uio_resid : 1;
        while (receiver && q.size() + needroom > max_buf) {
            may_write.wait(&mtx);
        }

        if (!receiver) {
            // FIXME: If we don't generate a SIGPIPE here, at least assert
            // that SIGPIPE is SIG_IGN (which can be our default); If the
            // user installed
            err = EPIPE;
            return;
        }

        // A blocking write() to a pipe never returns with partial success -
        // it waits, possibly writing its output in parts and waiting multiple
        // times, until the whole given buffer is written.
        size_t ind = 0, offset = 0;
        while (data->uio_resid && receiver) {
            copy_from_uio(data, &ind, &offset, q, max_buf);
            if (data->uio_resid) {
                // The buffer is full but we still have more to send. Wake up
                // readers, and go to sleep ourselves.
                assert(q.size() == max_buf);
                poll_wake(receiver, (POLLIN | POLLRDNORM));
                may_read.wake_all();
                while (receiver && q.size() == max_buf) {
                    may_write.wait(&mtx);
                }
            }
        }
        if (read_events_unlocked() & POLLIN)
            poll_wake(receiver, (POLLIN | POLLRDNORM));
    });
    may_read.wake_all();
    return err;
}

struct af_local {
    af_local(af_local_buffer* s, af_local_buffer* r)
        : send(s), receive(r) {}
    ~af_local() {
        send->detach_sender();
        receive->detach_receiver();
    }
    af_local_buffer_ref send;
    af_local_buffer_ref receive;
};

int af_local_init(file* f)
{
    af_local* afl = static_cast<af_local*>(f->f_data);
    afl->send->attach_sender(f);
    afl->receive->attach_receiver(f);
    return 0;
}

int af_local_read(file* f, uio* data, int flags)
{
    af_local* afl = static_cast<af_local*>(f->f_data);
    // FIXME: Should support also non-blocking operation in
    // af_local_read/write and pipe_read/write.
    assert(!(f->f_flags & FNONBLOCK));
    return afl->receive->read(data);
}

int af_local_write(file* f, uio* data, int flags)
{
    af_local* afl = static_cast<af_local*>(f->f_data);
    assert(!(f->f_flags & FNONBLOCK));
    return afl->send->write(data);
}

int af_local_poll(file* f, int events)
{
    af_local* afl = static_cast<af_local*>(f->f_data);
    int revents = 0;
    if (events & POLLIN) {
        revents |= afl->receive->read_events();
    }
    if (events & POLLOUT) {
        revents |= afl->send->write_events();
    }
    return revents;
}

int af_local_close(file* f)
{
    auto afl = static_cast<af_local*>(f->f_data);
    delete afl;
    f->f_data = nullptr;
    return 0;
}

fileops af_local_ops = {
    af_local_init,
    af_local_read,
    af_local_write,
    unsupported_truncate,
    unsupported_ioctl,
    af_local_poll,
    unsupported_stat,
    af_local_close,
    unsupported_chmod,
};

int socketpair_af_local(int type, int proto, int sv[2])
{
    assert(type == SOCK_STREAM);
    assert(proto == 0);
    auto b1 = new af_local_buffer;
    auto b2 = new af_local_buffer;
    std::unique_ptr<af_local> s1{new af_local(b1, b2)};
    std::unique_ptr<af_local> s2{new af_local(b2, b1)};
    try {
        fileref f1{falloc_noinstall()};
        fileref f2{falloc_noinstall()};
        finit(f1.get(), FREAD|FWRITE, DTYPE_UNSPEC, s1.release(), &af_local_ops);
        finit(f2.get(), FREAD|FWRITE, DTYPE_UNSPEC, s2.release(), &af_local_ops);
        fdesc fd1(f1);
        fdesc fd2(f2);
        // all went well, user owns descriptors now
        sv[0] = fd1.release();
        sv[1] = fd2.release();
        return 0;
    } catch (int error) {
        return libc_error(error);
    }
}

// Also implement pipes using the same af_local_buffer mechanism:

struct pipe_writer {
    af_local_buffer_ref buf;
    pipe_writer(af_local_buffer *b) : buf(b) { }
    ~pipe_writer() { buf->detach_sender(); }
};

struct pipe_reader {
    af_local_buffer_ref buf;
    pipe_reader(af_local_buffer *b) : buf(b) { }
    ~pipe_reader() { buf->detach_receiver(); }
};

int pipe_init(file* f)
{
    if (f->f_flags & FWRITE) {
        pipe_writer *po = static_cast<pipe_writer*>(f->f_data);
        po->buf->attach_sender(f);
    } else {
        pipe_reader *po = static_cast<pipe_reader*>(f->f_data);
        po->buf->attach_receiver(f);
    }
    return 0;
}

int pipe_read(file *f, uio *data, int flags)
{
    pipe_reader *po = static_cast<pipe_reader*>(f->f_data);
    assert(!(f->f_flags & FNONBLOCK));
    return po->buf->read(data);
}

int pipe_write(file *f, uio *data, int flags)
{
    pipe_writer *po = static_cast<pipe_writer*>(f->f_data);
    assert(!(f->f_flags & FNONBLOCK));
    return po->buf->write(data);
}

int pipe_poll(file *f, int events)
{
    int revents = 0;
    // One end of the pipe is read-only, the other write-only:
    if (f->f_flags & FWRITE) {
        pipe_writer *po = static_cast<pipe_writer*>(f->f_data);
        if (events & POLLOUT) {
            revents |= po->buf->write_events();
        }
    } else {
        pipe_reader *po = static_cast<pipe_reader*>(f->f_data);
        if (events & POLLIN) {
            revents |= po->buf->read_events();
        }
    }
    return revents;
}

int pipe_close(file *f)
{
    if (f->f_flags & FWRITE) {
        delete static_cast<pipe_writer*>(f->f_data);
    } else {
        delete static_cast<pipe_reader*>(f->f_data);
    }
    f->f_data = nullptr;
    return 0;
}

fileops pipe_ops = {
    pipe_init,
    pipe_read,
    pipe_write,
    unsupported_truncate,
    unsupported_ioctl,
    pipe_poll,
    unsupported_stat,
    pipe_close,
    unsupported_chmod,
};

int pipe(int pipefd[2]) {
    auto b = new af_local_buffer;
    std::unique_ptr<pipe_reader> s1{new pipe_reader(b)};
    std::unique_ptr<pipe_writer> s2{new pipe_writer(b)};
    try {
        fileref f1{falloc_noinstall()};
        fileref f2{falloc_noinstall()};
        finit(f1.get(), FREAD, DTYPE_UNSPEC, s1.release(), &pipe_ops);
        finit(f2.get(), FWRITE, DTYPE_UNSPEC, s2.release(), &pipe_ops);
        fdesc fd1(f1);
        fdesc fd2(f2);
        // all went well, user owns descriptors now
        pipefd[0] = fd1.release();
        pipefd[1] = fd2.release();
        return 0;
    } catch (int error) {
        return libc_error(error);
    }
}
