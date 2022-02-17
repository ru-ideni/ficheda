/*
 *  File Check Daemon
 *
 *  Usage: ficheda [-p path] [-i interval] [-j json]
 *  Or set an environment variable FICHEDA_PATH, FICHEDA_INTERVAL, FICHEDA_JSON respectively.
 *
 *  Общий алгоритм:
 *  - отключение обработки некоторых сигналов
 *  - переключение в редим демона
 *  - обработка конфигурации
 *  - инициализация разных семафоров
 *  - переключение в рабочий каталог
 *  - формирование эталонного списка файлов
 *  - создание потока JSON-writer
 *  - создание потока Calculators-Launcher
 *  - жду сигнала TERM
 *  - завершение работы
 *
 *  поток - Calculators-Launcher
 *  - первичный расчёт CRC32
 *  - инициализация обработчика сигнала USR1
 *  - инициализация потока расчёта по таймеру (генерирует сигнал USR1)
 *  - инициализация потока inotify (генерирует сигнал USR1)
 *  - основной цикл вторичных расчётов
 *    - ожидание сигнала USR1
 *    - сканирование рабочего каталога
 *      - если файл в эталонном списке
 *        - запуск потока Calculator
 *      - если файл не в списке
 *        - запись в syslog (NEW file)
 *        - запись в pipe для JSON-writer (NEW file)
 *    - перебор эталонного списка файлов
 *      - если поток расчёта запускался
 *        - ожидание завершения потока расчёта
 *        - если CRC32 отличается от эталона - дианостика в syslog
 *      - если поток расчёта не запускался
 *        - значит файл в каталоге отсутствует
 *          - запись в syslog (DELETE file)
 *          - запись в pipe для JSON-writer (DELETE file)
 *    - запись в pipe для JSON-writer - признак конца отчёта
 *    - семафор для потока JSON-writer (начало генерации JSON-файла)
 *    - жду семафор от JSON-writer (окончание генерации JSON-файла)
 *
 *  поток - Calculator
 *  - открываю поданый файл (ошибка - диагностика в syslog & json-pipe)
 *  - читаю файл и считаю CRC32 (ошибка - диагностика в syslog & json-pipe)
 *  - закрываю файл (ошибка - диагностика в syslog & json-pipe)
 *  - результат расчёта в json-pipe
 *
 *  поток - JSON-writer
 *  - жду семафора для начала
 *  - пересоздаю json-файл
 *  - цикл чтения диагностики из json-pipe
 *    - читаю из pipe
 *    - формирую текст диагностики
 *    - пишу в json-файл
 *  - закрываю json-файл
 *  - поднимаю семафор об окончании
 *
 */
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CRC_START_32      0xFFFFFFFFul
#define FIN_BUFF_SIZE     1048576
#define CRC_THREADS_MAX   55
#define INO_EVENT_SIZE     sizeof(struct inotify_event)

static inline uint32_t crc32_start();
static inline uint32_t crc32_update(uint32_t crc, unsigned char c);
static inline uint32_t crc32_finish(uint32_t crc);

char* mission_path = NULL;
char* mission_json = NULL;
char* mission_interval_str = NULL;
int* mission_interval = NULL;

sem_t sem_crc32_threads_limit;
sem_t sem_sigusr1_queue;
sem_t sem_sigterm;
sem_t sem_json_write_start;
sem_t sem_json_write_finish;
pthread_t tid_calculators_launcher;
pthread_t tid_interval_sigusr1_raiser;
pthread_t tid_json_writer;
pthread_t tid_inotify;
int pipefd[2];
pthread_mutex_t mutex_pipe_write;

struct FCD_FILE {
    enum {FCD_STATE_NEW, FCD_STATE_OLD, FCD_STATE_ERR} state;
    char *name;
    int name_len;
    uint32_t crc32_original, crc32_next;
    pthread_t thread;
    int ittr;
    struct FCD_FILE *next;
};

struct FCD_FILE *fcd_file_first = NULL;
struct FCD_FILE *fcd_file_last = NULL;
int inotifyFd;

void obtain_mission(int _argc, char* _argv[]);
void skeleton_daemon();
void *my_malloc(size_t _size);
char *my_strdup(char *_str);
void my_read_pipe(void* _dest, size_t _sz);
void my_write_pipe(void* _src, size_t _sz);
void fcd_file_append(char *_file_name);
void my_signals_handler(int signum);
void *thread_crc32_calculator_entry_point(void *_arg);
_Noreturn void *thread_calculators_launcher_entry_point(void *_arg);
_Noreturn void *thread_interval_sigusr1_raiser_entry_point(void *_arg);
_Noreturn void *thread_json_writer_entry_point(void *_arg);
_Noreturn void *thread_mission_path_inotify(void *_arg);
void severe_error_0(const char* _errt, int _errc);
void severe_error_1(const char* _errt);
void severe_error_2(const char* _errf, const char* _errt, int _errc);
void severe_error_3(const char* _errt, int _i1, int _i2);
char* light_error_0(const char* _errf, const char* _errt, int _errc);

