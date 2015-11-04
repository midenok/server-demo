## server-demo: event-driven multi-threaded connection server
Пример реализации сервера смешанной асинхронно-синхронной модели "event loop + worker threads". Такая модель позволяет задействовать преимущества обоих подходов: высокая производительность приёма коннектов в event loop; и конкурентность и простота алгоритма worker thread.

#### Устройство и принцип работы
Имеется некоторое сконфигурированное количество accept threads (опция `--accept-threads`), которое по умолчанию устанавливается в условное число ядер процессора (см. [std::thread::hardware_concurrency](http://en.cppreference.com/w/cpp/thread/thread/hardware_concurrency)). Каждый accept thread принимает входящие соединения на отдельном слушающем сокете. Однако, все слушающие сокеты привязаны к одному и тому же TCP-адресу (благодаря опции `SO_REUSEPORT`). Таким образом, входящие соединения равномерно распределяются ядром системы по имеющимся accept-тредам сервера.

Каждый accept thread исполняет внутри себя event loop (реализованный библиотекой libev). Все файл-дескрипторы обрабатываемые в event loop асинхронные (т.е. неблокирующиеся), включая конечно и слушающие сокеты (далее listen socket). После того, как соединение приходит в listen socket, формируется connection socket. Connection socket также добавляется в event loop и обрабатывается асинхронно в accept thread.

Если же некоторая задача бизнес-логики требует исполнения продолжительного алгоритма (а квантование на короткие промежутки времени представляется слишком сложным), такую задачу можно делегировать выделенному треду (worker thread). Также возможно делегировать туда и сам connection socket, убрав при этом его из event loop accept-треда, и обрабатывать в дальнейшем синхронно в worker thread. И даже, возможно организовать для него свой выделенный event loop и обрабатывать асинхронно в worker thread. Как видите, возможности весьма разнообразные! 

##### Как устроена работа с памятью
Прежде всего, главное требование к серверу -- во время обработки установленного соединения никаких malloc-ов! Память может быть преаллоцирована во время инициализации сервера, исходя из расчёта максимального количества одновременных соединений на один accept thread (опция `--accept-capacity`).

В данной реализации сервера реализован простейший пул памяти (класс `Pool`). Каждый accept thread обладает своим пулом памяти, поэтому защита от многопоточности для такого пула не нужна. Во время установления соединения accept thread формирует `ConnectionCtx` из своего пула. Время жизни `ConnectionCtx` равно времени установленного соединения: как только коннект разрывается (не важно с какой стороны), `ConnectionCtx` разрушается, а блок памяти возвращается в пул.

Если задача делегируется выделенному треду, то тред обладает этой задачей -- во время её исполнения она хранится в блоке памяти треда (класс `TaskHolder`). Чтобы избежать копирования, теоретически задача может быть создана сразу на памяти треда при помощи placement new. Однако, копирование не проблема, поскольку задумано, что размер задачи должен быть небольшим (соизмерим с передачей параметров через стек). Данная реализация ограничивает максимальный размер задачи в 96 байт (см. `TaskHolder`), при попытке использования классов большего размера компиляция завершится ошибкой.

В случае делегирования задачи выделенному треду, время жизни `ConnectionCtx` продлевается до времени жизни задачи, поскольку задача использует ресурсы `ConnectionCtx`. При этом `ConnectionCtx` может преждевременно разрывать соединение -- главное, чтобы ресурсы используемые задачей продолжали оставаться доступны.

##### Основная схема работы
Первоначально создаётся пул тредов, который равен сумме сконфигурированных accept и worker тредов минус 1 (главный тред также выступает в роли accept). Все accept-треды начинают исполнять `AcceptTask`. `AcceptTask` регистрирует callback на чтение listen socket (`accept_conn()`) в event loop и запускает event loop. Когда приходит соединение, создаётся `ConnectionCtx` который регистрирует connection socket в event loop у `AcceptTask`. Когда появляются данные, `ConnectionCtx` обрабатывает запрос при помощи `ReqParser` (`ConnectionCtx` также выступает в роли запроса, что означает, что не может быть несколько запросов на одном соединении). `ReqParser` различает два типа запросов: `FAST` и `SLOW`. В случае некорректного запроса `ConnectionCtx` разрывает соединение сразу, как только обнаружит это. Пример корректного запроса:
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
В случае `SLOW` `ConnectionCtx`, если число worker тредов равно нулю, сервер делает то же самое, что и в случае `FAST`. Если число worker тредов не равно нулю, worker-треду передаётся задача `SlowTask`, которая ожидает сконфигурированное количество миллисекунд и оповещает `ConnectionCtx` о своём завершении. Когда `ConnectionCtx` видит завершение `SlowTask`, он формирует вышеуказанный ответ и завершает соединение.

#### Тестирование сервера
Данная реализация сервера демонстрирует два вида GET-запросов: `/test/fast` и `/test/slow`. Первый из них сразу формирует ответ в accept-треде. Второй делегирует обработку в worker thread, где происходит задержка на сконфигурированный промежуток времени (опция `--slow-duration`). После чего accept thread формирует ответ.

##### Элементарное тестирование запросов
Здесь и далее, если это не указано явно, опция `--slow-duration` установлена в 30 миллисекунд по умолчанию, опция `--port` установлена в 9000 по умолчанию.
```
$ ./conn-daemon
```
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
Requests per second:    4901.96 [#/sec] (mean)
Time per request:       0.204 [ms] (mean)
Time per request:       0.204 [ms] (mean, across all concurrent requests)
Transfer rate:          272.86 [Kbytes/sec] received

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
Requests per second:    33.02 [#/sec] (mean)
Time per request:       30.285 [ms] (mean)
Time per request:       30.285 [ms] (mean, across all concurrent requests)
Transfer rate:          1.84 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.0      0       0
Processing:    30   30   0.0     30      30
Waiting:       30   30   0.0     30      30
Total:         30   30   0.0     30      30
```


