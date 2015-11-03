## server-demo: event-driven multi-threaded connection server
Пример реализации сервера смешанной асинхронно-синхронной модели "event loop + worker threads". Такая модель позволяет задействовать преимущества обоих подходов: высокая производительность приёма коннектов в event loop; и конкурентность и простота алгоритма worker thread!

#### Устройство и принцип работы
Имеется некоторое сконфигурированное количество accept threads (опция -A), которое по умолчанию устанавливается в условное количество ядер процессора (см. [std::thread::hardware_concurrency](http://en.cppreference.com/w/cpp/thread/thread/hardware_concurrency)). Каждый accept thread принимает входящие соединения на отдельном слушающем сокете. Однако, все слушающие сокеты привязаны к одному и тому же TCP-адресу, благодаря опции SO_REUSEPORT. Таким образом, входящие соединения равномерно распределяются ядром системы по имеющимся accept-трэдам сервера.

Каждый accept thread исполняет внутри себя event loop (реализованный библиотекой libev). Все файл-дескрипторы обрабатываемые в event loop асинхронные (т.е. неблокирующиеся), включая конечно и слушающие сокеты (далее listen socket). После того, как соединение приходит в listen socket, формируется connection socket. Connection socket также добавляется в event loop и обрабатывается асинхронно.

Если же некоторая задача бизнес-логики требует исполнения продолжительного алгоритма (а квантование на короткие промежутки времени представляется слишком запутанным), такую задачу можно делегировать выделенному трэду (worker thread). Также возможно делегировать туда и сам connection socket, убрав при этом его из event loop accept-трэда, и обрабатывать в дальнейшем синхронно в worker thread. И даже, возможно организовать для него свой выделенный event loop и обрабатывать асинхронно в worker thread. Как видите, возможности весьма разнообразные! 

##### Как устроена работа с памятью
Прежде всего, главное требование к серверу -- во время обработки коннекта никаких malloc-ов! Память может быть преаллоцирована во время инициализации сервера, исходя из расчёта максимального количества одновременных соединений на один accept thread (опция --accept-capacity). 