int main(int _argc, char* _argv[]) {
  int cc;
  //  temporary ignore some signals
  if (signal(SIGQUIT, SIG_IGN) == SIG_ERR) severe_error_0("signal(SIGQUIT)", errno);
  if (signal(SIGINT, SIG_IGN) == SIG_ERR) severe_error_0("signal(SIGINT)", errno);
  if (signal(SIGHUP, SIG_IGN) == SIG_ERR) severe_error_0("signal(SIGHUP)", errno);
  if (signal(SIGCONT, SIG_IGN) == SIG_ERR) severe_error_0("signal(SIGCONT)", errno);
  if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) severe_error_0("signal(SIGCHLD)", errno);
  if (signal(SIGTERM, SIG_IGN) == SIG_ERR) severe_error_0("signal(SIGTERM)", errno);
  if (signal(SIGUSR1, SIG_IGN) == SIG_ERR) severe_error_0("signal(SIGUSR1)", errno);
//  if (signal(SIGSTOP, my_signals_handler) == SIG_ERR) severe_error_0("signal(SIGSTOP)", errno);
  //  switch to daemon
  skeleton_daemon();
  //  open syslog
  openlog ("ficheda", LOG_PID, LOG_USER);
  syslog(LOG_NOTICE, "Program started (UserID=%i & PID=%i)", getuid(), getpid());
  //  obtain mission parameters
  obtain_mission(_argc, _argv);
  //  initialize some semaphore & mutex
  if (sem_init(&sem_sigusr1_queue, 0, 0)) severe_error_0("sem_init(sem_sigusr1_queue)", errno);
  if (sem_init(&sem_sigterm, 0, 0)) severe_error_0("sem_init(sem_sigterm)", errno);
  if (sem_init(&sem_json_write_start, 0, 0)) severe_error_0("sem_init(sem_json_write_start)", errno);
  if (sem_init(&sem_json_write_finish, 0, 0)) severe_error_0("sem_init(sem_json_write_finish)", errno);
  if (pthread_mutex_init(&mutex_pipe_write, NULL)) severe_error_0("pthread_mutex_init(mutex_pipe_write)", errno);
  //  goto mission directory
  syslog(LOG_NOTICE, "Connect to mission_path: %s", mission_path);
  if (chdir(mission_path) != 0) {
    syslog(LOG_ERR, "chdir(mission_path): %s", strerror(errno));
    syslog(LOG_NOTICE, "Program stoped (UserID=%i & PID=%i)", getuid(), getpid());
    exit(EXIT_FAILURE);
  }
  //  create
  if (pipe(pipefd) == -1) severe_error_0("pipe()", errno);
  //----------------------------------------------------------------------------
  DIR *mission_dir;
  struct dirent *dir_entry;
  mission_dir = opendir(mission_path);
  if (!mission_dir) severe_error_0("opendir(mission_path)", errno);
  errno = 0;
  while ((dir_entry = readdir(mission_dir))) {
    if (!(dir_entry->d_type & DT_REG)) continue;
    fcd_file_append(dir_entry->d_name);
  }
  if (errno) severe_error_0("readdir()", errno);
  if (closedir(mission_dir)) severe_error_0("closedir()", errno);
  //----------------------------------------------------------------------------
  if (sem_init(&sem_crc32_threads_limit, 0, CRC_THREADS_MAX))
    severe_error_0("sem_init(sem_crc32_threads_limit)", errno);
  //----------------------------------------------------------------------------
  if (signal(SIGTERM, my_signals_handler) == SIG_ERR)
    severe_error_0("signal(SIGTERM)", errno);
  //----------------------------------------------------------------------------
  cc = pthread_create(&tid_json_writer, NULL, &thread_json_writer_entry_point, NULL);
  if (cc != 0) severe_error_0("pthread_create(tid_json_writer)", cc);
  //----------------------------------------------------------------------------
  cc = pthread_create(&tid_calculators_launcher, NULL, &thread_calculators_launcher_entry_point, NULL);
  if (cc != 0) severe_error_0("pthread_create(tid_calculators_launcher)", cc);
  //----------------------------------------------------------------------------
  if (sem_wait(&sem_sigterm)) severe_error_0("sem_wait(sem_sigterm)", errno);
  syslog(LOG_NOTICE, "Program stoped (UserID=%i & PID=%i)", getuid(), getpid());
  //----------------------------------------------------------------------------
  return (0);
}

void skeleton_daemon()
{
  pid_t pid;
  // Fork off the parent process
  pid = fork();
  // An error occurred
  if (pid < 0) exit(EXIT_FAILURE);
  // Success: Let the parent terminate
  if (pid > 0) exit(EXIT_SUCCESS);
  // On success: The child process becomes session leader
  if (setsid() < 0) exit(EXIT_FAILURE);
  // Fork off for the second time
  pid = fork();
  // An error occurred
  if (pid < 0) exit(EXIT_FAILURE);
  // Success: Let the parent terminate
  if (pid > 0) exit(EXIT_SUCCESS);
  // Set new file permissions
  umask(0);
  // Change the working directory to the root directory
  // or another appropriated directory
  chdir("/");
  // Close all open file descriptors
  int x;
  for (x = sysconf(_SC_OPEN_MAX); x>=0; x--)
  {
    close (x);
  }
}

