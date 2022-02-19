# ficheda

### File Check Daemon
Usage: ficheda [-p path] [-i interval] [-j json]  
Or set an environment variable FICHEDA_PATH, FICHEDA_INTERVAL, FICHEDA_JSON respectively  

#### Сборка
git clone https://github.com/ru-ideni/ficheda  
cd ./ficheda/  
./build.sh  

#### Запуск с параметрами
cd ./bin/  
./ficheda -p /home/denis/FTC -i 2 -j /tmp/ficheda.json  

#### Запуск с переменными окружения
cd ./bin/  
export FICHEDA_PATH=/home/denis/FTC/  
export FICHEDA_INTERVAL=2  
export FICHEDA_JSON=/tmp/ficheda.json  
./ficheda  

#### Комбинированный запуск
cd ./bin/  
export FICHEDA_PATH=/home/denis/FTC/  
export FICHEDA_JSON=/tmp/ficheda.json  
./ficheda -i 2  

#### Команды мониторинга и управления
sudo tail -f /var/log/syslog  
while true; do cat /tmp/fichede.json; sleep 1; done  
while true; do killall -USR1 fichede; sleep 1; done  
killall -TERM ficheda  

### Общий алгоритм:
- отключение обработки некоторых сигналов
- переключение в редим демона
- обработка конфигурации
- инициализация разных семафоров
- переключение в рабочий каталог
- формирование эталонного списка файлов
- создание потока JSON-writer
- создание потока Calculators-Launcher
- жду сигнала TERM
- завершение работы

#### поток - Calculators-Launcher
- первичный расчёт CRC32
- инициализация обработчика сигнала USR1
- инициализация потока расчёта по таймеру (генерирует сигнал USR1)
- инициализация потока inotify (генерирует сигнал USR1)
- основной цикл вторичных расчётов
  - ожидание сигнала USR1
  - сканирование рабочего каталога
    - если файл в эталонном списке
      - запуск потока Calculator
    - если файл не в списке
      - запись в syslog (NEW file)
      - запись в pipe для JSON-writer (NEW file)
  - перебор эталонного списка файлов
    - если поток расчёта запускался
      - ожидание завершения потока расчёта
      - если CRC32 отличается от эталона - дианостика в syslog
    - если поток расчёта не запускался
      - значит файл в каталоге отсутствует
        - запись в syslog (DELETE file)
        - запись в pipe для JSON-writer (DELETE file)
  - запись в pipe для JSON-writer - признак конца отчёта
  - семафор для потока JSON-writer (начало генерации JSON-файла)
  - жду семафор от JSON-writer (окончание генерации JSON-файла)

#### поток - Calculator
- открываю поданый файл (ошибка - диагностика в syslog & json-pipe)
- блочно читаю файл и считаю CRC32 (ошибка - диагностика в syslog & json-pipe)
- закрываю файл (ошибка - диагностика в syslog & json-pipe)
- результат расчёта в json-pipe

#### поток - JSON-writer
- жду семафора для начала
- пересоздаю json-файл
- цикл чтения диагностики из json-pipe
  - читаю из pipe
  - формирую текст диагностики
  - пишу в json-файл
- закрываю json-файл
- поднимаю семафор об окончании
