## server-demo: event-driven multi-threaded connection server
Example of mixed asynchronous-synchronous "event loop + worker threads" server model. Such model allows to utilize strengths of both methods: high productivity of accepting connections in event loop and guaranteed concurrency and algorithm simplicity in worker threads.

#### Design and working principle
There are some configured amount of accept threads (option `--accept-threads`) which by default is set to formal amount of CPU cores (see [std::thread::hardware_concurrency](http://en.cppreference.com/w/cpp/thread/thread/hardware_concurrency)). Each accept thread accepts incoming connections on separate listening socket. However, all listening sockets are bound to the same TCP/IP address (via `SO_REUSEPORT` flag). Thus, the incoming connections are distributed equally by system kernel to all existing server's accept threads.