void fcd_file_append(char *_file_name) {
  //  create list item
  if (!fcd_file_first) { //  first item
    fcd_file_first = fcd_file_last = my_malloc(sizeof(struct FCD_FILE));
  } else {  //  next item
    fcd_file_last->next = my_malloc(sizeof(struct FCD_FILE));
    fcd_file_last = fcd_file_last->next;
  }
  //  fill  new list item
  fcd_file_last->state = FCD_STATE_NEW;
  fcd_file_last->next = NULL;
  fcd_file_last->name = my_strdup(_file_name);
  fcd_file_last->name_len = strlen(fcd_file_last->name);
  fcd_file_last->crc32_original = fcd_file_last->crc32_next = CRC_START_32;
  fcd_file_last->ittr = 0;
  return;
}

void my_write_pipe(void* _src, size_t _sz) {
  int wl;
  if ((wl=write(pipefd[1], _src, _sz)) < 0) severe_error_0("write(pipefd[1])", errno);
  if (wl != _sz) severe_error_3("write(pipefd[1]) dl=%i, wl=i%", _sz, wl);
}

_Noreturn void *thread_mission_path_inotify(void *_arg){
  int rl;
  char ino_buff[1024];
  struct inotify_event *ino_event = (struct inotify_event *)&ino_buff;
//  char event[sizeof(struct inotify_event)] __attribute__ ((aligned(8)));
  //  loop for inotify
  while (1) {
    //  read inotify event structure
    rl = read(inotifyFd, ino_buff, sizeof(ino_buff));
    if (rl < 1) severe_error_0("read(INOTIFY)", errno);
    //  check
    if ((ino_event->mask & IN_DELETE_SELF) || (ino_event->mask & IN_MOVE_SELF)) {
      //  disaster!!!
      syslog(LOG_ERR, "Disaster!!! Mission directory - deleted!!!");
      exit(EXIT_FAILURE);
    } else {
      //  post SIGUSR1 semaphore
      sem_post(&sem_sigusr1_queue);
    }
  }
}

void thread_calculators_launcher_inotify(void) {
  int cc;
  //  initialize inotify
  inotifyFd = inotify_init();                 // Create inotify instance
  if (inotifyFd == -1) severe_error_0("inotify_init()", errno);
  //  add watch
  cc = inotify_add_watch(inotifyFd, mission_path, IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);
  if (cc == -1) severe_error_0("inotify_add_watch()", errno);
  //  create thread
  cc = pthread_create(&tid_inotify, NULL, &thread_mission_path_inotify, NULL);
  if (cc != 0) severe_error_0("pthread_create(fcd_file)", cc);
}

void thread_calculators_launcher_pipe_status(char* _file, char *_msg) {
  int dl, sc, wl;
  //  lock mutex
  if (pthread_mutex_lock(&mutex_pipe_write)) severe_error_0("pthread_mutex_lock(mutex_pipe_write)", errno);
  //  write file status to pipe
  sc = 3;
  my_write_pipe(&sc, sizeof(sc));
  //  write additional information
  dl = strlen(_file) + 1;
  my_write_pipe(&dl, sizeof(dl));
  my_write_pipe(_file, dl);
  //  write additional information
  dl = strlen(_msg) + 1;
  my_write_pipe(&dl, sizeof(dl));
  my_write_pipe(_msg, dl);
  //  unlock mutex
  if (pthread_mutex_unlock(&mutex_pipe_write)) severe_error_0("pthread_mutex_unlock(mutex_pipe_write)", errno);
  //
  return;
}

