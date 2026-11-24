#include "asm-generic/errno-base.h"
#include "linux/gfp_types.h"
#include "linux/jiffies.h"
#include "linux/kasan.h"
#include "linux/llist.h"
#include "linux/percpu-defs.h"
#include "linux/printk.h"
#include "linux/stddef.h"
#include "linux/timekeeping.h"
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

MODULE_LICENSE("GPL");

// глобальные переменные ################
static struct tracepoint* found_swith = NULL;
static struct tracepoint* found_wakeup = NULL;
static struct delayed_work event_work;


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
  // хранит следующий элемент
  struct llist_node node;
};
// запись для полноценого списка
struct sleeping_task {
  pid_t pid;
  char comm[TASK_COMM_LEN];
  u64 sleep_start;
  u64 sleep_end;
  // механизм связанного списка
  struct list_head list;

};


// объявление циклического не связанного списка
static LLIST_HEAD(sleep_event_list);
// объявление циклического связанного списка для основного списка
static LIST_HEAD(sleeping_tasks);
// блокировка
static DEFINE_MUTEX(sleeping_tasks_mutex);
//отложенное выполнение
static DEFINE_PER_CPU(int, in_sched_probe);


// отложенный обработчик для парсв llist в основной циклический список
static void pars_event_llist_in_sleeping_tasks(struct work_struct* work) {
  // перменная указатель на next
  struct llist_node*  next_events;
  // переменная укаазатель на поля моего sleep_event
  struct sleep_event* pols_list;
  // перменная для хранения следующего в списке, так как если не сохранить то мы его потеряем
  struct llist_node* next;
  // передаем адес в перменную
  next_events = llist_del_all(&sleep_event_list);
  next_events = llist_reverse_order(next_events);

  // обработчи
  while (next_events != NULL) {
    // сохраняем некст чтобв его не потерть
    next = next_events->next;
    // получаем все поля структуры из списка
    pols_list = llist_entry(next_events, struct sleep_event , node);
    // сортировка событий
    switch (pols_list->type) {
      case SLEEP_YES:{
        //хрнаит структуру основного списка
        struct sleeping_task* sleep_task;
        // выделяем память под структуру
        sleep_task = kzalloc(sizeof(*sleep_task), GFP_KERNEL);
        if(sleep_task){
          //копируем данные из вторичного листа в струтуру основного
          sleep_task->pid = pols_list->pid;
          sleep_task->sleep_start = pols_list->time_event_start;
          strscpy(sleep_task->comm , pols_list->comm, TASK_COMM_LEN);
          // блокируем от одновременного взаимодействия
          mutex_lock(&sleeping_tasks_mutex);
          //добовляем в основной список
          list_add_tail(&sleep_task->list, &sleeping_tasks);
          // разблокируем
          mutex_unlock(&sleeping_tasks_mutex);
        }
        break;
      }
      case SLEEP_NO:{
        // хранит основную структуру
        struct sleeping_task* task_search_values;
        //следующий адрес структуры
        struct sleeping_task* next_task_adrress;
        //блокировка
        mutex_lock(&sleeping_tasks_mutex);
        //удаление из основного списка
        list_for_each_entry_safe(task_search_values, next_task_adrress, &sleeping_tasks, list){
          if(pols_list->pid == task_search_values->pid){
            list_del(&task_search_values->list);
            kfree(task_search_values);
            break;
          }
        }
        // разблокируем
        mutex_unlock(&sleeping_tasks_mutex);
        break;
      }
    }
    // освобождаем вторичную структуру ивентов
    kfree(pols_list);
    //переходим к слкдующему ивенту
    next_events = next;
  }
  schedule_delayed_work(&event_work, msecs_to_jiffies(100));
}



// обработчик ухода в сон
static void probe_sched_switch(void* data, bool preempt, struct task_struct* prev, struct task_struct* next, unsigned int prev_state) {
  struct sleep_event* new_event;
  if (preempt || !(prev_state & TASK_UNINTERRUPTIBLE)){
    return;
  }

    new_event = kzalloc(sizeof(*new_event), GFP_ATOMIC);
    if (!new_event) {
      goto out_dec;
    }
    // получаем данные в структуру
    new_event->pid = prev->pid;
    new_event->type = SLEEP_YES;
    new_event->time_event_start = ktime_get_ns();
    get_task_comm(new_event->comm, prev);
    // передаем в ллист
    llist_add(&new_event->node, &sleep_event_list);
  out_dec:
    this_cpu_dec(in_sched_probe);
}

