## server-demo: event-driven multi-threaded connection server
Пример реализации сервера смешанной асинхронно-синхронной модели "event loop + worker threads". Такая модель позволяет задействовать преимущества обоих подходов: высокая производительность приёма коннектов в event loop; и гарантированная конкурентность и простота алгоритма в worker thread.

#### Устройство и принцип работы
Имеется некоторое сконфигурированное количество accept threads (опция `--accept-threads`), которое по умолчанию устанавливается в условное число ядер процессора (см. [std::thread::hardware_concurrency](http://en.cppreference.com/w/cpp/thread/thread/hardware_concurrency)). Каждый accept thread принимает входящие соединения на отдельном слушающем сокете. Однако, все слушающие сокеты привязаны к одному и тому же TCP/IP адресу (благодаря флагу `SO_REUSEPORT`). Таким образом, входящие соединения равномерно распределяются ядром системы по имеющимся accept-тредам сервера.

Каждый accept thread исполняет внутри себя event loop (реализованный библиотекой [libev](http://software.schmorp.de/pkg/libev.html)). Все файл-дескрипторы обрабатываемые в event loop асинхронные (т.е. неблокирующиеся), включая конечно и слушающие сокеты (далее listen socket). После того, как соединение приходит в listen socket, формируется connection socket. Connection socket также добавляется в event loop и обрабатывается асинхронно в accept thread.

Если же некоторая задача бизнес-логики требует исполнения продолжительного алгоритма (а квантование на короткие промежутки времени представляется слишком сложным), такую задачу можно делегировать выделенному треду (worker thread). Также возможно делегировать туда и сам connection socket, убрав при этом его из event loop accept-треда, и обрабатывать в дальнейшем синхронно в worker thread. И даже, возможно организовать для него свой выделенный event loop и обрабатывать асинхронно в worker thread. Как видите, возможности весьма разнообразные! 

##### Как устроена работа с памятью
Основное требование к серверу -- во время приёма и обработки соединений не производить динамической аллокации памяти. Память может быть преаллоцирована во время инициализации сервера, исходя из расчёта максимального количества одновременных соединений на один accept thread (опция `--accept-capacity`).

Данная реализация сервера демонстрирует простейший пул памяти (класс `Pool`). Каждый accept thread обладает своим пулом памяти, поэтому защита от многопоточности для такого пула не нужна. После установления соединения accept thread формирует `ConnectionCtx` из своего пула. Время жизни `ConnectionCtx` равно времени установленного соединения: как только коннект разрывается (не важно с какой стороны), `ConnectionCtx` разрушается, а блок памяти возвращается в пул.

Если задача делегируется выделенному треду, то тред обладает этой задачей -- во время её исполнения она хранится в блоке памяти треда (класс `TaskHolder`). Чтобы избежать копирования, теоретически задача может быть создана сразу на памяти треда при помощи placement new. Однако, копирование не проблема, поскольку задумано, что размер задачи должен быть небольшим (соизмерим с передачей параметров через стек). Данная реализация ограничивает максимальный размер задачи в 96 байт (см. `TaskHolder`), при попытке использования классов большего размера компиляция завершится ошибкой.

В случае делегирования задачи выделенному треду, время жизни `ConnectionCtx` продлевается до времени жизни задачи, поскольку задача использует ресурсы `ConnectionCtx`. При этом `ConnectionCtx` может преждевременно разрывать соединение -- главное, чтобы ресурсы используемые задачей продолжали оставаться доступны.

##### Основная схема работы
Первоначально создаётся пул тредов, который равен сумме сконфигурированных accept и worker тредов минус 1 (главный тред программы также выступает в роли accept). Все accept-треды начинают исполнять `AcceptTask`. `AcceptTask` регистрирует callback на чтение listen socket (`accept_conn()`) в event loop и запускает event loop. Когда приходит соединение, создаётся `ConnectionCtx` который регистрирует connection socket в event loop у `AcceptTask`. Когда появляются данные, `ConnectionCtx` обрабатывает запрос при помощи `ReqParser` (`ConnectionCtx` также выступает в роли запроса, что означает, что не может быть несколько запросов на одном соединении). `ReqParser` различает два типа запросов: `FAST` и `SLOW`. В случае некорректного запроса `ConnectionCtx` разрывает соединение сразу, как только обнаружит это. Пример корректного запроса:
```
GET /test/fast<CR><LF>
<CR><LF>
```
В случае `FAST` `ConnectionCtx` отправляет следующий ответ и разрывает соединение:
```
HTTP/1.1 200 OK
Connection: close
Content-Length: 0
```
В случае `SLOW`, если число сконфигурированных worker тредов равно нулю, `ConnectionCtx` делает то же самое, что и в случае `FAST`. Если число worker тредов не равно нулю, свободному worker-треду передаётся задача `SlowTask`, которая ожидает сконфигурированное количество миллисекунд и оповещает `ConnectionCtx` о своём завершении. Когда `ConnectionCtx` видит завершение `SlowTask`, он формирует вышеуказанный ответ и завершает соединение.

Если все worker треды заняты на момент постановки новой задачи, то задача добавляется в очередь ожидания. Как только какой-либо тред освобождается, он забирает задачу из начала очереди (т.е. очередь это FIFO-стек).

#### Тестирование сервера
Данная реализация сервера поддерживает два вида GET-запросов: `/test/fast` и `/test/slow`. Первый из них сразу формирует ответ в accept-треде. Второй делегирует обработку в worker thread, где происходит задержка на сконфигурированный промежуток времени (опция `--slow-duration`). После чего accept thread формирует ответ.

Здесь и далее, если это не указано явно, опция `--slow-duration` установлена в 30 миллисекунд по умолчанию, опция `--port` установлена в 9000 по умолчанию.

##### 1 запрос в 1 потоке
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

##### N запросов в 1 потоке
Для наглядности используется число запросов, суммарно равное примерно 3-м секундам.
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
##### N запросов в 10 потоках
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
Как видим, `Time taken for tests` в случае `FAST` запросов не сильно изменился (2.536 по сравнению с 3.545). Это связано с тем, что в случае `FAST` запросов основновное время занимают накладные расходы, а не логика самого запроса. В случае `SLOW` запросов этот показатель уменьшился в 10 раз, как и ожидалось -- распараллеливание по тредам прошло нормально.

##### Нагрузка на `task_queue`
У пула тредов есть очередь задач. Если все треды заняты, задача ставится в очередь. Проверим, насколько хорошо это работает...
```
midenok@lian:~/src/server-demo/build$ ./server-demo -C 1000 -A 100 -w 1
```
---
```
midenok@lian:~$ ab -qn 500 -c 100 127.0.0.1:9000/test/slow
This is ApacheBench, Version 2.3 <$Revision: 1638069 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient).....done


Server Software:
Server Hostname:        127.0.0.1
Server Port:            9000

Document Path:          /test/slow
Document Length:        0 bytes

Concurrency Level:      100
Time taken for tests:   15.053 seconds
Complete requests:      500
Failed requests:        0
Total transferred:      28500 bytes
HTML transferred:       0 bytes
Requests per second:    33.22 [#/sec] (mean)
Time per request:       3010.562 [ms] (mean)
Time per request:       30.106 [ms] (mean, across all concurrent requests)
Transfer rate:          1.85 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    1   1.0      0       4
Processing:    37 2712 709.5   3008    3015
Waiting:       37 2712 709.5   3008    3015
Total:         39 2712 708.7   3008    3016

Percentage of the requests served within a certain time (ms)
  50%   3008
  66%   3008
  75%   3008
  80%   3008
  90%   3008
  95%   3009
  98%   3009
  99%   3009
 100%   3016 (longest request)
```
В данном случае, мы создали узкое место 1 worker thread, через которое проходили все `SLOW` запросы. Также, мы создали высокую конкурентность в 100 accept thread для проверки бесперебойности доступа к очереди из нескольких потоков. Как видим, общее время составило 15 секунд, что равно 500 * 30 мс, т.к. все запросы прошли через 1 поток. Очередь отработала нормально: `Complete requests: 500`.

##### Стресс-тест 1 час
```
midenok@lian:~/src/server-demo/build$ ./server-demo -C 1000 -w 300
```
---
```
midenok@lian:~$ ab -qt 3600 -n 100000000 -c 100 127.0.0.1:9000/test/slow
This is ApacheBench, Version 2.3 <$Revision: 1703952 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient).....done


Server Software:
Server Hostname:        127.0.0.1
Server Port:            9000

Document Path:          /test/slow
Document Length:        0 bytes

Concurrency Level:      100
Time taken for tests:   3600.000 seconds
Complete requests:      11926799
Failed requests:        0
Total transferred:      679827657 bytes
HTML transferred:       0 bytes
Requests per second:    3313.00 [#/sec] (mean)
Time per request:       30.184 [ms] (mean)
Time per request:       0.302 [ms] (mean, across all concurrent requests)
Transfer rate:          184.42 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.1      0       5
Processing:    30   30   0.1     30      47
Waiting:       27   30   0.1     30      46
Total:         30   30   0.1     30      49

Percentage of the requests served within a certain time (ms)
  50%     30
  66%     30
  75%     30
  80%     30
  90%     30
  95%     30
  98%     31
  99%     31
 100%     49 (longest request)
```
Параллельно с:
```
midenok@lian:~$ ab -qt 3600 -n 100000000 -c 100 127.0.0.1:9000/test/fast
This is ApacheBench, Version 2.3 <$Revision: 1703952 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient).....done


Server Software:
Server Hostname:        127.0.0.1
Server Port:            9000

Document Path:          /test/fast
Document Length:        0 bytes

Concurrency Level:      100
Time taken for tests:   2488.526 seconds
Complete requests:      100000000
Failed requests:        0
Total transferred:      5700000000 bytes
HTML transferred:       0 bytes
Requests per second:    40184.43 [#/sec] (mean)
Time per request:       2.489 [ms] (mean)
Time per request:       0.025 [ms] (mean, across all concurrent requests)
Transfer rate:          2236.83 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    1   0.2      1       5
Processing:     0    1   0.2      1      18
Waiting:        0    1   0.2      1      15
Total:          1    2   0.2      2      22

Percentage of the requests served within a certain time (ms)
  50%      2
  66%      2
  75%      2
  80%      2
  90%      3
  95%      3
  98%      3
  99%      3
 100%     22 (longest request)
```
Как видно из статистики, стресс-нагрузка `FAST` никак не повлияла на работу `SLOW`.

#### Опции запуска
```
midenok@lian:~/src/server-demo/build$ ./server-demo --help
server-demo - Server Example
Usage:  server-demo [ -<flag> [<val>] | --<name>[{=| }<val>] ]...

   -v, --verbose              Generate debug output to STDOUT
   -d, --daemonize            Detach from terminal and run in background
   -p, --port=num             Listen port
                                - it must be in the range:
                                  greater than or equal to 1
   -A, --accept-threads=num   Number of accept threads (defaults to number of CPU cores)
                                - it must be in the range:
                                  greater than or equal to 1
   -C, --accept-capacity=num  Maximum number of simultaneous accepted connections per 1 accept thread
(100 000)
                                - it must be in the range:
                                  greater than or equal to 1
   -w, --worker-threads=num   Worker threads to spawn (defaults to number of accept threads)
   -D, --slow-duration=num    Slow task delay in milliseconds (30)
   -?, --help                 display extended usage information and exit
   -!,  --- help           display extended usage information and exit

Options are specified by doubled hyphens and their name or by a single
hyphen and the flag character.
```