_Noreturn void *thread_calculators_launcher_entry_point(void *_arg) {
  int cc;
  struct FCD_FILE *fcd_file;
  //  initial calculation
  fcd_file = fcd_file_first;
  while (fcd_file) {
    if (sem_wait(&sem_crc32_threads_limit)) severe_error_0("sem_wait(sem_crc32_threads_limit)", errno);
    cc = pthread_create(&fcd_file->thread, NULL, &thread_crc32_calculator_entry_point, fcd_file);
    if (cc != 0) severe_error_0("pthread_create(fcd_file)", cc);
    fcd_file = fcd_file->next;
  }
  fcd_file = fcd_file_first;
  while (fcd_file) {
    pthread_join(fcd_file->thread, NULL);
    if (fcd_file->state == FCD_STATE_ERR)
      severe_error_1("Initial calculation failed! Program stoped!");
    fcd_file = fcd_file->next;
  }
  //  syslog message
  syslog(LOG_NOTICE, "Initial calculation finished. Service ready.");
  //  initialize SIGUSR1-handler
  if (signal(SIGUSR1, my_signals_handler) == SIG_ERR) severe_error_0("signal(SIGUSR1)", errno);
  //  initialize interval-timer
  cc = pthread_create(&tid_interval_sigusr1_raiser, NULL, &thread_interval_sigusr1_raiser_entry_point, NULL);
  if (cc != 0) severe_error_0("pthread_create(tid_interval_sigusr1_raiser)", cc);
  //  initialize inotify event
  thread_calculators_launcher_inotify();
  //  regular calculation
  for (int ittr=1; ; ++ittr) {
    //  wait for next signal
    if (sem_wait(&sem_sigusr1_queue)) severe_error_0("sem_wait(sem_sigusr1_queue)", errno);
    int integrity_check_ok = 1;
    //  itterate dir
    DIR *mission_dir;
    struct dirent *dir_entry;
    mission_dir = opendir(mission_path);
    if (!mission_dir) severe_error_0("opendir(mission_path)", errno);
    errno = 0;
    while ((dir_entry = readdir(mission_dir))) {
      if (!(dir_entry->d_type & DT_REG)) continue;
      //  select the appropriate file & create thread
      fcd_file = fcd_file_first;
      while (fcd_file) {
        if (strcmp(fcd_file->name, dir_entry->d_name) == 0) {
          if (sem_wait(&sem_crc32_threads_limit)) severe_error_0("sem_wait(sem_crc32_threads_limit)", errno);
          cc = pthread_create(&fcd_file->thread, NULL, &thread_crc32_calculator_entry_point, fcd_file);
          if (cc != 0) severe_error_0("pthread_create(fcd_file)", cc);
          fcd_file->ittr = ittr;
          break;
        }
        fcd_file = fcd_file->next;
      }
      //  check the file exits in original list
      if (!fcd_file) {
        syslog(LOG_WARNING, "Integrity check: FAIL (%s/%s - NEW)", mission_path, dir_entry->d_name);
        thread_calculators_launcher_pipe_status(dir_entry->d_name, "NEW");
        integrity_check_ok = 0;
      }
    }
    if (errno) severe_error_0("readdir()", errno);
    if (closedir(mission_dir)) severe_error_0("closedir()", errno);
    //  wait for all threads end
    fcd_file = fcd_file_first;
    while (fcd_file) {
      //  check for missing files
      if (fcd_file->ittr != ittr) {
        syslog(LOG_WARNING, "Integrity check: FAIL (%s/%s - DELETED)", mission_path, fcd_file->name);
        thread_calculators_launcher_pipe_status(fcd_file->name, "DELETED");
        integrity_check_ok = 0;
        fcd_file = fcd_file->next;
        continue;
      }
      //  wait for thread
      cc = pthread_join(fcd_file->thread, NULL);
      if (cc != 0) severe_error_0("pthread_join(fcd_file)", cc);
      if (fcd_file->state == FCD_STATE_ERR)
        integrity_check_ok = 0;
      else if (fcd_file->crc32_next != fcd_file->crc32_original) {
        integrity_check_ok = 0;
        syslog(LOG_WARNING, "Integrity check: FAIL (%s/%s - CRC32 <0x%08X,0x%08X>)", mission_path, fcd_file->name,
               fcd_file->crc32_original, fcd_file->crc32_next);
      }
      fcd_file = fcd_file->next;
    }
    if (integrity_check_ok) {
      syslog(LOG_NOTICE, "Integrity check: OK");
    }
    //  lock mutex
    if (pthread_mutex_lock(&mutex_pipe_write)) severe_error_0("pthread_mutex_lock(mutex_pipe_write)", errno);
    //  write status
    int sc = 9, wl;
    my_write_pipe(&sc, sizeof(sc));
    //  unlock mutex
    if (pthread_mutex_unlock(&mutex_pipe_write)) severe_error_0("pthread_mutex_unlock(mutex_pipe_write)", errno);
    //  start json writer
    sem_post(&sem_json_write_start);
    //  wait for json writer finish
    sem_wait(&sem_json_write_finish);
  }
}

void thread_crc32_calculator_finish(struct FCD_FILE *_fcd_file, char *_text, int _errno, void* _buff) {
  //  lock mutex
  if (pthread_mutex_lock(&mutex_pipe_write)) severe_error_0("pthread_mutex_lock(mutex_pipe_write)", errno);
  //
  int dl, sc, wl;
  if (!_text) {
    //  if not initial calculation
    if (_fcd_file->state != FCD_STATE_NEW) {
      //  write file status to pipe
      sc = 1;
      my_write_pipe(&sc, sizeof(sc));
      my_write_pipe(&_fcd_file, sizeof(_fcd_file));
    }
    _fcd_file->state = FCD_STATE_OLD;
  } else {
    //  if initial calculation
    if (_fcd_file->state == FCD_STATE_NEW) severe_error_2(_fcd_file->name, _text, errno);
    //  write file status to pipe
    sc = 2;
    my_write_pipe(&sc, sizeof(sc));
    my_write_pipe(&_fcd_file, sizeof(_fcd_file));
    //  write additional information
    char *errmsg = light_error_0(_fcd_file->name, _text, errno);
    dl = strlen(errmsg) + 1;
    my_write_pipe(&dl, sizeof(dl));
    my_write_pipe(errmsg, dl);
    free(errmsg);
    _fcd_file->state = FCD_STATE_ERR;
  }
  //
  if (_buff) free(_buff);
  //  unlock mutex
  if (pthread_mutex_unlock(&mutex_pipe_write)) severe_error_0("pthread_mutex_unlock(mutex_pipe_write)", errno);
  //
  if (sem_post(&sem_crc32_threads_limit)) severe_error_0("sem_post(sem_crc32_threads_limit)", errno);
  //
  return;
}

