/**
 * Copyright (c) 2018 Cornell University.
 *
 * Author: Ted Yin <tederminant@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SALTICIDAE_CONN_H
#define _SALTICIDAE_CONN_H

#ifdef __cplusplus
#include <cassert>
#include <cstdint>
#include <arpa/inet.h>
#include <unistd.h>

#include <string>
#include <unordered_map>
#include <list>
#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <fcntl.h>

#include "salticidae/type.h"
#include "salticidae/ref.h"
#include "salticidae/event.h"
#include "salticidae/util.h"
#include "salticidae/netaddr.h"
#include "salticidae/msg.h"
#include "salticidae/buffer.h"

namespace salticidae {

struct ConnPoolError: public SalticidaeError {
    using SalticidaeError::SalticidaeError;
};

/** Abstraction for connection management. */
class ConnPool {
    class Worker;
    public:
    class Conn;
    /** The handle to a bi-directional connection. */
    using conn_t = ArcObj<Conn>;
    /** The type of callback invoked when connection status is changed. */
    using conn_callback_t = std::function<void(const conn_t &, bool)>;
    /** Abstraction for a bi-directional connection. */
    class Conn {
        friend ConnPool;
        public:
        enum ConnMode {
            ACTIVE, /**< the connection is established by connect() */
            PASSIVE, /**< the connection is established by accept() */
            DEAD, /**< the connection is dead */
        };
    
        protected:
        size_t seg_buff_size;
        conn_t self_ref;
        std::mutex ref_mlock;
        int fd;
        Worker *worker;
        ConnPool *cpool;
        std::atomic<ConnMode> mode;
        NetAddr addr;

        MPSCWriteBuffer send_buffer;
        SegBuffer recv_buffer;

        TimedFdEvent ev_connect;
        FdEvent ev_socket;
        TimerEvent ev_send_wait;
        /** does not need to wait if true */
        bool ready_send;
    
        void recv_data(int, int);
        void send_data(int, int);
        void conn_server(int, int);

        /** Terminate the connection (from the worker thread). */
        void worker_terminate();
        /** Terminate the connection (from the dispatcher thread). */
        void disp_terminate();

        public:
        Conn(): ready_send(false) {}
        Conn(const Conn &) = delete;
        Conn(Conn &&other) = delete;
    
        virtual ~Conn() {
            SALTICIDAE_LOG_INFO("destroyed %s", std::string(*this).c_str());
        }

        /** Get the handle to itself. */
        conn_t self() {
            mutex_lg_t _(ref_mlock);
            return self_ref;
        }

        void release_self() {
            mutex_lg_t _(ref_mlock);
            self_ref = nullptr;
        }

        operator std::string() const;
        const NetAddr &get_addr() const { return addr; }
        ConnMode get_mode() const { return mode; }
        ConnPool *get_pool() const { return cpool; }
        MPSCWriteBuffer &get_send_buffer() { return send_buffer; }

        /** Write data to the connection (non-blocking). The data will be sent
         * whenever I/O is available. */
        bool write(bytearray_t &&data) {
            return send_buffer.push(std::move(data), !cpool->queue_capacity);
        }

        protected:
        /** Close the IO and clear all on-going or planned events. Remove the
         * connection from a Worker. */
        virtual void stop();
        /** Called when new data is available. */
        virtual void on_read() {}
        /** Called when the underlying connection is established. */
        virtual void on_setup() {}
        /** Called when the underlying connection breaks. */
        virtual void on_teardown() {}
    };

    protected:
    EventContext ec;
    EventContext disp_ec;
    ThreadCall* disp_tcall;
    /** Should be implemented by derived class to return a new Conn object. */
    virtual Conn *create_conn() = 0;

    private:
    const int max_listen_backlog;
    const double conn_server_timeout;
    const size_t seg_buff_size;
    const size_t queue_capacity;

    /* owned by user loop */
    BoxObj<ThreadCall> user_tcall;
    conn_callback_t conn_cb;

    /* owned by the dispatcher */
    FdEvent ev_listen;
    std::unordered_map<int, conn_t> pool;
    int listen_fd;  /**< for accepting new network connections */

    void update_conn(const conn_t &conn, bool connected) {
        user_tcall->async_call([this, conn, connected](ThreadCall::Handle &) {
            if (conn_cb) conn_cb(conn, connected);
        });
    }

    class Worker {
        EventContext ec;
        ThreadCall tcall;
        std::thread handle;
        bool disp_flag;
        std::atomic<size_t> nconn;

        public:
        Worker(): tcall(ec), disp_flag(false), nconn(0) {}

        /* the following functions are called by the dispatcher */
        void start() {
            handle = std::thread([this]() { ec.dispatch(); });
        }

        void feed(const conn_t &conn, int client_fd) {
            /* the caller should finalize all the preparation */
            tcall.async_call([this, conn, client_fd](ThreadCall::Handle &) {
                if (conn->mode == Conn::ConnMode::DEAD)
                {
                    SALTICIDAE_LOG_INFO("worker %x discarding dead connection",
                        std::this_thread::get_id());
                    return;
                }
                assert(conn->fd != -1);
                SALTICIDAE_LOG_INFO("worker %x got %s",
                        std::this_thread::get_id(),
                        std::string(*conn).c_str());
                conn->get_send_buffer()
                        .get_queue()
                        .reg_handler(this->ec, [conn, client_fd]
                                    (MPSCWriteBuffer::queue_t &) {
                    if (conn->ready_send)
                    {
                        conn->ev_socket.del();
                        conn->ev_socket.add(FdEvent::READ | FdEvent::WRITE);
                        conn->send_data(client_fd, FdEvent::WRITE);
                    }
                    return false;
                });
                conn->ev_socket = FdEvent(ec, client_fd, [conn=conn](int fd, int what) {
                    if (what & FdEvent::READ)
                        conn->recv_data(fd, what);
                    else
                        conn->send_data(fd, what);
                });
                conn->ev_socket.add(FdEvent::READ | FdEvent::WRITE);
                nconn++;
            });
        }

