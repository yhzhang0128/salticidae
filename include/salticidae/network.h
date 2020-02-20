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

#ifndef _SALTICIDAE_NETWORK_H
#define _SALTICIDAE_NETWORK_H

#include "salticidae/event.h"
#include "salticidae/crypto.h"
#include "salticidae/netaddr.h"
#include "salticidae/msg.h"
#include "salticidae/conn.h"

#ifdef __cplusplus
#include <unordered_set>
#include <shared_mutex>
#include <openssl/rand.h>
namespace salticidae {
/** Network of nodes who can send async messages.  */
template<typename OpcodeType>
class MsgNetwork: public ConnPool {
    public:
    using Msg = MsgBase<OpcodeType>;
    /* match lambdas */
    template<typename T>
    struct callback_traits:
        public callback_traits<decltype(&T::operator())> {};

    /* match plain functions */
    template<typename ReturnType, typename MsgType, typename ConnType>
    struct callback_traits<ReturnType(MsgType, ConnType)> {
        using ret_type = ReturnType;
        using conn_type = typename std::remove_reference<ConnType>::type::type;
        using msg_type = typename std::remove_reference<MsgType>::type;
    };

    /* match function pointers */
    template<typename ReturnType, typename... Args>
    struct callback_traits<ReturnType(*)(Args...)>:
        public callback_traits<ReturnType(Args...)> {};

    /* match const member functions */
    template<typename ClassType, typename ReturnType, typename... Args>
    struct callback_traits<ReturnType(ClassType::*)(Args...) const>:
        public callback_traits<ReturnType(Args...)> {};

    /* match member functions */
    template<typename ClassType, typename ReturnType, typename... Args>
    struct callback_traits<ReturnType(ClassType::*)(Args...)>:
        public callback_traits<ReturnType(Args...)> {};

    class Conn: public ConnPool::Conn {
        friend MsgNetwork;
        enum MsgState {
            HEADER,
            PAYLOAD
        };

        Msg msg;
        MsgState msg_state;
        bool msg_sleep;
        TimerEvent ev_enqueue_poll;

        protected:
#ifdef SALTICIDAE_MSG_STAT
        mutable std::atomic<size_t> nsent;
        mutable std::atomic<size_t> nrecv;
        mutable std::atomic<size_t> nsentb;
        mutable std::atomic<size_t> nrecvb;
#endif

        public:
        Conn(): msg_state(HEADER), msg_sleep(false)
#ifdef SALTICIDAE_MSG_STAT
            , nsent(0), nrecv(0), nsentb(0), nrecvb(0)
#endif
        {}

        MsgNetwork *get_net() {
            return static_cast<MsgNetwork *>(get_pool());
        }

#ifdef SALTICIDAE_MSG_STAT
        size_t get_nsent() const { return nsent; }
        size_t get_nrecv() const { return nrecv; }
        size_t get_nsentb() const { return nsentb; }
        size_t get_nrecvb() const { return nrecvb; }
        void clear_msgstat() const {
            nsent.store(0, std::memory_order_relaxed);
            nrecv.store(0, std::memory_order_relaxed);
            nsentb.store(0, std::memory_order_relaxed);
            nrecvb.store(0, std::memory_order_relaxed);
        }
#endif
    };

    using conn_t = ArcObj<Conn>;
#ifdef SALTICIDAE_MSG_STAT
    // TODO: a lock-free, thread-safe, fine-grained stat
#endif

    private:
    const size_t max_msg_size;
    const size_t max_msg_queue_size;
    std::unordered_map<
        typename Msg::opcode_t,
        std::function<void(const Msg &msg, const conn_t &)>> handler_map;
    using queue_t = MPSCQueueEventDriven<std::pair<Msg, conn_t>>;
    queue_t incoming_msgs;

    protected:
    const uint32_t msg_magic;
    ConnPool::Conn *create_conn() override { return new Conn(); }
    void on_read(const ConnPool::conn_t &) override;

    void on_setup(const ConnPool::conn_t &_conn) override {
        auto conn = static_pointer_cast<Conn>(_conn);
        conn->ev_enqueue_poll = TimerEvent(conn->worker->get_ec(),
                [this, conn](TimerEvent &) {
            if (!incoming_msgs.enqueue(std::make_pair(conn->msg, conn), false))
            {
                conn->msg_sleep = true;
                conn->ev_enqueue_poll.add(0);
                return;
            }
            conn->msg_sleep = false;
            on_read(conn);
        });
    }

    void on_teardown(const ConnPool::conn_t &_conn) override {
        auto conn = static_pointer_cast<Conn>(_conn);
        conn->ev_enqueue_poll.clear();
    }

    public:

    class Config: public ConnPool::Config {
        friend class MsgNetwork;
        size_t _max_msg_size;
        size_t _max_msg_queue_size;
        size_t _burst_size;
        uint32_t _msg_magic;

        public:
        Config(): Config(ConnPool::Config()) {}
        Config(const ConnPool::Config &config):
            ConnPool::Config(config),
            _max_msg_size(1024),
            _max_msg_queue_size(65536),
            _burst_size(1000),
            _msg_magic(0x0) {}

        Config &max_msg_size(size_t x) {
            _max_msg_size = x;
            return *this;
        }

        Config &max_msg_queue_size(size_t x) {
            _max_msg_queue_size = x;
            return *this;
        }

        Config &burst_size(size_t x) {
            _burst_size = x;
            return *this;
        }

        Config &msg_magic(uint32_t x) {
            _msg_magic = x;
        }
    };

    virtual ~MsgNetwork() { stop(); }

    MsgNetwork(const EventContext &ec, const Config &config):
            ConnPool(ec, config),
            max_msg_size(config._max_msg_size),
            max_msg_queue_size(config._max_msg_queue_size),
            msg_magic(config._msg_magic) {
        incoming_msgs.set_capacity(max_msg_queue_size);
        incoming_msgs.reg_handler(ec, [this, burst_size=config._burst_size](queue_t &q) {
            std::pair<Msg, conn_t> item;
            size_t cnt = 0;
            while (q.try_dequeue(item) && this->system_state == 1)
            {
                auto &msg = item.first;
                auto &conn = item.second;
                auto it = handler_map.find(msg.get_opcode());
                if (it == handler_map.end())
                    SALTICIDAE_LOG_WARN("unknown opcode: %s",
                                        get_hex(msg.get_opcode()).c_str());
                else /* call the handler */
                {
                    SALTICIDAE_LOG_DEBUG("got message %s from %s",
                            std::string(msg).c_str(),
                            std::string(*conn).c_str());
#ifdef SALTICIDAE_MSG_STAT
                    conn->nrecv++;
                    conn->nrecvb += msg.get_length();
#endif
                    it->second(msg, conn);
                }
                if (++cnt == burst_size) return true;
            }
            return false;
        });
    }

