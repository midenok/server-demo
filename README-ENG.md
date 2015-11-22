## server-demo: event-driven multi-threaded connection server
Example of mixed asynchronous-synchronous "event loop + worker threads" server model. Such model allows to utilize strengths of both methods: high productivity of accepting connections in event loop and guaranteed concurrency and algorithm simplicity in worker threads.

#### Design and working principle
There are some configured amount of accept threads (option `--accept-threads`) which by default is set to formal amount of CPU cores (see [std::thread::hardware_concurrency](http://en.cppreference.com/w/cpp/thread/thread/hardware_concurrency)). Each accept thread accepts incoming connections on separate listening socket. However, all listening sockets are bound to the same TCP/IP address (via `SO_REUSEPORT` flag). Thus, the incoming connections are distributed equally by system kernel to all existing server's accept threads.

Each accept thread runs the event loop (provided by [libev](http://software.schmorp.de/pkg/libev.html) library). All file descriptors handled by event loop are asynchronous (i.e. non-blocking), including of course the listening sockets. After the connection comes to listen socket, the new connection socket is created. Connection socket is also added into the event loop and handled asynchronously in accept thread.

If some business-logic task requires the execution of heavy-weight algorithm (and splitting to short time periods seems to be difficult), such task can be delegated to dedicated thread (worker thread). Also, it is possible to delegate there the connection socket itself, removing it from the event loop of accept thread first and handling synchronously in worker thread afterwards. And even more, it is possible to organise for it a dedicated event loop and handle it asynchronously in worker thread. As you can see, the possibilities are quite rich!

#### Working with memory
The main requirement for memory usage design is avoid dynamic allocations on connections handling. Memory can be preallocated at server initialization time by the parameter of maximum connection count per 1 accept thread (`--accept-capacity` option).

Server implementation demonstrates simple memory pool (`Pool` class). Each accept thread have its own memory pool, so there is no need to protect it from multiple threads. After new connection is established accept thread creates new `ConnectionCtx` from thread's memory pool. Life span of `ConnectionCtx` is equal to the time of established connection: when the connection is closed (no matter by what side), `ConnectionCtx` gets destroyed and memory block returns to its pool.

If the task is delegated to a dedicated worker thread, then that thread owns the task -- during the execution inside worker thread the task is kept in its private memory (`TaskHolder` class). To avoid object copying, theoretically the task can be created already inside the thread's memory with help of placement new operator. However, copy is not the bin problem because it is designed for a task to be as small as possible (comparable to function arguments passed through stack). Current implementation limits maximum size of task to 96 bytes (see `TaskHolder`) at compilation time.

In case task delegated to a worker thread, the life span `ConnectionCtx` is increased to the life span of a task, because the task uses `ConnectionCtx` resources. Wherein `ConnectionCtx` can disconnect peer at any time as long as the resources used by the task will still be available.

#### Server working scheme
At start a thread pool is created with amount of configured accept and worker threads minus 1 (the main thread is also plays the role of accept thread). All accept threads start to execute `AcceptTask`. `AcceptTask` registers a callback on listen socket read (`accept_conn()`) in event loop and runs that event loop. When a new connection comes new `ConnectionCtx` is created, which registers connection socket in `AcceptTask` event loop. When new data arrives, `ConnectionCtx` processes request with `ReqParser` (`ConnectionCtx` takes also request role, so it can't handle multiple requests). `ReqParser` detects two kinds of queries: `FAST` and `SLOW`. In case of incorrect request `ConnectionCtx` finishes the connection as soon as possible. Example of correct request:
```
GET /test/fast<CR><LF>
<CR><LF>
```
In case of `FAST` query, `ConnectionCtx` sends this response and finishes the connection:
```
HTTP/1.1 200 OK
Connection: close
Content-Length: 0
```
In case of `SLOW` query, if the amount of worker threads is zero, `ConnectionCtx` does the same as in case of `FAST`. If the amount of worker threads is not zero, the task `SlowTask` is passed to a free worker thread. Inside worker thread `SlowTask` waits some configured amount of time and notifies `ConnectionCtx` about its end. When `ConnectionCtx` sees `SlowTask` end, it generates the above response and finishes the connection.

If all worker threads are busy when the new task arrives, then this task is added to a wait queue. When some thread finishes its task, it takes a task from wait queue head (so the queue is a FIFO stack).

#### Testing
Current implementation supports two kinds of GET-requests: `/test/fast` and `/test/slow`. The former one does instant reply in accept thread. The latter one delegates processing to a worker thread, where it does delay for a configured amount of time (`--slow-duration` option). After that accept thread generates reply.

In the tests below `--slow-duration` is set to a default of 30 milliseconds, `--port` is set to a default of 9000.

##### 1 request in 1 thread
```
midenok@lian:~/src/server-demo/build$ ./server-demo -A 1
```
---
```
midenok@lian:~$ ab 127.0.0.1:9000/test/fast
This is ApacheBench, Version 2.3 <$Revision: 1638069 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient).....done


Server Software:
Server Hostname:        127.0.0.1
Server Port:            9000

Document Path:          /test/fast
Document Length:        0 bytes

Concurrency Level:      1
Time taken for tests:   0.000 seconds
Complete requests:      1
Failed requests:        0
Total transferred:      57 bytes
HTML transferred:       0 bytes
Requests per second:    4000.00 [#/sec] (mean)
Time per request:       0.250 [ms] (mean)
Time per request:       0.250 [ms] (mean, across all concurrent requests)
Transfer rate:          222.66 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.0      0       0
Processing:     0    0   0.0      0       0
Waiting:        0    0   0.0      0       0
Total:          0    0   0.0      0       0
```
---
```
midenok@lian:~$ ab 127.0.0.1:9000/test/slow
This is ApacheBench, Version 2.3 <$Revision: 1638069 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient).....done


Server Software:
Server Hostname:        127.0.0.1
Server Port:            9000

Document Path:          /test/slow
Document Length:        0 bytes

Concurrency Level:      1
Time taken for tests:   0.030 seconds
Complete requests:      1
Failed requests:        0
Total transferred:      57 bytes
HTML transferred:       0 bytes
Requests per second:    32.91 [#/sec] (mean)
Time per request:       30.389 [ms] (mean)
Time per request:       30.389 [ms] (mean, across all concurrent requests)
Transfer rate:          1.83 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.0      0       0
Processing:    30   30   0.0     30      30
Waiting:       30   30   0.0     30      30
Total:         30   30   0.0     30      30
```

##### N requests in 1 thread
For convenience the amount of queries is taken to be around 3 seconds in total. 
```
midenok@lian:~$ ab -qn 100000 127.0.0.1:9000/test/fast
This is ApacheBench, Version 2.3 <$Revision: 1638069 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient).....done


Server Software:
Server Hostname:        127.0.0.1
Server Port:            9000

Document Path:          /test/fast
Document Length:        0 bytes

Concurrency Level:      1
Time taken for tests:   3.545 seconds
Complete requests:      100000
Failed requests:        0
Total transferred:      5700000 bytes
HTML transferred:       0 bytes
Requests per second:    28211.74 [#/sec] (mean)
Time per request:       0.035 [ms] (mean)
Time per request:       0.035 [ms] (mean, across all concurrent requests)
Transfer rate:          1570.38 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.0      0       0
Processing:     0    0   0.0      0       0
Waiting:        0    0   0.0      0       0
Total:          0    0   0.0      0       0

Percentage of the requests served within a certain time (ms)
  50%      0
  66%      0
  75%      0
  80%      0
  90%      0
  95%      0
  98%      0
  99%      0
 100%      0 (longest request)
```
---
```
midenok@lian:~$ ab -qn 100 127.0.0.1:9000/test/slow
This is ApacheBench, Version 2.3 <$Revision: 1638069 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient).....done


Server Software:
Server Hostname:        127.0.0.1
Server Port:            9000

Document Path:          /test/slow
Document Length:        0 bytes

Concurrency Level:      1
Time taken for tests:   3.035 seconds
Complete requests:      100
Failed requests:        0
Total transferred:      5700 bytes
HTML transferred:       0 bytes
Requests per second:    32.95 [#/sec] (mean)
Time per request:       30.352 [ms] (mean)
Time per request:       30.352 [ms] (mean, across all concurrent requests)
Transfer rate:          1.83 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.0      0       0
Processing:    30   30   0.0     30      30
Waiting:       30   30   0.0     30      30
Total:         30   30   0.0     30      30

Percentage of the requests served within a certain time (ms)
  50%     30
  66%     30
  75%     30
  80%     30
  90%     30
  95%     30
  98%     30
  99%     30
 100%     30 (longest request)
 ```
##### N requests in 10 threads
```
midenok@lian:~/src/server-demo/build$ ./server-demo -A 10
```
---
```
midenok@lian:~$ ab -qn 100000 -c 10 127.0.0.1:9000/test/fast
This is ApacheBench, Version 2.3 <$Revision: 1638069 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient).....done


Server Software:
Server Hostname:        127.0.0.1
Server Port:            9000

Document Path:          /test/fast
Document Length:        0 bytes

Concurrency Level:      10
Time taken for tests:   2.536 seconds
Complete requests:      100000
Failed requests:        0
Total transferred:      5700000 bytes
HTML transferred:       0 bytes
Requests per second:    39432.46 [#/sec] (mean)
Time per request:       0.254 [ms] (mean)
Time per request:       0.025 [ms] (mean, across all concurrent requests)
Transfer rate:          2194.97 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.0      0       1
Processing:     0    0   0.0      0       1
Waiting:        0    0   0.0      0       1
Total:          0    0   0.0      0       1

Percentage of the requests served within a certain time (ms)
  50%      0
  66%      0
  75%      0
  80%      0
  90%      0
  95%      0
  98%      0
  99%      0
 100%      1 (longest request)
 ```
 ---
 ```
midenok@lian:~$ ab -qn 100 -c 10 127.0.0.1:9000/test/slow
This is ApacheBench, Version 2.3 <$Revision: 1638069 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient).....done


Server Software:
Server Hostname:        127.0.0.1
Server Port:            9000

Document Path:          /test/slow
Document Length:        0 bytes

Concurrency Level:      10
Time taken for tests:   0.307 seconds
Complete requests:      100
Failed requests:        0
Total transferred:      5700 bytes
HTML transferred:       0 bytes
Requests per second:    326.10 [#/sec] (mean)
Time per request:       30.665 [ms] (mean)
Time per request:       3.066 [ms] (mean, across all concurrent requests)
Transfer rate:          18.15 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.1      0       1
Processing:    30   30   0.2     30      31
Waiting:       30   30   0.2     30      31
Total:         30   31   0.3     30      31
ERROR: The median and mean for the total time are more than twice the standard
       deviation apart. These results are NOT reliable.

Percentage of the requests served within a certain time (ms)
  50%     30
  66%     31
  75%     31
  80%     31
  90%     31
  95%     31
  98%     31
  99%     31
 100%     31 (longest request)
```