void *thread_crc32_calculator_entry_point(void *_arg) {
  int i, fecc;
  struct FCD_FILE *fcd_file = _arg;
  FILE *fin = fopen(fcd_file->name, "rb");
  if (!fin) {
    thread_crc32_calculator_finish(fcd_file, "fopen", errno, NULL);
    return NULL;
  }
  u_char *buff = my_malloc(FIN_BUFF_SIZE);
  uint32_t crc32 = crc32_start();
  for (; (i = fread(buff, 1, FIN_BUFF_SIZE, fin)) > 0;) {
    u_char* ptr = buff;
    for (int j = 0; j < i; ++j) {
      crc32 = crc32_update(crc32, *(ptr++));
    }
  }
  fecc = ferror(fin);
  if (fclose(fin)) {
    thread_crc32_calculator_finish(fcd_file, "fclose", errno, buff);
    return NULL;
  }
  if (fecc) {
    thread_crc32_calculator_finish(fcd_file, "fread", errno, buff);
    return NULL;
  }
  crc32 = crc32_finish(crc32);
  if (fcd_file->state == FCD_STATE_NEW) fcd_file->crc32_original = crc32;
  else fcd_file->crc32_next = crc32;
  thread_crc32_calculator_finish(fcd_file, NULL, 0, buff);
  return NULL;
}

void thread_json_writer_stream_struct(int _i1, int _i2) {
  if (_i1 != _i2) {
    syslog(LOG_ERR, "read(pipefd[0]) error in stream structure! (expected: %i, received: %i)", _i1, _i2);
    exit(EXIT_FAILURE);
  }
}

void thread_json_writer_stream_data(void) {
  syslog(LOG_ERR, "read(pipefd[0]) error in stream data!");
  exit(EXIT_FAILURE);
}

void my_read_pipe(void* _dest, size_t _sz) {
  int rl;
  if ((rl = read(pipefd[0], _dest, _sz)) < 0) severe_error_0("read(pipefd[0])", errno);
  thread_json_writer_stream_struct(_sz, rl);
}

_Noreturn void *thread_json_writer_entry_point(void *_arg) {
  int lr, sc, ml;
  time_t ct;
  struct FCD_FILE* fcd_file;
  char* jm0;
  char* jm1 = " {\"path\":\"%s/%s\",\"etalon_crc32\":\"0x%08X\",\"result_crc32\":\"0x%08X\",\"status\":\"OK\"}\n";
  char* jm2 = " {\"path\":\"%s/%s\",\"etalon_crc32\":\"0x%08X\",\"result_crc32\":\"0x%08X\",\"status\":\"FAIL\"}\n";
  char* jm3 = ",{\"path\":\"%s/%s\",\"etalon_crc32\":\"0x%08X\",\"result_crc32\":\"0x%08X\",\"status\":\"OK\"}\n";
  char* jm4 = ",{\"path\":\"%s/%s\",\"etalon_crc32\":\"0x%08X\",\"result_crc32\":\"0x%08X\",\"status\":\"FAIL\"}\n";
  char* jm5 = " {\"path\":\"%s/%s\",\"status\":\"%s\"}\n";
  char* jm6 = ",{\"path\":\"%s/%s\",\"status\":\"%s\"}\n";
  char buff1[1024];
  char buff2[1024];
  while(1) {
    //  wait semaphore
    sem_wait(&sem_json_write_start);
    //  (re)create json-file
    FILE* fout = fopen(mission_json, "w+t");
    if (!fout) severe_error_0("fopen(mission_json)", errno);
    //  write json-header
    if (fprintf(fout, "[\n") < 0) severe_error_0("fprintf(fout)", errno);
    //  loop for output data
    for (int ilf=1; ilf>0; ++ilf) {
      //  read status-code
      if ((lr = read(pipefd[0], &sc, sizeof(sc))) < 0) severe_error_0("read(pipefd[0])(---)", errno);
      thread_json_writer_stream_struct(sizeof(sc), lr);
      switch (sc) {
        case 1: //  normal status
          //  read FCD_STATUS
          my_read_pipe(&fcd_file, sizeof(fcd_file));
          //  write information
          if (ilf == 1) if (fcd_file->crc32_original == fcd_file->crc32_next) jm0 = jm1;
            else jm0 = jm2;
          else if (fcd_file->crc32_original == fcd_file->crc32_next) jm0 = jm3;
          else jm0 = jm4;
          fprintf(fout, jm0, mission_path, fcd_file->name, fcd_file->crc32_original, fcd_file->crc32_next);
          break;
        case 2: //  error status
          //  read FCD_STATUS
          my_read_pipe(&fcd_file, sizeof(fcd_file));
          //  read message length
          my_read_pipe(&ml, sizeof(ml));
          //  read message
          my_read_pipe(&buff1, ml);
          //  write information
          if (ilf == 1) jm0 = jm5;
          else jm0 = jm6;
          fprintf(fout, jm0, mission_path, fcd_file->name, buff1);
          break;
        case 3: //  error status
          //  read file name length
          my_read_pipe(&ml, sizeof(ml));
          //  read file name
          my_read_pipe(&buff1, ml);
          //  read message length
          my_read_pipe(&ml, sizeof(ml));
          //  read message
          my_read_pipe(&buff2, ml);
          //  write information
          if (ilf == 1) jm0 = jm5;
          else jm0 = jm6;
          fprintf(fout, jm0, mission_path, buff1, buff2);
          break;
        case 9: //  end-of-stream
          ilf = -1;
          break;
        default:  //  unknown status-code
          syslog(LOG_ERR, "sc = 0x%08X (%i)", sc, sc);
          thread_json_writer_stream_data();
          break;
      }
    }
    //  write json-footer
    if (fprintf(fout, "]\n") < 0) severe_error_0("fprintf(fout)", errno);
    //  close json-file
    if (fclose(fout)) severe_error_0("fclose(mission_json)", errno);
    //  post semaphore
    sem_post(&sem_json_write_finish);
  }
}

