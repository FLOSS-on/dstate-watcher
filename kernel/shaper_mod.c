/* ========================================================================
 * СОДЕРЖАНИЕ ФАЙЛА (сверху вниз, порядок важен: C читает файл сверху вниз)
 *
 *  1. Заголовки (#include)
 *  2. Лицензия и константы
 *  3. Типы: enum и структуры
 *  4. Глобальные переменные
 *  5. Буферы событий по процессорам (alloc_fifos / free_fifos)
 *  6. Вывод в /proc (seq_file)
 *  7. Отложенная работа (work): события -> список sleeping_tasks
 *  8. Пробы (вызываются планировщиком): sched_switch и sched_wakeup
 *  9. Поиск tracepoint'ов (find_tp)
 * 10. Запуск модуля (start_m) и остановка (stop_m)
 * 11. Точки входа модуля
 * ======================================================================== */


/* ========================================================================
 * 1. ЗАГОЛОВКИ
 * ======================================================================== */
#include "asm-generic/errno-base.h"
#include "linux/bitops.h"
#include "linux/cpumask.h"
#include "linux/gfp_types.h"
#include "linux/jiffies.h"
#include "linux/kasan.h"
#include "linux/limits.h"
#include "linux/llist.h"
#include "linux/percpu-defs.h"
#include "linux/printk.h"
#include "linux/stddef.h"
#include "linux/timekeeping.h"
#include "linux/tracepoint-defs.h"
#include "linux/types.h"
#include "linux/workqueue_types.h"
#include <linux/cleanup.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/bitmap.h>
#include <linux/kfifo.h>


/* ========================================================================
 * 2. ЛИЦЕНЗИЯ И КОНСТАНТЫ
 * ======================================================================== */
MODULE_LICENSE("GPL");
#define MAX_PID_DEFINE 4194304
#define EVENTS_PER_CPU 1024


/* ========================================================================
 * 3. ТИПЫ: enum и структуры
 * ======================================================================== */

// структураы #############
// приравнивание числам значений для удобной работы
enum sleep_event_type {
  SLEEP_YES = 0,
  SLEEP_NO = 1
};
// ивентовая структура для быстрой обработки
struct sleep_event {
  enum sleep_event_type type;
  pid_t pid;
  char comm[TASK_COMM_LEN];
  u64 time_event_start;
};
// запись для полноценого списка
struct sleeping_task {
  pid_t pid;
  char comm[TASK_COMM_LEN];
  u64 sleep_start;
  // механизм связанного списка
  struct list_head list;

};

// функция для for_each_kernel_tracepoint нужна для получения адреса шед свича
// для того чтобы потом зарагестровать по его адресу функцию
// tracepoint_probe_register
struct tp_lookup{
  const char *name;
  struct tracepoint *tp;
};
// буфер событий одного процессора (используется ниже в event_bufs)
struct cpu_event_buf{
  DECLARE_KFIFO_PTR(fifo, struct sleep_event);
};


/* ========================================================================
 * 4. ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
 * ======================================================================== */
// --- итоговый список спящих задач (его показывает /proc) ---
// объявление циклического связанного списка для основного списка
static LIST_HEAD(sleeping_tasks);
// блокировка
static DEFINE_MUTEX(sleeping_tasks_mutex);

// --- защита проб и быстрый фильтр ---
//отложенное выполнение
static DEFINE_PER_CPU(int, in_sched_probe);
static unsigned long *my_bit_map;

// --- новые буферы событий, по одному на процессор ---
static DEFINE_PER_CPU(struct cpu_event_buf, event_bufs);

// --- tracepoint'ы, отложенная работа, папка в /proc ---
// глобальные переменные ################
static struct tracepoint* found_swith = NULL;
static struct tracepoint* found_wakeup = NULL;
static struct delayed_work event_work;
static struct proc_dir_entry *my_dir;


/* ========================================================================
 * 5. БУФЕРЫ СОБЫТИЙ ПО ПРОЦЕССОРАМ
 * ======================================================================== */
static void free_fifos(void){
  int cpu;

  for_each_possible_cpu(cpu){
    kfifo_free(&per_cpu_ptr(&event_bufs, cpu)->fifo);
  }
}
static int alloc_fifos(void){
  int cpu;
  int err;
  for_each_possible_cpu(cpu){
    err = kfifo_alloc(&per_cpu_ptr(&event_bufs, cpu)->fifo, EVENTS_PER_CPU, GFP_KERNEL);
    if (err){
      free_fifos();
      return err;
    }
  }
  return 0;
}