    template<typename Func>
    typename std::enable_if<std::is_constructible<
        typename callback_traits<
            typename std::remove_reference<Func>::type>::msg_type,
        DataStream &&>::value>::type
    reg_handler(Func &&handler) {
        using callback_t = callback_traits<typename std::remove_reference<Func>::type>;
        set_handler(callback_t::msg_type::opcode,
            [handler=std::forward<Func>(handler)](const Msg &msg, const conn_t &conn) {
            handler(typename callback_t::msg_type(msg.get_payload()),
                    static_pointer_cast<typename callback_t::conn_type>(conn));
        });
    }

    template<typename Func>
    inline void set_handler(OpcodeType opcode, Func &&handler) {
        handler_map[opcode] = std::forward<Func>(handler);
    }

    template<typename MsgType>
    inline bool send_msg(const MsgType &msg, const conn_t &conn);
    inline bool _send_msg(const Msg &msg, const conn_t &conn);
    template<typename MsgType>
    inline int32_t send_msg_deferred(MsgType &&msg, const conn_t &conn);
    inline int32_t _send_msg_deferred(Msg &&msg, const conn_t &conn);

    void stop() { stop_workers(); }
    using ConnPool::listen;
    conn_t connect_sync(const NetAddr &addr) {
        return static_pointer_cast<Conn>(ConnPool::connect_sync(addr));
    }
};

/** Simple network that handles client-server requests. */
template<typename OpcodeType>
class ClientNetwork: public MsgNetwork<OpcodeType> {
    public:
    using MsgNet = MsgNetwork<OpcodeType>;
    using Msg = typename MsgNet::Msg;

    private:
    std::unordered_map<NetAddr, typename MsgNet::conn_t> addr2conn;

    public:
    class Conn: public MsgNet::Conn {
        friend ClientNetwork;
        public:
        Conn() = default;
        ClientNetwork *get_net() {
            return static_cast<ClientNetwork *>(ConnPool::Conn::get_pool());
        }
    };

    using conn_t = ArcObj<Conn>;

    protected:
    ConnPool::Conn *create_conn() override { return new Conn(); }
    void on_setup(const ConnPool::conn_t &) override;
    void on_teardown(const ConnPool::conn_t &) override;

    public:
    using Config = typename MsgNet::Config;
    ClientNetwork(const EventContext &ec, const Config &config):
        MsgNet(ec, config) {}

    using MsgNet::send_msg;
    template<typename MsgType>
    inline bool send_msg(const MsgType &msg, const NetAddr &addr);
    inline bool _send_msg(const Msg &msg, const NetAddr &addr);
    template<typename MsgType>
    inline int32_t send_msg_deferred(MsgType &&msg, const NetAddr &addr);
    inline int32_t _send_msg_deferred(Msg &&msg, const NetAddr &addr);
};

struct PeerId: public uint256_t {
    using uint256_t::uint256_t;
    PeerId(const NetAddr &addr) {
        *(static_cast<uint256_t *>(this)) = salticidae::get_hash(addr);
    }

    PeerId(const X509 &cert) {
        *(static_cast<uint256_t *>(this)) = salticidae::get_hash(cert.get_der());
    }
};

}

namespace std {
    template <>
    struct hash<salticidae::PeerId> {
        size_t operator()(const salticidae::PeerId &p) const {
            return hash<salticidae::uint256_t>()(p);
        }
    };

    template <>
    struct hash<const salticidae::PeerId> {
        size_t operator()(const salticidae::PeerId &p) const {
            return hash<salticidae::uint256_t>()(p);
        }
    };
}

namespace salticidae {

/** Peer-to-peer network where any two nodes could hold a bi-diretional message
 * channel, established by either side. */
template<typename OpcodeType = uint8_t,
        OpcodeType OPCODE_PING = 0xf0,
        OpcodeType OPCODE_PONG = 0xf1>
class PeerNetwork: public MsgNetwork<OpcodeType> {
    public:
    using MsgNet = MsgNetwork<OpcodeType>;
    using Msg = typename MsgNet::Msg;

    enum IdentityMode {
        ADDR_BASED,
        CERT_BASED
    };

    private:
    struct Peer;

    public:
    class Conn: public MsgNet::Conn {
        friend PeerNetwork;
        Peer *peer;
        TimerEvent ev_timeout;

        void reset_timeout(double timeout);

        public:
        Conn(): MsgNet::Conn(), peer(nullptr) {}
        NetAddr get_peer_addr() {
            auto ret = *(static_cast<NetAddr *>(
                get_net()->disp_tcall->call([this](ThreadCall::Handle &h) {
                    h.set_result(peer ? NetAddr(peer->addr) : NetAddr());
                }).get()));
            return ret;
        }

        PeerNetwork *get_net() {
            return static_cast<PeerNetwork *>(ConnPool::Conn::get_pool());
        }

        protected:
        void stop() override {
            ev_timeout.clear();
            MsgNet::Conn::stop();
        }
    };

    using conn_t = ArcObj<Conn>;
    using peer_callback_t = std::function<void(const conn_t &peer_conn, bool connected)>;
    using unknown_peer_callback_t = std::function<void(const NetAddr &claimed_addr, const X509 *cert)>;

    private:

    struct Peer {
        PeerId id;
        NetAddr addr; /** remote address (if set) */
        uint32_t nonce;
        conn_t conn;

        double retry_delay;
        ssize_t ntry;
        TimerEvent ev_retry_timer;

        /** the underlying connection, may be invalid when connected = false */
        conn_t chosen_conn;
        conn_t inbound_conn;
        conn_t outbound_conn;

        TimerEvent ev_ping_timer;
        bool ping_timer_ok;
        bool pong_msg_ok;
        double ping_period;

        enum State {
            DISCONNECTED,
            CONNECTED,
            RESET
        } state;

        Peer(const PeerId &pid, const PeerNetwork *pn):
            id(pid),
            retry_delay(0), ntry(0),
            ev_ping_timer(
                TimerEvent(pn->disp_ec, std::bind(&Peer::ping_timer, this, _1))),
            ping_period(pn->ping_period),
            state(DISCONNECTED) {}

        Peer &operator=(const Peer &) = delete;
        Peer(const Peer &) = delete;

        void reset_ping_timer();
        void send_ping();
        void ping_timer(TimerEvent &);
        void clear_all_events() {
            if (ev_ping_timer)
                ev_ping_timer.del();
        }
        uint32_t get_nonce() {
            if (nonce == 0)
            {
                uint16_t n;
                if (!RAND_bytes((uint8_t *)&n, 2))
                    throw PeerNetworkError(SALTI_ERROR_RAND_SOURCE);
                nonce = n + 1;
            }
            return nonce;
        }

        public:
        ~Peer() {
            if (inbound_conn) inbound_conn->peer = nullptr;
            if (outbound_conn) outbound_conn->peer = nullptr;
        }
    };