_Noreturn void *thread_interval_sigusr1_raiser_entry_point(void *_arg) {
  while(1) {
    sleep(*mission_interval);
    raise(SIGUSR1);
  }
}

void syslog_usage(void) {
  syslog(LOG_ERR, "Usage: ficheda [-p path] [-i interval] [-j json]");
  syslog(LOG_ERR, "Or set an environment variable FICHEDA_PATH, FICHEDA_INTERVAL, FICHEDA_JSON respectively.");
}

void obtain_mission(int _argc, char* _argv[]) {
  int opt = 0, i;
  opterr = 0; //  disable output on error for getopt_long
  while ((opt = getopt(_argc, _argv, "p:i:j:")) != -1) {
    switch (opt) {
      case 'p':
        mission_path = strdup(optarg);
        break;
      case 'i':
        mission_interval_str = strdup(optarg);
        break;
      case 'j':
        mission_json = strdup(optarg);
        break;
      default:
        syslog_usage();
        exit(EXIT_FAILURE);
    }
  }
  if (!mission_path) {
    mission_path = getenv("FICHEDA_PATH");
    if (!mission_path) {
      syslog(LOG_ERR, "[path] not set");
      syslog_usage();
      exit(EXIT_FAILURE);
    }
  }
  if (!mission_interval_str) {
    mission_interval_str = getenv("FICHEDA_INTERVAL");
    if (!mission_interval_str) {
      syslog(LOG_ERR, "[interval] not set");
      syslog_usage();
      exit(EXIT_FAILURE);
    }
  }
  if (!mission_json) {
    mission_json = getenv("FICHEDA_JSON");
    if (!mission_json) {
      syslog(LOG_ERR, "[json] not set");
      syslog_usage();
      exit(EXIT_FAILURE);
    }
  }
  int lcp = strlen(mission_path) - 1;
  if (mission_path[lcp] == '/') mission_path[lcp] = '\0';
  if (sscanf(mission_interval_str, "%d", &i) == 1) {
    mission_interval = my_malloc(sizeof(int));
    *mission_interval = i;
  } else {
    syslog(LOG_ERR, "[interval] wrong value");
    syslog_usage();
    exit(EXIT_FAILURE);
  }
  syslog(LOG_NOTICE, "mission_path     = [%s]\n", mission_path);
  syslog(LOG_NOTICE, "mission_interval = [%i]\n", *mission_interval);
  syslog(LOG_NOTICE, "mission_json     = [%s]\n", mission_json);
}

void severe_error_0(const char* _errt, int _errc) {
  const int strerrs = 1024;
  char* strerrt = my_malloc(strerrs);
  strerror_r(_errc, strerrt, strerrs);
  syslog(LOG_ERR, "%s: [%i][%s]", _errt, _errc, strerrt);
  exit(EXIT_FAILURE);
}

void severe_error_1(const char* _errt) {
  syslog(LOG_ERR, "%s", _errt);
  exit(EXIT_FAILURE);
}

void severe_error_2(const char* _errf, const char* _errt, int _errc) {
  const int strerrs = 1024;
  char* strerrt = my_malloc(strerrs);
  strerror_r(_errc, strerrt, strerrs);
  syslog(LOG_WARNING, "Initial calculation: FAIL (%s/%s - %s: [%i] %s)", mission_path, _errf, _errt, _errc, strerrt);
  exit(EXIT_FAILURE);
}

