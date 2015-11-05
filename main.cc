#include <iostream>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include "main_opts.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <ev.h>

#include "threads.h"
#include "pool.h"
#include "util.h"

const std::string CRLF("\r\n");
const std::string GET("GET ");
const std::string QUERY_FAST("/test/fast");
const std::string QUERY_SLOW("/test/slow");
const std::string RESPONSE(
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Length: 0\r\n"
    "\r\n");

ThreadPool thread_pool;

class ReqParser
{
public:
    enum Status {
        // Terminate connection
        TERMINATE = 0,
        // Continue current phase
        CONTINUE,
        // Proceed to next phase
        PROCEED
    };
    
    enum Service {
        NOT_DEFINED = 0,
        FAST,
        SLOW
    };
    
private:
    typedef Status(ReqParser::*parse_f)();
    parse_f parse;
    char *full_buf;
    size_t &received_size;
    size_t crlf_scan = 0;
    char *crlf_prev = nullptr; // detection of CRLFCRLF sequence
    bool method_ok = false;
    
public:
    size_t requestline_size = 0;
    size_t uri_start = 0;
    size_t uri_size = 0;
    Service service = NOT_DEFINED;

public:
    char *
    scan_for(const std::string& pattern, size_t &scan_start)
    {
        if (scan_start >= received_size)
            return 0;
        size_t scan_size = received_size - scan_start;
        if (scan_size >= pattern.size()) {
            char *found = (char *) memmem(
                &full_buf[scan_start],
                scan_size,
                pattern.data(),
                pattern.size());
            
            if (found) {
                scan_start = found - full_buf + pattern.size();
                return found;
            }
            scan_start = received_size - pattern.size() + 1;
        }
        return 0;
    }
    
    bool compare(const std::string& s1, const char* s2, size_t s2_size)
    {
        return (s1.size() == s2_size && 0 == memcmp(s1.data(), s2, s2_size));
    }
    
    bool match_uri()
    {
        assert(requestline_size && uri_start && uri_start <= requestline_size);
        while (uri_start < requestline_size && full_buf[uri_start] == ' ')
            ++uri_start;
        size_t rest_size = requestline_size - uri_start;
        if (rest_size == 0)
            return false;
        char *uri_end = 0;
        if (rest_size > 1)
            uri_end = (char *) memchr(&full_buf[uri_start + 1], ' ', rest_size - 1);
        uri_size = uri_end ? uri_end - &full_buf[uri_start] : rest_size;
        
        if (compare(QUERY_FAST, &full_buf[uri_start], uri_size)) {
            service = FAST;
            return true;
        }
        if (compare(QUERY_SLOW, &full_buf[uri_start], uri_size)) {
            service = SLOW;
            return true;
        }
        return false;
    }
    
    Status
    find_crlf()
    {
        char *crlf = scan_for(CRLF, crlf_scan);
        if (crlf) {
            if (!method_ok) {
                debug("wrong method!");
                return TERMINATE;
            }
            if (crlf_prev) {
                if (crlf - crlf_prev == CRLF.size()) {
                    return PROCEED; // found CRLFCRLF sequence
                }
            } else {
                requestline_size = crlf - full_buf;
                if (!match_uri()) {
                    debug("wrong query!");
                    return TERMINATE;
                }
            }
            crlf_prev = crlf;
        }
        return CONTINUE;
    }
    
    Status
    check_method()
    {
        if (received_size >= GET.size()) {
            method_ok = (0 == memcmp(full_buf, GET.data(), GET.size()));
            if (!method_ok) {
                debug("wrong method!");
                return TERMINATE;
            }
            if (crlf_scan < GET.size())
                crlf_scan = GET.size();
            parse = &ReqParser::find_crlf;
            uri_start = GET.size();
        }
        
        if (received_size < CRLF.size())
            return CONTINUE;

        Status res = CONTINUE;        
        while (res == CONTINUE && crlf_scan <= received_size - CRLF.size())
            res = find_crlf();
        
        return res;
    }
    