    /* connections whose PeerId is unknown */
    std::unordered_map<NetAddr, conn_t> pending_peers;
    /* registered peers */
    std::unordered_map<PeerId, BoxObj<Peer>> known_peers;

    using pinfo_slock_t = std::shared_lock<std::shared_timed_mutex>;
    using pinfo_ulock_t = std::unique_lock<std::shared_timed_mutex>;

    mutable std::shared_timed_mutex known_peers_lock;

    peer_callback_t peer_cb;
    unknown_peer_callback_t unknown_peer_cb;

    const IdentityMode id_mode;
    double ping_period;
    double conn_timeout;
    NetAddr listen_addr;
    bool allow_unknown_peer;
    uint32_t passive_nonce;

    struct MsgPing {
        static const OpcodeType opcode;
        DataStream serialized;
        NetAddr claimed_addr;
        uint32_t nonce;
        MsgPing() { serialized << (uint8_t)false; }
        MsgPing(const NetAddr &_claimed_addr, uint32_t _nonce) {
            serialized << (uint8_t)true << _claimed_addr << htole(_nonce);
        }
        MsgPing(DataStream &&s) {
            uint8_t flag;
            s >> flag;
            if (flag)
                s >> claimed_addr >> nonce;
            nonce = letoh(nonce);
        }
    };

    struct MsgPong: public MsgPing {
        static const OpcodeType opcode;
        MsgPong(): MsgPing() {}
        MsgPong(const NetAddr &_claimed_addr, uint32_t _nonce):
            MsgPing(_claimed_addr, _nonce) {}
        MsgPong(DataStream &&s): MsgPing(std::move(s)) {}
    };

    void ping_handler(MsgPing &&msg, const conn_t &conn);
    void pong_handler(MsgPong &&msg, const conn_t &conn);
    void _ping_msg_cb(const conn_t &conn, uint16_t port);
    void _pong_msg_cb(const conn_t &conn, uint16_t port);
    void finish_handshake(Peer *peer);
    void replace_pending_conn(const conn_t &conn);
    void start_active_conn(Peer *peer);
    static void tcall_reset_timeout(ConnPool::Worker *worker,
                                    const conn_t &conn, double timeout);
    inline conn_t _get_peer_conn(const PeerId &peer) const;

    protected:
    ConnPool::Conn *create_conn() override { return new Conn(); }
    void on_setup(const ConnPool::conn_t &) override;
    void on_teardown(const ConnPool::conn_t &) override;

    PeerId get_peer_id(const conn_t &conn, const NetAddr &addr) {
        DataStream tmp;
        if (!this->enable_tls || id_mode == ADDR_BASED)
            return PeerId(addr);
        else
            return PeerId(*conn->get_peer_cert());
    }

    public:
    class Config: public MsgNet::Config {
        friend PeerNetwork;
        double _ping_period;
        double _conn_timeout;
        bool _allow_unknown_peer;
        IdentityMode _id_mode;

        public:
        Config(): Config(typename MsgNet::Config()) {}

        Config(const typename MsgNet::Config &config):
            MsgNet::Config(config),
            _ping_period(30),
            _conn_timeout(180),
            _allow_unknown_peer(false),
            _id_mode(CERT_BASED) {}


        Config &ping_period(double x) {
            _ping_period = x;
            return *this;
        }

        Config &conn_timeout(double x) {
            _conn_timeout = x;
            return *this;
        }

        Config &id_mode(IdentityMode x) {
            _id_mode = x;
            return *this;
        }

        Config &allow_unknown_peer(bool x) {
            _allow_unknown_peer = x;
            return *this;
        }
    };

    PeerNetwork(const EventContext &ec, const Config &config):
            MsgNet(ec, config),
            id_mode(config._id_mode),
            ping_period(config._ping_period),
            conn_timeout(config._conn_timeout),
            allow_unknown_peer(config._allow_unknown_peer) {
        passive_nonce = 0xffff;
        this->reg_handler(generic_bind(&PeerNetwork::ping_handler, this, _1, _2));
        this->reg_handler(generic_bind(&PeerNetwork::pong_handler, this, _1, _2));
    }

    virtual ~PeerNetwork() { this->stop(); }

    /* register a peer as known */
    int32_t add_peer(const PeerId &peer);
    /* unregister the peer */
    int32_t del_peer(const PeerId &peer);
    /* set the peer's public IP */
    int32_t set_peer_addr(const PeerId &peer, const NetAddr &addr);
    /* try to connect to the peer: once (ntry = 1), indefinitely (ntry = -1), give up retry (ntry = 0) */
    int32_t conn_peer(const PeerId &peer, ssize_t ntry = -1, double retry_delay = 2);
    /* check if a peer is registered */
    bool has_peer(const PeerId &peer) const;

    size_t get_npending() const;
    conn_t get_peer_conn(const PeerId &addr) const;
    using MsgNet::send_msg;
    template<typename MsgType>
    inline bool send_msg(const MsgType &msg, const PeerId &peer);
    inline bool _send_msg(const Msg &msg, const PeerId &peer);
    template<typename MsgType>
    inline int32_t send_msg_deferred(MsgType &&msg, const PeerId &peer);
    inline int32_t _send_msg_deferred(Msg &&msg, const PeerId &peer);
    template<typename MsgType>
    inline int32_t multicast_msg(MsgType &&msg, const std::vector<PeerId> &peers);
    inline int32_t _multicast_msg(Msg &&msg, const std::vector<PeerId> &peers);