void severe_error_3(const char* _errt, int _i1, int _i2) {
  syslog(LOG_ERR, _errt, _i1, _i2);
  exit(EXIT_FAILURE);
}

char* light_error_0(const char* _errf, const char* _errt, int _errc) {
  const int strerrs = 1024;
  char* strerrt = my_malloc(strerrs);
  strerror_r(_errc, strerrt, strerrs);
  syslog(LOG_WARNING, "Integrity check: FAIL (%s/%s - %s: [%i] %s)", mission_path, _errf, _errt, _errc, strerrt);
  return strerrt;
}

void my_signals_handler(int signum) {
  switch (signum) {
    case SIGUSR1:
      if (sem_post(&sem_sigusr1_queue)) severe_error_0("sem_post(sem_sigusr1_queue)", errno);
      break;
    case SIGTERM:
      if (sem_post(&sem_sigterm)) severe_error_0("sem_post(sem_sigterm)", errno);
      break;
    default:
      break;
  }
  return;
}

void *my_malloc(size_t _size) {
  void *ptr = malloc(_size);
  if (!ptr) {
    syslog(LOG_ERR, "Out of memory!!!");
    raise(SIGTERM);
  }
  return ptr;
}

char *my_strdup(char *_str) {
  char *ptr = strdup(_str);
  if (!ptr) {
    syslog(LOG_ERR, "Out of memory!!!");
    raise(SIGTERM);
  }
  return ptr;
}

/*
 * Table for the CRC 32 calculation
 */