    ReqParser(char *full_buf_, size_t &received_size_) :
        parse{&ReqParser::check_method},
        full_buf{full_buf_},
        received_size{received_size_}
    {
    }
    
    Status
    operator()()
    {
        return (this->*parse)();
    }
};

class SlowTask : public Task
{
    struct ev_loop *event_loop;
    ev_async *async_watcher;
    
public:
    SlowTask(struct ev_loop* l, ev_async *w) :
        event_loop{l},
        async_watcher{w}
    {
    }
    virtual ~SlowTask()
    {
    }
    virtual void execute()
    {
        debug("SlowTask is started");
        usleep(OPT_VALUE_SLOW_DURATION * 1000);
        ev_async_send(event_loop, async_watcher);
        debug("SlowTask is ended");
    }
};

class ConnectionCtx : public OnPool<ConnectionCtx>
{
    static const size_t buf_size = 4096;
    struct ev_loop *event_loop;
    ev_io conn_watcher;
    ev_async async_watcher;
    char full_buf[buf_size + 1];
    char *recv_buf = full_buf;
    size_t received_size = 0;
    ReqParser parser;
    bool read_expected = true;
    ssize_t sent_size = 0;
    bool async_task = false;
    
    void read_conn()
    {
        size_t recv_size = recv(conn_watcher.fd, recv_buf, buf_size - received_size, 0);
        if (recv_size == 0) {
            debug ("peer shutdown");
            delete this;
            return;
        }
        if (recv_size == -1) {
            switch (errno) {
                case ENOTCONN:
                    debug ("peer reset");
                    delete this;
                    return;
                case EAGAIN:
                    return;
                default:
                    throw Errno("recv");
            }
        }
        received_size += recv_size;
        recv_buf += recv_size;
        assert (received_size <= buf_size);
        
        ReqParser::Status s = parser();
        switch(s) {
            case ReqParser::TERMINATE:
                delete this;
                return;
            case ReqParser::PROCEED: // reached request end
                debug("got request service ", parser.service);
                read_expected = false;
                if (parser.service == ReqParser::FAST || OPT_VALUE_WORKER_THREADS == 0) {
                    /* For fast request we do processing inside accept thread.
                       In this example there is no processing at all, we just activate
                       response sending. */
                    ev_io_stop(event_loop, &conn_watcher);
                    conn_watcher.events = EV_READ | EV_WRITE;
                    ev_io_start(event_loop, &conn_watcher);
                } else {
                    /* Push slow task into thread pool. Note, that read event is still active
                       in event loop. So, we terminate connection on unexpected read.
                       Asynchronous task must be aware of it! */
                    ev_async_start(event_loop, &async_watcher);
                    SlowTask task (event_loop, &async_watcher);
                    thread_pool.add_task(task);
                    async_task = true;
                }
                return;
            default:
                break;
        }
  
        if (received_size >= buf_size) {
            // TODO: mmap-based ring buffer (see https://github.com/willemt/cbuffer)
            error("Not enough buffer!");
            delete this;
            return;
        }
    }
    
    void terminate()
    {
        if (conn_watcher.fd) {
            debug("terminating connection");
            ev_io_stop(event_loop, &conn_watcher);
            close(conn_watcher.fd);
            conn_watcher.fd = 0;
        }
    }

    void read_unexpected()
    {
        char buf[1];
        size_t recv_size = recv(conn_watcher.fd, buf, 1, 0);
        if (recv_size == -1) {
            switch (errno) {
                case ENOTCONN:
                    debug ("peer reset");
                    if (async_task)
                        terminate();
                    else
                        delete this;
                    return;
                case EAGAIN:
                    return;
                default:
                    throw Errno("recv");
            }
        }
        if (recv_size == 0)
            debug("peer shutdown");
        else
            debug("unexpected read!");
        if (async_task)
            terminate();
        else
            delete this;
        return;
    }
    