        void unfeed() { nconn--; }

        void stop() {
            tcall.async_call([this](ThreadCall::Handle &) { ec.stop(); });
        }

        std::thread &get_handle() { return handle; }
        const EventContext &get_ec() { return ec; }
        ThreadCall *get_tcall() { return &tcall; }
        void set_dispatcher() { disp_flag = true; }
        bool is_dispatcher() const { return disp_flag; }
        size_t get_nconn() { return nconn; }
    };

    /* related to workers */
    size_t nworker;
    salticidae::BoxObj<Worker[]> workers;
    bool worker_running;

    void accept_client(int, int);
    conn_t add_conn(const conn_t &conn);
    void del_conn(const conn_t &conn);

    protected:
    conn_t _connect(const NetAddr &addr);
    void _listen(NetAddr listen_addr);

    private:

    //class DspMulticast: public DispatchCmd {
    //    std::vector<conn_t> receivers;
    //    bytearray_t data;
    //    public:
    //    DspMulticast(std::vector<conn_t> &&receivers, bytearray_t &&data):
    //        receivers(std::move(receivers)),
    //        data(std::move(data)) {}
    //    void exec(ConnPool *) override {
    //        for (auto &r: receivers) r->write(bytearray_t(data));
    //    }
    //};

    Worker &select_worker() {
        size_t idx = 0;
        size_t best = workers[idx].get_nconn();
        for (size_t i = 0; i < nworker; i++)
        {
            size_t t = workers[i].get_nconn();
            if (t < best)
            {
                best = t;
                idx = i;
            }
        }
        return workers[idx];
    }

    public:

    class Config {
        friend ConnPool;
        int _max_listen_backlog;
        double _conn_server_timeout;
        size_t _seg_buff_size;
        size_t _nworker;
        size_t _queue_capacity;

        public:
        Config():
            _max_listen_backlog(10),
            _conn_server_timeout(2),
            _seg_buff_size(4096),
            _nworker(1),
            _queue_capacity(0) {}

        Config &max_listen_backlog(int x) {
            _max_listen_backlog = x;
            return *this;
        }

        Config &conn_server_timeout(double x) {
            _conn_server_timeout = x;
            return *this;
        }

        Config &seg_buff_size(size_t x) {
            _seg_buff_size = x;
            return *this;
        }

        Config &nworker(size_t x) {
            _nworker = std::max((size_t)1, x);
            return *this;
        }

        Config &queue_capacity(size_t x) {
            _queue_capacity = x;
            return *this;
        }
    };

    ConnPool(const EventContext &ec, const Config &config):
            ec(ec),
            max_listen_backlog(config._max_listen_backlog),
            conn_server_timeout(config._conn_server_timeout),
            seg_buff_size(config._seg_buff_size),
            queue_capacity(config._queue_capacity),
            listen_fd(-1),
            nworker(config._nworker),
            worker_running(false) {
        workers = new Worker[nworker];
        user_tcall = new ThreadCall(ec);
        disp_ec = workers[0].get_ec();
        disp_tcall = workers[0].get_tcall();
        workers[0].set_dispatcher();
    }

    ~ConnPool() { stop(); }

    ConnPool(const ConnPool &) = delete;
    ConnPool(ConnPool &&) = delete;

    void start() {
        if (worker_running) return;
        SALTICIDAE_LOG_INFO("starting all threads...");
        for (size_t i = 0; i < nworker; i++)
            workers[i].start();
        worker_running = true;
    }

    void stop_workers() {
        if (!worker_running) return;
        worker_running = false;
        SALTICIDAE_LOG_INFO("stopping all threads...");
        /* stop the dispatcher */
        workers[0].stop();
        workers[0].get_handle().join();
        /* stop all workers */
        for (size_t i = 1; i < nworker; i++)
            workers[i].stop();
        /* join all worker threads */
        for (size_t i = 1; i < nworker; i++)
            workers[i].get_handle().join();
    }

    void stop() {
        stop_workers();
        for (auto it: pool)
        {
            conn_t conn = it.second;
            conn->stop();
            conn->self_ref = nullptr;
            ::close(conn->fd);
        }
        if (listen_fd != -1)
        {
            close(listen_fd);
            listen_fd = -1;
        }
    }

    /** Actively connect to remote addr. */
    conn_t connect(const NetAddr &addr, bool blocking = true) {
        if (blocking)
        {
            auto ret = *(static_cast<conn_t *>(disp_tcall->call(
                        [this, addr](ThreadCall::Handle &h) {
                auto conn = _connect(addr);
                h.set_result(std::move(conn));
            }).get()));
            return std::move(ret);
        }
        else
        {
            disp_tcall->async_call([this, addr](ThreadCall::Handle &) {
                _connect(addr);
            });
            return nullptr;
        }
    }

    /** Listen for passive connections (connection initiated from remote).
     * Does not need to be called if do not want to accept any passive
     * connections. */
    void listen(NetAddr listen_addr) {
        disp_tcall->call([this, listen_addr](ThreadCall::Handle &) {
            _listen(listen_addr);
        });
    }

    template<typename Func>
    void reg_conn_handler(Func cb) { conn_cb = cb; }

    void terminate(const conn_t &conn) {
        disp_tcall->async_call([this, conn](ThreadCall::Handle &) {
            conn->disp_terminate();
        });
    }
};

}

#endif

#endif