    void listen(NetAddr listen_addr);
    conn_t connect(const NetAddr &addr) = delete;
    template<typename Func>
    void reg_unknown_peer_handler(Func &&cb) { unknown_peer_cb = std::forward<Func>(cb); }
    template<typename Func>
    void reg_peer_handler(Func &&cb) { peer_cb = std::forward<Func>(cb); }
};

/* this callback is run by a worker */
template<typename OpcodeType>
void MsgNetwork<OpcodeType>::on_read(const ConnPool::conn_t &_conn) {
    auto conn = static_pointer_cast<Conn>(_conn);
    if (conn->msg_sleep) return;
    ConnPool::on_read(_conn);
    auto &recv_buffer = conn->recv_buffer;
    auto &msg = conn->msg;
    auto &msg_state = conn->msg_state;
    while (true)
    {
        if (msg_state == Conn::HEADER)
        {
            if (recv_buffer.size() < Msg::header_size) break;
            /* new header available */
            msg = Msg(recv_buffer.pop(Msg::header_size));
            if (msg.get_length() > max_msg_size)
            {
                SALTICIDAE_LOG_WARN(
                        "oversized message from %s, terminating the connection",
                        std::string(*conn).c_str());
                throw MsgNetworkError(SALTI_ERROR_CONN_OVERSIZED_MSG);
            }
            msg_state = Conn::PAYLOAD;
        }
        if (msg_state == Conn::PAYLOAD)
        {
            size_t len = msg.get_length();
            if (recv_buffer.size() < len) break;
            /* new payload available */
            msg.set_payload(recv_buffer.pop(len));
            msg_state = Conn::HEADER;
#ifndef SALTICIDAE_NOCHECKSUM
            if (!msg.verify_checksum())
            {
                SALTICIDAE_LOG_WARN("checksums do not match, dropping the message");
                break;
            }
#endif
            if (!incoming_msgs.enqueue(std::make_pair(msg, conn), false))
            {
                conn->msg_sleep = true;
                conn->ev_enqueue_poll.add(0);
                return;
            }
        }
    }
    if (conn->ready_recv && recv_buffer.len() < conn->max_recv_buff_size)
    {
        /* resume reading from socket */
        conn->ev_socket.del();
        conn->ev_socket.add(FdEvent::READ |
                            (conn->ready_send ? 0: FdEvent::WRITE));
        conn->recv_data_func(conn, conn->fd, FdEvent::READ);
    }
}

template<typename OpcodeType>
template<typename MsgType>
inline int32_t MsgNetwork<OpcodeType>::send_msg_deferred(MsgType &&msg, const conn_t &conn) {
    return _send_msg_deferred(Msg(std::move(msg), msg_magic), conn);
}

template<typename OpcodeType>
inline int32_t MsgNetwork<OpcodeType>::_send_msg_deferred(Msg &&msg, const conn_t &conn) {
    auto id = this->gen_async_id();
    this->disp_tcall->async_call(
            [this, msg=std::move(msg), conn, id](ThreadCall::Handle &) {
        try {
            if (!_send_msg(msg, conn))
                throw SalticidaeError(SALTI_ERROR_CONN_NOT_READY);
        } catch (...) { this->recoverable_error(std::current_exception(), id); }
    });
    return id;
}

template<typename OpcodeType>
template<typename MsgType>
inline bool MsgNetwork<OpcodeType>::send_msg(const MsgType &msg, const conn_t &conn) {
    return _send_msg(Msg(msg, msg_magic), conn);
}

template<typename OpcodeType>
inline bool MsgNetwork<OpcodeType>::_send_msg(const Msg &msg, const conn_t &conn) {
    bytearray_t msg_data = msg.serialize();
    SALTICIDAE_LOG_DEBUG("wrote message %s to %s",
                std::string(msg).c_str(),
                std::string(*conn).c_str());
#ifdef SALTICIDAE_MSG_STAT
    conn->nsent++;
    conn->nsentb += msg.get_length();
#endif
    return conn->write(std::move(msg_data));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::tcall_reset_timeout(ConnPool::Worker *worker,
                                    const conn_t &conn, double timeout) {
    worker->get_tcall()->async_call([worker, conn, t=timeout](ThreadCall::Handle &) {
        try {
            if (!conn->ev_timeout) return;
            conn->ev_timeout.del();
            conn->ev_timeout.add(t);
            SALTICIDAE_LOG_DEBUG("reset connection timeout %.2f", t);
        } catch (...) { worker->error_callback(std::current_exception()); }
    });
}

/* begin: functions invoked by the dispatcher */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::on_setup(const ConnPool::conn_t &_conn) {
    MsgNet::on_setup(_conn);
    auto conn = static_pointer_cast<Conn>(_conn);
    auto worker = conn->worker;
    SALTICIDAE_LOG_INFO("connection: %s", std::string(*conn).c_str());
    worker->get_tcall()->async_call([this, conn, worker](ThreadCall::Handle &) {
        auto &ev_timeout = conn->ev_timeout;
        assert(!ev_timeout);
        ev_timeout = TimerEvent(worker->get_ec(), [=](TimerEvent &) {
            try {
                SALTICIDAE_LOG_INFO("peer ping-pong timeout");
                this->worker_terminate(conn);
            } catch (...) { worker->error_callback(std::current_exception()); }
        });
    });
    /* the initial ping-pong to set up the connection */
    tcall_reset_timeout(worker, conn, conn_timeout);
    replace_pending_conn(conn);
    if (conn->get_mode() == Conn::ConnMode::ACTIVE)
    {
        auto pid = get_peer_id(conn, conn->get_addr());
        pinfo_slock_t _g(known_peers_lock);
        send_msg(MsgPing(
            listen_addr,
            known_peers.find(pid)->second->get_nonce()), conn);
    }
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::on_teardown(const ConnPool::conn_t &_conn) {
    MsgNet::on_teardown(_conn);
    auto conn = static_pointer_cast<Conn>(_conn);
    auto addr = conn->get_addr();
    pending_peers.erase(addr);
    SALTICIDAE_LOG_INFO("connection lost: %s", std::string(*conn).c_str());
    auto p = conn->peer;
    if (!p) return;
    /* if this connect was the active peer connection */
    bool reset = p->state == Peer::State::RESET;
    if (p->conn == conn)
    {
        p->state = Peer::State::DISCONNECTED;
        p->inbound_conn = nullptr;
        p->outbound_conn = nullptr;
        p->ev_ping_timer.del();
        p->nonce = 0;
        this->user_tcall->async_call([this, conn](ThreadCall::Handle &) {
            if (peer_cb) peer_cb(conn, false);
        });
    }
    /* auto retry the connection */
    if (p->ntry > 0) p->ntry--;
    if (p->ntry)
    {
        p->ev_retry_timer = TimerEvent(this->disp_ec, [this, addr=p->addr, p](TimerEvent &) {
            try {
                start_active_conn(p);
                p->ev_retry_timer.add(gen_rand_timeout(p->retry_delay));
            } catch (...) { this->disp_error_cb(std::current_exception()); }
        });
        p->ev_retry_timer.add(reset ? 0 : gen_rand_timeout(p->retry_delay));
    }
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::reset_ping_timer() {
    assert(ev_ping_timer);
    ev_ping_timer.del();
    ev_ping_timer.add(gen_rand_timeout(ping_period));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::send_ping() {
    auto pn = chosen_conn->get_net();
    ping_timer_ok = false;
    pong_msg_ok = false;
    tcall_reset_timeout(chosen_conn->worker, chosen_conn, pn->conn_timeout);
    pn->send_msg(MsgPing(), chosen_conn);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::ping_timer(TimerEvent &) {
    ping_timer_ok = true;
    if (pong_msg_ok)
    {
        reset_ping_timer();
        send_ping();
    }
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::finish_handshake(Peer *p) {
    assert(p->state == Peer::State::DISCONNECTED);
    p->clear_all_events();
    if (p->inbound_conn && p->inbound_conn != p->chosen_conn)
        p->inbound_conn->peer = nullptr;
    if (p->outbound_conn && p->outbound_conn != p->chosen_conn)
        p->outbound_conn->peer = nullptr;
    p->state = Peer::State::CONNECTED;
    p->reset_ping_timer();
    p->send_ping();
    p->ev_retry_timer.del();
    auto &old_conn = p->conn;
    auto &new_conn = p->chosen_conn;
    if (old_conn)
    {
        /* there is some previously terminated connection */
        assert(p->conn->is_terminated());
        for (;;)
        {
            bytearray_t buff_seg = old_conn->send_buffer.move_pop();
            if (!buff_seg.size()) break;
            new_conn->write(std::move(buff_seg));
        }
        old_conn->peer = nullptr;
    }
    old_conn = new_conn;
    new_conn->peer = p;
    this->user_tcall->async_call([this, conn=p->conn](ThreadCall::Handle &) {
        if (peer_cb) peer_cb(conn, true);
    });
    pending_peers.erase(p->conn->get_addr());
    auto color_begin = "";
    auto color_end = "";
    if (logger.is_tty())
    {
        color_begin = TTY_COLOR_BLUE;
        color_end = TTY_COLOR_RESET;
    }
    SALTICIDAE_LOG_INFO("%sPeerNetwork: established connection %s <-> %s(%s)",
        color_begin,
        std::string(listen_addr).c_str(),
        get_hex10(p->id).c_str(),
        std::string(*(p->conn)).c_str(),
        color_end);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::replace_pending_conn(const conn_t &conn) {
    const auto &addr = conn->get_addr();
    auto it = pending_peers.find(addr);
    if (it != pending_peers.end())
    {
        auto &old_conn = it->second;
        if (old_conn != conn)
        {
            this->disp_terminate(old_conn);
            pending_peers.erase(it);
        }
    }
    pending_peers.insert(std::make_pair(addr, conn));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::start_active_conn(Peer *p) {
    assert(!p->addr.is_null());
    auto conn = static_pointer_cast<Conn>(MsgNet::_connect(p->addr));
    p->outbound_conn = conn;
    replace_pending_conn(conn);
}

template<typename O, O _, O __>
inline typename PeerNetwork<O, _, __>::conn_t PeerNetwork<O, _, __>::_get_peer_conn(const PeerId &pid) const {
    auto it = known_peers.find(pid);
    if (it == known_peers.end())
        throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
    return it->second->conn;
}
/* end: functions invoked by the dispatcher */

/* begin: functions invoked by the user loop */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::ping_handler(MsgPing &&msg, const conn_t &conn) {
    this->disp_tcall->async_call([this, conn, msg=std::move(msg)](ThreadCall::Handle &) {
        try {
            if (conn->is_terminated()) return;
            if (!msg.claimed_addr.is_null()) /* handshake ping */
            {
                if (conn->get_mode() == Conn::ConnMode::PASSIVE)
                {
                    auto pid = get_peer_id(conn, msg.claimed_addr);
                    pinfo_slock_t _g(known_peers_lock);
                    auto pit = known_peers.find(pid);
                    if (pit == known_peers.end())
                    {
                        this->user_tcall->async_call([this, addr=msg.claimed_addr, conn](ThreadCall::Handle &) {
                            if (unknown_peer_cb) unknown_peer_cb(addr, conn->get_peer_cert());
                        });
                        this->disp_terminate(conn);
                        return;
                    }
                    auto &p = pit->second;
                    if (p->state != Peer::State::DISCONNECTED ||
                        (!p->addr.is_null() && p->addr != msg.claimed_addr)) return;
                    SALTICIDAE_LOG_INFO("%s inbound handshake from %s",
                        std::string(listen_addr).c_str(),
                        std::string(*conn).c_str());
                    send_msg(MsgPong(
                        listen_addr,
                        p->addr.is_null() ? passive_nonce : p->get_nonce()), conn);
                    auto &old_conn = p->inbound_conn;
                    if (old_conn && old_conn != conn)
                    {
                        SALTICIDAE_LOG_DEBUG("%s terminating stale handshake connection %s",
                                std::string(listen_addr).c_str(),
                                std::string(*old_conn).c_str());
                        assert(old_conn->peer == nullptr);
                        this->disp_terminate(old_conn);
                    }
                    old_conn = conn;
                    if (msg.nonce < p->get_nonce() || p->addr.is_null())
                    {
                        SALTICIDAE_LOG_DEBUG("connection %s chosen", std::string(*conn).c_str());
                        p->chosen_conn = conn;
                        finish_handshake(p.get());
                    }
                    else
                    {
                        SALTICIDAE_LOG_DEBUG("%04x >= %04x, terminating", msg.nonce, p->get_nonce());
                        this->disp_terminate(conn);
                    }
                }
                else
                    SALTICIDAE_LOG_WARN("unexpected inbound handshake from %s",
                        std::string(*conn).c_str());
            }
            else /* heartbeat ping */
            {
                SALTICIDAE_LOG_INFO("ping from %s", std::string(*conn).c_str());
                send_msg(MsgPong(), conn);
            }
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::pong_handler(MsgPong &&msg, const conn_t &conn) {
    this->disp_tcall->async_call([this, conn, msg=std::move(msg)](ThreadCall::Handle &) {
        try {
            if (conn->is_terminated()) return;
            if (!msg.claimed_addr.is_null()) /* handshake pong */
            {
                if (conn->get_mode() == Conn::ConnMode::ACTIVE)
                {
                    auto pid = get_peer_id(conn, conn->get_addr());
                    pinfo_slock_t _g(known_peers_lock);
                    auto pit = known_peers.find(pid);
                    if (pit == known_peers.end())
                    {
                        SALTICIDAE_LOG_WARN("unexpected pong from an unknown peer");
                        this->disp_terminate(conn);
                        return;
                    }
                    auto &p = pit->second;
                    assert(!p->addr.is_null() && p->addr == conn->get_addr());
                    if (p->state != Peer::State::DISCONNECTED ||
                        p->addr != msg.claimed_addr) return;
                    SALTICIDAE_LOG_INFO("%s outbound handshake to %s",
                        std::string(listen_addr).c_str(),
                        std::string(*conn).c_str());
                    auto &old_conn = p->outbound_conn;
                    if (old_conn && old_conn != conn)
                    {
                        SALTICIDAE_LOG_DEBUG("%s terminating stale handshake connection %s",
                                std::string(listen_addr).c_str(),
                                std::string(*old_conn).c_str());
                        assert(old_conn->peer == nullptr);
                        old_conn->get_net()->disp_terminate(old_conn);
                    }
                    old_conn = conn;
                    if (p->get_nonce() < msg.nonce)
                    {
                        SALTICIDAE_LOG_DEBUG("connection %s chosen", std::string(*conn).c_str());
                        p->chosen_conn = conn;
                        p->reset_ping_timer();
                        finish_handshake(p.get());
                    }
                    else
                    {
                        SALTICIDAE_LOG_DEBUG(
                                "%04x >= %04x, terminating and resetting",
                                p->get_nonce(), msg.nonce);
                        p->nonce = 0;
                        this->disp_terminate(conn);
                    }
                }
                else
                    SALTICIDAE_LOG_WARN("unexpected outbound handshake from %s",
                        std::string(*conn).c_str());
            }
            else /* heartbeat pong */
            {
                auto p = conn->peer;
                if (!p)
                {
                    SALTICIDAE_LOG_WARN("unexpected pong mesage");
                    return;
                }
                p->pong_msg_ok = true;
                if (p->ping_timer_ok)
                {
                    p->reset_ping_timer();
                    p->send_ping();
                }
            }
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::listen(NetAddr _listen_addr) {
    this->disp_tcall->call([this, _listen_addr](ThreadCall::Handle &) {
        MsgNet::_listen(_listen_addr);
        listen_addr = _listen_addr;
    }).get();
}

template<typename O, O _, O __>
int32_t PeerNetwork<O, _, __>::add_peer(const PeerId &pid) {
    auto id = this->gen_async_id();
    this->disp_tcall->async_call([this, pid, id](ThreadCall::Handle &) {
        try {
            pinfo_ulock_t _g(known_peers_lock);
            if (known_peers.count(pid))
                throw PeerNetworkError(SALTI_ERROR_PEER_ALREADY_EXISTS);
            known_peers.insert(std::make_pair(pid, new Peer(pid, this)));
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception(), id);
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
    return id;
}

template<typename O, O _, O __>
int32_t PeerNetwork<O, _, __>::conn_peer(const PeerId &pid, ssize_t ntry, double retry_delay) {
    auto id = this->gen_async_id();
    this->disp_tcall->async_call([this, pid, ntry, retry_delay, id](ThreadCall::Handle &) {
        try {
            pinfo_slock_t _g(known_peers_lock);
            auto it = known_peers.find(pid);
            if (it == known_peers.end())
                throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
            auto &p = it->second;
            if (p->addr.is_null())
                throw PeerNetworkError(SALTI_ERROR_PEER_NOT_READY);
            p->ntry = ntry;
            p->retry_delay = retry_delay;
            p->inbound_conn = nullptr;
            p->outbound_conn = nullptr;
            p->ev_ping_timer.del();
            p->nonce = 0;
            /* has to terminate established connection *before* making the next
             * attempt */
            if (!p->conn || p->state == Peer::State::DISCONNECTED)
                start_active_conn(p.get());
            else if (p->state == Peer::State::CONNECTED)
            {
                p->state = Peer::State::RESET;
                this->disp_terminate(p->conn);
            }
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception(), id);
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
    return id;
}

template<typename O, O _, O __>
int32_t PeerNetwork<O, _, __>::set_peer_addr(const PeerId &pid, const NetAddr &addr) {
    auto id = this->gen_async_id();
    this->disp_tcall->async_call([this, pid, addr, id](ThreadCall::Handle &) {
        try {
            pinfo_slock_t _g(known_peers_lock);
            auto it = known_peers.find(pid);
            if (it == known_peers.end())
                throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
            it->second->addr = addr;
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception(), id);
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
    return id;
}


template<typename O, O _, O __>
int32_t PeerNetwork<O, _, __>::del_peer(const PeerId &pid) {
    auto id = this->gen_async_id();
    this->disp_tcall->async_call([this, pid, id](ThreadCall::Handle &) {
        try {
            pinfo_ulock_t _g(known_peers_lock);
            auto it = known_peers.find(pid);
            if (it == known_peers.end())
                throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
            auto addr = it->second->addr;
            this->disp_terminate(it->second->conn);
            known_peers.erase(it);
            auto it2 = pending_peers.find(addr);
            if (it2 != pending_peers.end())
            {
                auto &conn = it2->second;
                if (!conn->peer)
                    this->disp_terminate(conn);
                pending_peers.erase(it2);
            }
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception(), id);
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
    return id;
}

template<typename O, O _, O __>
typename PeerNetwork<O, _, __>::conn_t
PeerNetwork<O, _, __>::get_peer_conn(const PeerId &pid) const {
    auto ret = *(static_cast<conn_t *>(
            this->disp_tcall->call([this, pid](ThreadCall::Handle &h) {
        conn_t conn;
        pinfo_slock_t _g(known_peers_lock);
        auto it = known_peers.find(pid);
        if (it == known_peers.end())
            throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
        h.set_result(it->second->conn);
    }).get()));
    return ret;
}

template<typename O, O _, O __>
bool PeerNetwork<O, _, __>::has_peer(const PeerId &pid) const {
    return *(static_cast<bool *>(this->disp_tcall->call(
                [this, pid](ThreadCall::Handle &h) {
        pinfo_slock_t _g(known_peers_lock);
        h.set_result(known_peers.count(pid));
    }).get()));
}

template<typename O, O _, O __>
size_t PeerNetwork<O, _, __>::get_npending() const {
    return *(static_cast<bool *>(this->disp_tcall->call(
                [this](ThreadCall::Handle &h) {
        h.set_result(pending_peers.size());
    }).get()));
}

template<typename O, O _, O __>
template<typename MsgType>
inline int32_t PeerNetwork<O, _, __>::send_msg_deferred(MsgType &&msg, const PeerId &pid) {
    return _send_msg_deferred(Msg(std::move(msg), this->msg_magic), pid);
}

template<typename O, O _, O __>
inline int32_t PeerNetwork<O, _, __>::_send_msg_deferred(Msg &&msg, const PeerId &pid) {
    auto id = this->gen_async_id();
    this->disp_tcall->async_call(
            [this, msg=std::move(msg), pid, id](ThreadCall::Handle &) {
        try {
            if (!_send_msg(msg, pid))
                throw PeerNetworkError(SALTI_ERROR_CONN_NOT_READY);
        } catch (...) { this->recoverable_error(std::current_exception(), id); }
    });
    return id;
}

template<typename O, O _, O __>
template<typename MsgType>
inline bool PeerNetwork<O, _, __>::send_msg(const MsgType &msg, const PeerId &pid) {
    return _send_msg(Msg(msg, this->msg_magic), pid);
}

template<typename O, O _, O __>
inline bool PeerNetwork<O, _, __>::_send_msg(const Msg &msg, const PeerId &pid) {
    pinfo_slock_t _g(known_peers_lock);
    return MsgNet::_send_msg(msg, _get_peer_conn(pid));
}

template<typename O, O _, O __>
template<typename MsgType>
inline int32_t PeerNetwork<O, _, __>::multicast_msg(MsgType &&msg, const std::vector<PeerId> &pids) {
    return _multicast_msg(Msg(std::move(msg), this->msg_magic), pids);
}

template<typename O, O _, O __>
inline int32_t PeerNetwork<O, _, __>::_multicast_msg(Msg &&msg, const std::vector<PeerId> &pids) {
    auto id = this->gen_async_id();
    this->disp_tcall->async_call(
                [this, msg=std::move(msg), pids, id](ThreadCall::Handle &) {
        try {
            pinfo_slock_t _g(known_peers_lock);
            bool succ = true;
            for (auto &pid: pids)
                succ &= MsgNet::_send_msg(msg, _get_peer_conn(pid));
            if (!succ) throw PeerNetworkError(SALTI_ERROR_CONN_NOT_READY);
        } catch (...) { this->recoverable_error(std::current_exception(), id); }
    });
    return id;
}

/* end: functions invoked by the user loop */

template<typename OpcodeType>
void ClientNetwork<OpcodeType>::on_setup(const ConnPool::conn_t &_conn) {
    MsgNet::on_setup(_conn);
    auto conn = static_pointer_cast<Conn>(_conn);
    assert(conn->get_mode() == Conn::PASSIVE);
    const auto &addr = conn->get_addr();
    auto cn = conn->get_net();
    cn->addr2conn.erase(addr);
    cn->addr2conn.insert(std::make_pair(addr, conn));
}

template<typename OpcodeType>
void ClientNetwork<OpcodeType>::on_teardown(const ConnPool::conn_t &_conn) {
    MsgNet::on_teardown(_conn);
    auto conn = static_pointer_cast<Conn>(_conn);
    conn->get_net()->addr2conn.erase(conn->get_addr());
}

template<typename OpcodeType>
template<typename MsgType>
inline int32_t ClientNetwork<OpcodeType>::send_msg_deferred(MsgType &&msg, const NetAddr &addr) {
    return _send_msg_deferred(Msg(std::move(msg), this->msg_magic), addr);
}

template<typename OpcodeType>
inline int32_t ClientNetwork<OpcodeType>::_send_msg_deferred(Msg &&msg, const NetAddr &addr) {
    auto id = this->gen_async_id();
    this->disp_tcall->async_call(
            [this, msg=std::move(msg), addr, id](ThreadCall::Handle &) {
        try {
            _send_msg(msg, addr);
        } catch (...) { this->recoverable_error(std::current_exception(), id); }
    });
    return id;
}

template<typename OpcodeType>
template<typename MsgType>
inline bool ClientNetwork<OpcodeType>::send_msg(const MsgType &msg, const NetAddr &addr) {
    return _send_msg(Msg(msg, this->msg_magic), addr);
}

template<typename OpcodeType>
inline bool ClientNetwork<OpcodeType>::_send_msg(const Msg &msg, const NetAddr &addr) {
    auto it = addr2conn.find(addr);
    if (it == addr2conn.end())
        throw ClientNetworkError(SALTI_ERROR_CLIENT_NOT_EXIST);
    return MsgNet::_send_msg(msg, it->second);
}

template<typename O, O OPCODE_PING, O _>
const O PeerNetwork<O, OPCODE_PING, _>::MsgPing::opcode = OPCODE_PING;

template<typename O, O _, O OPCODE_PONG>
const O PeerNetwork<O, _, OPCODE_PONG>::MsgPong::opcode = OPCODE_PONG;

}

#ifdef SALTICIDAE_CBINDINGS
using peerid_t = salticidae::PeerId;
using peerid_array_t = std::vector<peerid_t>;

using msgnetwork_t = salticidae::MsgNetwork<_opcode_t>;
using msgnetwork_config_t = msgnetwork_t::Config;
using msgnetwork_conn_t = msgnetwork_t::conn_t;

using peernetwork_t = salticidae::PeerNetwork<_opcode_t>;
using peernetwork_config_t = peernetwork_t::Config;
using peernetwork_conn_t = peernetwork_t::conn_t;

using clientnetwork_t = salticidae::ClientNetwork<_opcode_t>;
using clientnetwork_conn_t = clientnetwork_t::conn_t;
#endif

#else

#ifdef SALTICIDAE_CBINDINGS
typedef struct peerid_t peerid_t;
typedef struct peerid_t_array_t peerid_array_t;

typedef struct msgnetwork_t msgnetwork_t;
typedef struct msgnetwork_config_t msgnetwork_config_t;
typedef struct msgnetwork_conn_t msgnetwork_conn_t;

typedef struct peernetwork_t peernetwork_t;
typedef struct peernetwork_config_t peernetwork_config_t;
typedef struct peernetwork_conn_t peernetwork_conn_t;

typedef struct clientnetwork_t clientnetwork_t;
typedef struct clientnetwork_conn_t clientnetwork_conn_t;
#endif

#endif

#ifdef SALTICIDAE_CBINDINGS
typedef enum msgnetwork_conn_mode_t {
    CONN_MODE_ACTIVE,
    CONN_MODE_PASSIVE,
} msgnetwork_conn_mode_t;

typedef enum peernetwork_id_mode_t {
    ID_MODE_ADDR_BASED,
    ID_MODE_CERT_BASED
} peernetwork_id_mode_t;

#ifdef __cplusplus
extern "C" {
#endif

void salticidae_injected_msg_callback(const msg_t *msg, msgnetwork_conn_t *conn);

/* MsgNetwork */

msgnetwork_config_t *msgnetwork_config_new();
void msgnetwork_config_free(const msgnetwork_config_t *self);
void msgnetwork_config_max_msg_size(msgnetwork_config_t *self, size_t size);
void msgnetwork_config_max_msg_queue_size(msgnetwork_config_t *self, size_t size);
void msgnetwork_config_burst_size(msgnetwork_config_t *self, size_t burst_size);
void msgnetwork_config_max_listen_backlog(msgnetwork_config_t *self, int backlog);
void msgnetwork_config_conn_server_timeout(msgnetwork_config_t *self, double timeout);
void msgnetwork_config_recv_chunk_size(msgnetwork_config_t *self, size_t size);
void msgnetwork_config_nworker(msgnetwork_config_t *self, size_t nworker);
void msgnetwork_config_max_recv_buff_size(msgnetwork_config_t *self, size_t size);
void msgnetwork_config_max_send_buff_size(msgnetwork_config_t *self, size_t size);
void msgnetwork_config_enable_tls(msgnetwork_config_t *self, bool enabled);
void msgnetwork_config_tls_key_file(msgnetwork_config_t *self, const char *pem_fname);
void msgnetwork_config_tls_cert_file(msgnetwork_config_t *self, const char *pem_fname);
void msgnetwork_config_tls_key_by_move(msgnetwork_config_t *self, pkey_t *key);
void msgnetwork_config_tls_cert_by_move(msgnetwork_config_t *self, x509_t *cert);

msgnetwork_t *msgnetwork_new(const eventcontext_t *ec, const msgnetwork_config_t *config, SalticidaeCError *err);
void msgnetwork_free(const msgnetwork_t *self);
bool msgnetwork_send_msg(msgnetwork_t *self, const msg_t *msg, const msgnetwork_conn_t *conn);
int32_t msgnetwork_send_msg_deferred_by_move(msgnetwork_t *self, msg_t *_moved_msg, const msgnetwork_conn_t *conn);
msgnetwork_conn_t *msgnetwork_connect_sync(msgnetwork_t *self, const netaddr_t *addr, SalticidaeCError *err);
int32_t msgnetwork_connect(msgnetwork_t *self, const netaddr_t *addr);
msgnetwork_conn_t *msgnetwork_conn_copy(const msgnetwork_conn_t *self);
void msgnetwork_conn_free(const msgnetwork_conn_t *self);
void msgnetwork_listen(msgnetwork_t *self, const netaddr_t *listen_addr, SalticidaeCError *err);
void msgnetwork_start(msgnetwork_t *self);
void msgnetwork_stop(msgnetwork_t *self);
void msgnetwork_terminate(msgnetwork_t *self, const msgnetwork_conn_t *conn);

typedef void (*msgnetwork_msg_callback_t)(const msg_t *, const msgnetwork_conn_t *, void *userdata);
void msgnetwork_reg_handler(msgnetwork_t *self, _opcode_t opcode, msgnetwork_msg_callback_t cb, void *userdata);

typedef bool (*msgnetwork_conn_callback_t)(const msgnetwork_conn_t *, bool connected, void *userdata);
void msgnetwork_reg_conn_handler(msgnetwork_t *self, msgnetwork_conn_callback_t cb, void *userdata);


typedef void (*msgnetwork_error_callback_t)(const SalticidaeCError *, bool fatal, int32_t async_id, void *userdata);
void msgnetwork_reg_error_handler(msgnetwork_t *self, msgnetwork_error_callback_t cb, void *userdata);

msgnetwork_t *msgnetwork_conn_get_net(const msgnetwork_conn_t *conn);
msgnetwork_conn_mode_t msgnetwork_conn_get_mode(const msgnetwork_conn_t *conn);
const netaddr_t *msgnetwork_conn_get_addr(const msgnetwork_conn_t *conn);
const x509_t *msgnetwork_conn_get_peer_cert(const msgnetwork_conn_t *conn);
bool msgnetwork_conn_is_terminated(const msgnetwork_conn_t *conn);

/* PeerNetwork */

void peerid_free(const peerid_t *self);
peerid_t *peerid_new_from_netaddr(const netaddr_t *addr);
peerid_t *peerid_new_from_x509(const x509_t *cert);
peerid_array_t *peerid_array_new();
peerid_array_t *peerid_array_new_from_peerids(const peerid_t * const *pids, size_t npids);
void peerid_array_free(peerid_array_t *self);

peernetwork_config_t *peernetwork_config_new();
void peernetwork_config_free(const peernetwork_config_t *self);
void peernetwork_config_ping_period(peernetwork_config_t *self, double t);
void peernetwork_config_conn_timeout(peernetwork_config_t *self, double t);
void peernetwork_config_id_mode(peernetwork_config_t *self, peernetwork_id_mode_t mode);
msgnetwork_config_t *peernetwork_config_as_msgnetwork_config(peernetwork_config_t *self);

peernetwork_t *peernetwork_new(const eventcontext_t *ec, const peernetwork_config_t *config, SalticidaeCError *err);
void peernetwork_free(const peernetwork_t *self);
int32_t peernetwork_add_peer(peernetwork_t *self, const peerid_t *peer);
int32_t peernetwork_del_peer(peernetwork_t *self, const peerid_t *peer);
int32_t peernetwork_conn_peer(peernetwork_t *self, const peerid_t *peer, ssize_t ntry, double retry_delay);
bool peernetwork_has_peer(const peernetwork_t *self, const peerid_t *peer);
const peernetwork_conn_t *peernetwork_get_peer_conn(const peernetwork_t *self, const peerid_t *peer, SalticidaeCError *cerror);
int32_t peernetwork_set_peer_addr(peernetwork_t *self, const peerid_t *peer, const netaddr_t *addr);
msgnetwork_t *peernetwork_as_msgnetwork(peernetwork_t *self);
peernetwork_t *msgnetwork_as_peernetwork_unsafe(msgnetwork_t *self);
msgnetwork_conn_t *msgnetwork_conn_new_from_peernetwork_conn(const peernetwork_conn_t *conn);
peernetwork_conn_t *peernetwork_conn_new_from_msgnetwork_conn_unsafe(const msgnetwork_conn_t *conn);
peernetwork_conn_t *peernetwork_conn_copy(const peernetwork_conn_t *self);
netaddr_t *peernetwork_conn_get_peer_addr(const peernetwork_conn_t *self);
void peernetwork_conn_free(const peernetwork_conn_t *self);
bool peernetwork_send_msg(peernetwork_t *self, const msg_t * msg, const peerid_t *peer);
int32_t peernetwork_send_msg_deferred_by_move(peernetwork_t *self, msg_t * _moved_msg, const peerid_t *peer);
int32_t peernetwork_multicast_msg_by_move(peernetwork_t *self, msg_t *_moved_msg, const peerid_array_t *peers);
void peernetwork_listen(peernetwork_t *self, const netaddr_t *listen_addr, SalticidaeCError *err);

typedef void (*peernetwork_peer_callback_t)(const peernetwork_conn_t *, bool connected, void *userdata);
void peernetwork_reg_peer_handler(peernetwork_t *self, peernetwork_peer_callback_t cb, void *userdata);

typedef void (*peernetwork_unknown_peer_callback_t)(const netaddr_t *, const x509_t *, void *userdata);
void peernetwork_reg_unknown_peer_handler(peernetwork_t *self, peernetwork_unknown_peer_callback_t cb, void *userdata);

/* ClientNetwork */

clientnetwork_t *clientnetwork_new(const eventcontext_t *ec, const msgnetwork_config_t *config, SalticidaeCError *err);
void clientnetwork_free(const clientnetwork_t *self);
msgnetwork_t *clientnetwork_as_msgnetwork(clientnetwork_t *self);
clientnetwork_t *msgnetwork_as_clientnetwork_unsafe(msgnetwork_t *self);
msgnetwork_conn_t *msgnetwork_conn_new_from_clientnetwork_conn(const clientnetwork_conn_t *conn);
clientnetwork_conn_t *clientnetwork_conn_new_from_msgnetwork_conn_unsafe(const msgnetwork_conn_t *conn);
clientnetwork_conn_t *clientnetwork_conn_copy(const clientnetwork_conn_t *self);
void clientnetwork_conn_free(const clientnetwork_conn_t *self);
bool clientnetwork_send_msg(clientnetwork_t *self, const msg_t * msg, const netaddr_t *addr);
int32_t clientnetwork_send_msg_deferred_by_move(clientnetwork_t *self, msg_t * _moved_msg, const netaddr_t *addr);

#ifdef __cplusplus
}
#endif
#endif

#endif