    void write_conn()
    {
        ssize_t send_sz = send(conn_watcher.fd, RESPONSE.data() + sent_size, RESPONSE.size() - sent_size, 0);
        sent_size += send_sz;
        if (sent_size == RESPONSE.size()) {
            debug("sent reply");
            delete this;
            return;
        }
    }
    
    static void
    conn_callback (EV_P_ ev_io *w, int revents)
    {
        ConnectionCtx *self = (ConnectionCtx *)w->data;
        if (revents & EV_READ) {
            if (self->read_expected) {
                self->read_conn();
            } else {
                self->read_unexpected();
            }
        }
        if (revents & EV_WRITE)
            self->write_conn();
    }

    static void
    async_callback (EV_P_ ev_async *w, int revents)
    {
        ConnectionCtx *self = (ConnectionCtx *)w->data;
        self->async_task = false;
        ev_async_stop(self->event_loop, &self->async_watcher);
        if (self->conn_watcher.fd == 0) {
            delete self;
            return;
        }
        ev_io_stop(self->event_loop, &self->conn_watcher);
        self->conn_watcher.events = EV_READ | EV_WRITE;
        ev_io_start(self->event_loop, &self->conn_watcher);
    }

public:
    ConnectionCtx(struct ev_loop *event_loop_, int conn_fd) :
        event_loop{event_loop_},
        parser(full_buf, received_size)
    {
        debug("ConnectionCtx created");
        if (fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFL, 0) | O_NONBLOCK) == -1) {
            throw Errno("fcntl");
        }
        ev_io_init (&conn_watcher, conn_callback, conn_fd, EV_READ);
        ev_async_init (&async_watcher, async_callback);
        conn_watcher.data = this;
        async_watcher.data = this;
        ev_io_start(event_loop, &conn_watcher);
    }
    ConnectionCtx(const ConnectionCtx&) = delete;
    ~ConnectionCtx()
    {
        terminate();
        debug("ConnectionCtx destroying");
    }
};

INIT_POOL(ConnectionCtx);

using std::unique_ptr;

class AcceptTask : public Task
{
    /* Because AcceptTask is done inside event loop thread, processing must be fast enough
        to provide responsive frontend. This may be:
        1. read initial data;
        2. validate application protocol;
        3. fast answer and finish connection.
       Otherwise, if longer processing is required, additional task should created and routed to worker thread. */

    static const int MAX_LISTEN_QUEUE = SOMAXCONN;
    int listen_fd;
    // OPTIMIZE: addr can be shared
    struct sockaddr_in addr;

    // libev entities
    struct ev_loop *event_loop;
    ev_io accept_watcher;
    unique_ptr<Pool<ConnectionCtx> > pool;
    
    void
    accept_conn()
    {
        debug("AcceptTask incoming connection!");
        struct sockaddr_in peer_addr;
        socklen_t addr_len = sizeof (peer_addr);
        int conn_fd = accept(listen_fd, (sockaddr *)&peer_addr, &addr_len);
        if (conn_fd == -1) {
            if (errno != EAGAIN) {
                throw Errno("accept");
            }
            // something ugly happened: we should get valid conn_fd here (because of read event)
            error("Warning: unexpected EAGAIN!");
            return;
        }
        debug("got connection!");
        new (*pool) ConnectionCtx(event_loop, conn_fd);
    }

    static void
    accept_callback (EV_P_ ev_io *w, int revents)
    {
        ((AcceptTask *)w->data)->accept_conn();
    }

public:
    static size_t
    pool_size(size_t capacity)
    {
        decltype(pool)::element_type::memsize(capacity);
    }
    