const uint32_t crc_tab32[256] = {
  0x00000000ul, 0x77073096ul, 0xEE0E612Cul, 0x990951BAul, 0x076DC419ul, 0x706AF48Ful, 0xE963A535ul, 0x9E6495A3ul,
  0x0EDB8832ul, 0x79DCB8A4ul, 0xE0D5E91Eul, 0x97D2D988ul, 0x09B64C2Bul, 0x7EB17CBDul, 0xE7B82D07ul, 0x90BF1D91ul,
  0x1DB71064ul, 0x6AB020F2ul, 0xF3B97148ul, 0x84BE41DEul, 0x1ADAD47Dul, 0x6DDDE4EBul, 0xF4D4B551ul, 0x83D385C7ul,
  0x136C9856ul, 0x646BA8C0ul, 0xFD62F97Aul, 0x8A65C9ECul, 0x14015C4Ful, 0x63066CD9ul, 0xFA0F3D63ul, 0x8D080DF5ul,
  0x3B6E20C8ul, 0x4C69105Eul, 0xD56041E4ul, 0xA2677172ul, 0x3C03E4D1ul, 0x4B04D447ul, 0xD20D85FDul, 0xA50AB56Bul,
  0x35B5A8FAul, 0x42B2986Cul, 0xDBBBC9D6ul, 0xACBCF940ul, 0x32D86CE3ul, 0x45DF5C75ul, 0xDCD60DCFul, 0xABD13D59ul,
  0x26D930ACul, 0x51DE003Aul, 0xC8D75180ul, 0xBFD06116ul, 0x21B4F4B5ul, 0x56B3C423ul, 0xCFBA9599ul, 0xB8BDA50Ful,
  0x2802B89Eul, 0x5F058808ul, 0xC60CD9B2ul, 0xB10BE924ul, 0x2F6F7C87ul, 0x58684C11ul, 0xC1611DABul, 0xB6662D3Dul,
  0x76DC4190ul, 0x01DB7106ul, 0x98D220BCul, 0xEFD5102Aul, 0x71B18589ul, 0x06B6B51Ful, 0x9FBFE4A5ul, 0xE8B8D433ul,
  0x7807C9A2ul, 0x0F00F934ul, 0x9609A88Eul, 0xE10E9818ul, 0x7F6A0DBBul, 0x086D3D2Dul, 0x91646C97ul, 0xE6635C01ul,
  0x6B6B51F4ul, 0x1C6C6162ul, 0x856530D8ul, 0xF262004Eul, 0x6C0695EDul, 0x1B01A57Bul, 0x8208F4C1ul, 0xF50FC457ul,
  0x65B0D9C6ul, 0x12B7E950ul, 0x8BBEB8EAul, 0xFCB9887Cul, 0x62DD1DDFul, 0x15DA2D49ul, 0x8CD37CF3ul, 0xFBD44C65ul,
  0x4DB26158ul, 0x3AB551CEul, 0xA3BC0074ul, 0xD4BB30E2ul, 0x4ADFA541ul, 0x3DD895D7ul, 0xA4D1C46Dul, 0xD3D6F4FBul,
  0x4369E96Aul, 0x346ED9FCul, 0xAD678846ul, 0xDA60B8D0ul, 0x44042D73ul, 0x33031DE5ul, 0xAA0A4C5Ful, 0xDD0D7CC9ul,
  0x5005713Cul, 0x270241AAul, 0xBE0B1010ul, 0xC90C2086ul, 0x5768B525ul, 0x206F85B3ul, 0xB966D409ul, 0xCE61E49Ful,
  0x5EDEF90Eul, 0x29D9C998ul, 0xB0D09822ul, 0xC7D7A8B4ul, 0x59B33D17ul, 0x2EB40D81ul, 0xB7BD5C3Bul, 0xC0BA6CADul,
  0xEDB88320ul, 0x9ABFB3B6ul, 0x03B6E20Cul, 0x74B1D29Aul, 0xEAD54739ul, 0x9DD277AFul, 0x04DB2615ul, 0x73DC1683ul,
  0xE3630B12ul, 0x94643B84ul, 0x0D6D6A3Eul, 0x7A6A5AA8ul, 0xE40ECF0Bul, 0x9309FF9Dul, 0x0A00AE27ul, 0x7D079EB1ul,
  0xF00F9344ul, 0x8708A3D2ul, 0x1E01F268ul, 0x6906C2FEul, 0xF762575Dul, 0x806567CBul, 0x196C3671ul, 0x6E6B06E7ul,
  0xFED41B76ul, 0x89D32BE0ul, 0x10DA7A5Aul, 0x67DD4ACCul, 0xF9B9DF6Ful, 0x8EBEEFF9ul, 0x17B7BE43ul, 0x60B08ED5ul,
  0xD6D6A3E8ul, 0xA1D1937Eul, 0x38D8C2C4ul, 0x4FDFF252ul, 0xD1BB67F1ul, 0xA6BC5767ul, 0x3FB506DDul, 0x48B2364Bul,
  0xD80D2BDAul, 0xAF0A1B4Cul, 0x36034AF6ul, 0x41047A60ul, 0xDF60EFC3ul, 0xA867DF55ul, 0x316E8EEFul, 0x4669BE79ul,
  0xCB61B38Cul, 0xBC66831Aul, 0x256FD2A0ul, 0x5268E236ul, 0xCC0C7795ul, 0xBB0B4703ul, 0x220216B9ul, 0x5505262Ful,
  0xC5BA3BBEul, 0xB2BD0B28ul, 0x2BB45A92ul, 0x5CB36A04ul, 0xC2D7FFA7ul, 0xB5D0CF31ul, 0x2CD99E8Bul, 0x5BDEAE1Dul,
  0x9B64C2B0ul, 0xEC63F226ul, 0x756AA39Cul, 0x026D930Aul, 0x9C0906A9ul, 0xEB0E363Ful, 0x72076785ul, 0x05005713ul,
  0x95BF4A82ul, 0xE2B87A14ul, 0x7BB12BAEul, 0x0CB61B38ul, 0x92D28E9Bul, 0xE5D5BE0Dul, 0x7CDCEFB7ul, 0x0BDBDF21ul,
  0x86D3D2D4ul, 0xF1D4E242ul, 0x68DDB3F8ul, 0x1FDA836Eul, 0x81BE16CDul, 0xF6B9265Bul, 0x6FB077E1ul, 0x18B74777ul,
  0x88085AE6ul, 0xFF0F6A70ul, 0x66063BCAul, 0x11010B5Cul, 0x8F659EFFul, 0xF862AE69ul, 0x616BFFD3ul, 0x166CCF45ul,
  0xA00AE278ul, 0xD70DD2EEul, 0x4E048354ul, 0x3903B3C2ul, 0xA7672661ul, 0xD06016F7ul, 0x4969474Dul, 0x3E6E77DBul,
  0xAED16A4Aul, 0xD9D65ADCul, 0x40DF0B66ul, 0x37D83BF0ul, 0xA9BCAE53ul, 0xDEBB9EC5ul, 0x47B2CF7Ful, 0x30B5FFE9ul,
  0xBDBDF21Cul, 0xCABAC28Aul, 0x53B39330ul, 0x24B4A3A6ul, 0xBAD03605ul, 0xCDD70693ul, 0x54DE5729ul, 0x23D967BFul,
  0xB3667A2Eul, 0xC4614AB8ul, 0x5D681B02ul, 0x2A6F2B94ul, 0xB40BBE37ul, 0xC30C8EA1ul, 0x5A05DF1Bul, 0x2D02EF8Dul
};

/*
 * uint32_t crc_32_start( uint32_t crc, unsigned char c );
 *
 * The function crc32_start() return initialize CRC-32 value
 */
static inline uint32_t crc32_start() {
  return CRC_START_32;
}  /* crc32_start */

/*
 * uint32_t crc_32_update( uint32_t crc, unsigned char c );
 *
 * The function crc32_update() calculates a new CRC-32 value based on the
 * previous value of the CRC and the next byte of the data to be checked.
 */
static inline uint32_t crc32_update(uint32_t crc, unsigned char c) {
  return (crc >> 8) ^ crc_tab32[(crc ^ (uint32_t) c) & 0x000000FFul];
}  /* crc32_update */

/*
 * uint32_t crc_32_finish( uint32_t crc, unsigned char c );
 *
 * The function crc32_finish() return finalize CRC-32 value
 */
static inline uint32_t crc32_finish(uint32_t crc) {
  return (crc ^= 0xffffffffL);
}  /* crc32_finish */