/* ========================================================================
 * 6. ВЫВОД В /proc (seq_file)
 * ======================================================================== */
static void *my_seq_start(struct seq_file *s, loff_t *pos){
  mutex_lock(&sleeping_tasks_mutex);
  return seq_list_start(&sleeping_tasks, *pos);
}

static void *my_seq_next(struct seq_file *s, void *v, loff_t *pos){
    return seq_list_next(v, &sleeping_tasks, pos);
}

static void my_seq_stop(struct seq_file *s, void *v){
  mutex_unlock(&sleeping_tasks_mutex);

}

static int my_seq_show(struct seq_file *s, void *v){
    struct sleeping_task *task = list_entry(v, struct sleeping_task, list);
    seq_printf(s, "PID:%d|Comm:%-16s|Start:%llu\n", task->pid, task->comm, task->sleep_start);
    return 0;
}

static const struct seq_operations my_seq_ops = {
  .start = my_seq_start,
  .next = my_seq_next,
  .stop = my_seq_stop,
  .show = my_seq_show,
};

static int my_proc_open(struct inode *inode, struct file *file){
  return seq_open(file, &my_seq_ops);
}

static const struct proc_ops my_proc_ops = {
    .proc_open    = my_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = seq_release,
};


/* ========================================================================
 * 7. ОТЛОЖЕННАЯ РАБОТА (work)
 * ======================================================================== */

// отложенный обработчик для парсв llist в основной циклический список
static void pars_event_llist_in_sleeping_tasks(struct work_struct* work) {
  int cpu;
  int best_cpu;
  u64 min_time;
  struct sleep_event temp_ev;
  mutex_lock(&sleeping_tasks_mutex);
  while (1) {
    best_cpu = -1;
    min_time = U64_MAX;
    for_each_possible_cpu(cpu){
      if(!(kfifo_peek(&per_cpu_ptr(&event_bufs, cpu)->fifo, &temp_ev))){
        continue;
      }
      if (!(temp_ev.time_event_start<min_time)){
        continue;
      }
      min_time = temp_ev.time_event_start;
      best_cpu = cpu;
    }
    if (best_cpu == -1){
        break;
    }
    if(!(kfifo_get(&per_cpu_ptr(&event_bufs, best_cpu)->fifo, &temp_ev))){
      continue;
    }
    switch (temp_ev.type) {
      case SLEEP_YES:{
        //хрнаит структуру основного списка
        struct sleeping_task* sleep_task;
        // выделяем память под структуру
        sleep_task = kzalloc(sizeof(*sleep_task), GFP_ATOMIC);
        if(sleep_task){
          sleep_task->pid = temp_ev.pid;
          sleep_task->sleep_start = temp_ev.time_event_start;
          strscpy(sleep_task->comm , temp_ev.comm, TASK_COMM_LEN);
          list_add_tail(&sleep_task->list, &sleeping_tasks);
        }
        break;
      }
      case SLEEP_NO:{
        // хранит основную структуру
        struct sleeping_task* task_search_values;
        //следующий адрес структуры
        struct sleeping_task* next_task_adrress;
        list_for_each_entry_safe(task_search_values, next_task_adrress, &sleeping_tasks, list){
          if(temp_ev.pid == task_search_values->pid){
            list_del(&task_search_values->list);
            kfree(task_search_values);
            break;
          }
        }
        break;
      }
    }

  }
  mutex_unlock(&sleeping_tasks_mutex);
  schedule_delayed_work(&event_work, msecs_to_jiffies(50));
}


/* ========================================================================
 * 8. ПРОБЫ (вызываются планировщиком)
 * ======================================================================== */

// обработчик ухода в сон
static void probe_sched_switch(void* data, bool preempt, struct task_struct* prev, struct task_struct* next, unsigned int prev_state) {
  struct sleep_event new_event;
  if (preempt || !(prev_state & TASK_UNINTERRUPTIBLE)){
    return;
  }
  if (this_cpu_inc_return(in_sched_probe) > 1) {
    goto out_dec;
  }
  new_event.pid = prev->pid;
  new_event.type = SLEEP_YES;
  new_event.time_event_start = ktime_get_ns();
  get_task_comm(new_event.comm, prev);
  int err;
  err = kfifo_put(&this_cpu_ptr(&event_bufs)->fifo, new_event);
  if(err){
      set_bit(prev->pid, my_bit_map);

  }


out_dec:
    this_cpu_dec(in_sched_probe);
}