// обработчик пробуждения
static void probe_sched_wakeup(void* data, struct task_struct* p) {
  // выделяем память
  struct sleep_event* new_event;
  if(this_cpu_inc_return(in_sched_probe)>1){
    goto out_dec;
  }

  new_event = kzalloc(sizeof(*new_event), GFP_ATOMIC);
  if (!new_event) {
    goto out_dec;
  }
  // получаем данные в структуру
  new_event->pid = p->pid;
  new_event->type = SLEEP_NO;
  new_event->time_event_start = ktime_get_ns();
  get_task_comm(new_event->comm, p);
  llist_add(&new_event->node, &sleep_event_list);
out_dec:
  this_cpu_dec(in_sched_probe);
}

// функция для for_each_kernel_tracepoint нужна для получения адреса шед свича
// для того чтобы потом зарагестровать по его адресу функцию
// tracepoint_probe_register
static void find_tracepoint_switch(struct tracepoint* tp, void* priv) {
  if (strcmp(tp->name, "sched_switch") == 0) {
    struct tracepoint** found_ptr = (struct tracepoint** )priv;
    *found_ptr = tp;
  }
}


// получает адрес sched_wakeup
static void find_tracepoint_wakeup(struct tracepoint* tp, void* priv) {
  if (strcmp(tp->name, "sched_wakeup") == 0) {
    struct tracepoint **found_ptr = (struct tracepoint **)priv;
    *found_ptr = tp;
  }
}


// старт
static int __init start_m(void) {
  // вывод в логи привет
  printk(KERN_INFO "hello\n");
  // получаем время загрузки можуля

  // получаем адрес структуры для регестрации в шед_свич
  // пириберааем траспоинты передовая функцию для поиска адреса шед_свича
  for_each_kernel_tracepoint(find_tracepoint_switch, &found_swith);
  if (!found_swith) {
    pr_err("свичь не найден ");
    return -ENOENT;
  }
  // получаем адрес структуры для регестрации в шед_свич вецкам
  // пириберааем траспоинты передовая функцию для поиска адреса шед_свича
  for_each_kernel_tracepoint(find_tracepoint_wakeup, &found_wakeup);
  if (!found_wakeup) {
    pr_err("свичь не найден ");
    return -ENOENT;
  }
  // регестрируем функцию в отложенный обработчик
  INIT_DELAYED_WORK(&event_work, pars_event_llist_in_sleeping_tasks);
  schedule_delayed_work(&event_work, msecs_to_jiffies(100));
  // ##############################################
  // регистрация обработчикков
  int err;
  err = tracepoint_probe_register(found_swith, probe_sched_switch, NULL);
  if(err){
    pr_err("Error: %d\n", err);
    return err;
  }
  err = tracepoint_probe_register(found_wakeup, probe_sched_wakeup, NULL);
  if(err){
    tracepoint_probe_unregister(found_swith, probe_sched_switch, NULL);
    tracepoint_synchronize_unregister();
    cancel_delayed_work_sync(&event_work);
    pr_err("Error: %d\n", err);
    return err;
  }
  return 0;
}

// конец
static void __exit stop_m(void) {
  struct llist_node *node;
  struct llist_node *next;
  struct sleep_event *event;
  struct sleeping_task *node_main_struct, *next_main_struct;

  tracepoint_probe_unregister(found_swith, probe_sched_switch, NULL);
  tracepoint_probe_unregister(found_wakeup, probe_sched_wakeup, NULL);
  tracepoint_synchronize_unregister();
  cancel_delayed_work_sync(&event_work);

  node = llist_del_all(&sleep_event_list);
  while (node != NULL) {
    next = node->next;
    event = llist_entry(node, struct sleep_event, node);
    kfree(event);
    node = next;
  }


  mutex_lock(&sleeping_tasks_mutex);
  list_for_each_entry_safe(node_main_struct, next_main_struct, &sleeping_tasks, list){
    list_del(&node_main_struct->list);
    kfree(node_main_struct);
  }
  mutex_unlock(&sleeping_tasks_mutex);
  printk(KERN_INFO "exeit\n");
}


module_init(start_m);
module_exit(stop_m);