    AcceptTask(size_t conn_capacity) :
        pool(new Pool<ConnectionCtx>(conn_capacity))
    {
        debug("AcceptTask created");
        // listen socket setup
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd == -1) {
            throw Errno("socket");;
        }
        int sock_opt = 1;
        // SO_REUSEPORT allows multiple sockets with same ADDRESS:PORT. Linux does load-balancing of incoming connections.
        // See long explanation in SO-14388706.
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, (char *) &sock_opt, sizeof(sock_opt)) == -1) {
            throw Errno("setsockopt");
        }
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(OPT_VALUE_PORT);
        if (bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
            throw Errno("bind");;
        }
        if (fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK) == -1) {
            throw Errno("fcntl");
        }
        if (listen(listen_fd, MAX_LISTEN_QUEUE) != 0) {
            throw Errno("listen");
        }
        // libev setup
        event_loop = ev_loop_new(EVBACKEND_EPOLL);
        ev_io_init (&accept_watcher, accept_callback, listen_fd, EV_READ);
        accept_watcher.data = this;
    }
    virtual ~AcceptTask()
    {
        if (event_loop)
            ev_loop_destroy(event_loop);
    }
    AcceptTask(AcceptTask &&src) :
        listen_fd{src.listen_fd},
        addr{src.addr},
        event_loop{src.event_loop},
        accept_watcher{src.accept_watcher},
        pool(std::move(src.pool))
    {
        debug("AcceptTask moved from ", &src);
        accept_watcher.data = this;
        src.listen_fd = 0;
        src.event_loop = nullptr;
    }
    virtual void execute()
    {
        ev_io_start(event_loop, &accept_watcher);
        socklen_t addr_len = sizeof(addr);
        int conn_fd = accept(listen_fd, (struct sockaddr *) &addr, &addr_len);
        if (conn_fd == -1) {
            if (errno != EAGAIN) {
                throw Errno("accept");
            }
        } else {
            ev_invoke(event_loop, &accept_watcher, EV_READ);
        }
        debug("running event loop...");
        ev_run(event_loop, 0);
    }
};

void
daemonize()
{
    static const int nochdir = 1;
    static const int noclose = ENABLED_OPT(VERBOSE);
    static const char *dir = "/var/tmp";

    if (chdir(dir))
        throw Errno("chdir ", dir);

    if (daemon(nochdir, noclose))
        throw Errno("daemon");
}

int
main(int argc, char ** argv)
{
    int res = optionProcess(&server_demoOptions, argc, argv);
    res = ferror(stdout);
    if (res != 0) {
        cerror("optionProcess", "output error writing to stdout!");
        return res;
    }

    if (!HAVE_OPT(ACCEPT_THREADS))
        OPT_VALUE_ACCEPT_THREADS = std::thread::hardware_concurrency();
    
    if (!HAVE_OPT(WORKER_THREADS))
        OPT_VALUE_WORKER_THREADS = OPT_VALUE_ACCEPT_THREADS;

    // main thread is also accept thread, thus decreasing spawning
    int accept_pool_sz = OPT_VALUE_ACCEPT_THREADS - 1;
        
    thread_pool.spawn_threads(accept_pool_sz + OPT_VALUE_WORKER_THREADS);
    
    cdebug("main", "Running ", OPT_VALUE_ACCEPT_THREADS, " "
           "accept threads; pool size: ", AcceptTask::pool_size(OPT_VALUE_ACCEPT_CAPACITY) / 1024, " kb; "
           "total pool size: ", AcceptTask::pool_size(OPT_VALUE_ACCEPT_CAPACITY) * OPT_VALUE_ACCEPT_THREADS / 1024, " kb.");

    try
    {
        for (int i = 0; i < accept_pool_sz; ++i) {
            AcceptTask accept_task(OPT_VALUE_ACCEPT_CAPACITY);
            thread_pool.add_task(accept_task);
        }

        if (ENABLED_OPT(DAEMONIZE))
            daemonize();

        AcceptTask accept_task(OPT_VALUE_ACCEPT_CAPACITY);
        accept_task.execute();
    } catch(std::bad_alloc &) {
        std::cerr << "Not enough memory!\n";
        return 10;
    } catch(std::exception &ex) {
        std::cerr << ex.what() << "\n";
        return 100;
    }

    return res;
}