// обработчик пробуждения
static void probe_sched_wakeup(void* data, struct task_struct* p) {
    struct sleep_event new_event;
  if(!test_and_clear_bit(p->pid, my_bit_map)){
    return;
  }

  if(this_cpu_inc_return(in_sched_probe)>1){
    goto out_dec;
  }

  new_event.pid = p->pid;
  new_event.type = SLEEP_NO;
  new_event.time_event_start = ktime_get_ns();

  kfifo_put(&this_cpu_ptr(&event_bufs)->fifo, new_event);

out_dec:
  this_cpu_dec(in_sched_probe);
}


/* ========================================================================
 * 9. ПОИСК TRACEPOINT'ОВ
 * ======================================================================== */
static void find_tp(struct tracepoint *tp, void *priv){
  struct tp_lookup *l = priv;
  if (strcmp(tp->name, l->name) == 0){
    l->tp = tp;
  }
}


/* ========================================================================
 * 10. ЗАПУСК И ОСТАНОВКА МОДУЛЯ
 * ======================================================================== */

// старт
static int __init start_m(void) {
  struct tp_lookup sw = { .name = "sched_switch" };
	struct tp_lookup wk = { .name = "sched_wakeup" };
  int err;
  my_bit_map = bitmap_zalloc(MAX_PID_DEFINE, GFP_KERNEL);
  if(!my_bit_map){
    return -ENOMEM;
  }
  err = alloc_fifos();
  if(err){
    goto err_bitmap;
  }
  for_each_kernel_tracepoint(find_tp, &sw);
  for_each_kernel_tracepoint(find_tp, &wk);
  if (!sw.tp || !wk.tp) {
    pr_err("свичь не найден ");
    err = -ENOENT;
    goto err_fifos;
  }
  found_swith = sw.tp;
  found_wakeup = wk.tp;
  my_dir = proc_mkdir("my_dir", NULL);
  if(!my_dir){
    err = -ENOMEM;
    goto err_fifos;
  }
  if(!proc_create("dstat_info", 0400, my_dir, &my_proc_ops)){
    err = -ENOMEM;
    goto err_proc;
  }
  INIT_DELAYED_WORK(&event_work, pars_event_llist_in_sleeping_tasks);
  err = tracepoint_probe_register(found_swith, probe_sched_switch, NULL);
  if(err){
    goto err_proc;
  }
  err = tracepoint_probe_register(found_wakeup, probe_sched_wakeup, NULL);
  if(err){
    goto err_unreg_switch;
  }
  schedule_delayed_work(&event_work, msecs_to_jiffies(100));
  pr_info("hello\n");
  return 0;
  err_unreg_switch:
    tracepoint_probe_unregister(found_swith, probe_sched_switch, NULL);
	  tracepoint_synchronize_unregister();
  err_proc:
    proc_remove(my_dir);
  err_fifos:
    free_fifos();
  err_bitmap:
    bitmap_free(my_bit_map);
    return err;
}

// конец
static void __exit stop_m(void) {

  struct sleeping_task *node_main_struct, *next_main_struct;
  proc_remove(my_dir);
  tracepoint_probe_unregister(found_swith, probe_sched_switch, NULL);
  tracepoint_probe_unregister(found_wakeup, probe_sched_wakeup, NULL);
  tracepoint_synchronize_unregister();
  cancel_delayed_work_sync(&event_work);
  /////////////////////////
  free_fifos();
////////////////////////////////

  mutex_lock(&sleeping_tasks_mutex);
  list_for_each_entry_safe(node_main_struct, next_main_struct, &sleeping_tasks, list){
    list_del(&node_main_struct->list);
    kfree(node_main_struct);
  }
  mutex_unlock(&sleeping_tasks_mutex);
  bitmap_free(my_bit_map);
  printk(KERN_INFO "exet\n");
}


/* ========================================================================
 * 11. ТОЧКИ ВХОДА МОДУЛЯ
 * ======================================================================== */
module_init(start_m);
module_exit(stop_m);