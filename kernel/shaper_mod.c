#include "linux/gfp_types.h"
#include "linux/llist.h"
#include "linux/printk.h"
#include "linux/spinlock_types.h"
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

MODULE_LICENSE("GPL");

// глобальные переменные ################
static u64 load_time;
static u64 start_time;
static struct tracepoint* found_swith = NULL;
static struct tracepoint* found_wakeup = NULL;
static struct work_struct event_work;


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

static DEFINE_MUTEX(sleeping_tasks_mutex);


// отложенный обработчик для парсв llist в основной циклический список
static void pars_event_llist_in_sleeping_tasks(struct work_struct* event_work) {
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
        if(!sleep_task){
          break;
        } else {
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
}



// обработчик ухода в сон
static void probe_sched_switch(void* data, bool preempt, struct task_struct* prev, struct task_struct* next, unsigned int prev_state) {
  // переменная которая хранит адрес структуры типа sleep_event
  struct sleep_event* new_event;
  // главное условие если процес сам выщел(хранит тру если вытеснели поэтому
  // отрицаес до фолз), и проверяем есть ли бит D у потока
  if (!preempt && (prev_state & TASK_UNINTERRUPTIBLE)) {
    // выделение памяти,  с флагом "без сна"
    new_event = kzalloc(sizeof(*new_event), GFP_ATOMIC);
    if (!new_event) {
      return;
    }
    // получаем данные в структуру
    new_event->pid = prev->pid;
    new_event->type = SLEEP_YES;
    new_event->time_event_start = ktime_get_ns();
    get_task_comm(new_event->comm, prev);
    // передаем в ллист
    llist_add(&new_event->node, &sleep_event_list);
    schedule_work(&event_work);
  }
}

// обработчик пробуждения
static void probe_sched_wakeup(void* data, struct task_struct* p) {
  // выделяем память
  struct sleep_event* new_event;
  new_event = kzalloc(sizeof(*new_event), GFP_ATOMIC);
  if (!new_event) {
    return;
  }
  // получаем данные в структуру
  new_event->pid = p->pid;
  new_event->type = SLEEP_NO;
  new_event->time_event_start = ktime_get_ns();
  get_task_comm(new_event->comm, p);
  llist_add(&new_event->node, &sleep_event_list);
  schedule_work(&event_work);
}


// вывод инфы о задаче (безполезен, был нужкн для проверки)
static void pr_inf(struct task_struct* task) {
  pr_info("PID: %d, Name: %s\n", task->pid, task->comm);
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
  INIT_WORK(&event_work, pars_event_llist_in_sleeping_tasks);
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
    cancel_work_sync(&event_work);
    pr_err("Error: %d\n", err);
    return err;
  }
  return 0;
}

// конец
static void __exit stop_m(void) {

  tracepoint_probe_unregister(found_swith, probe_sched_switch, NULL);
  tracepoint_probe_unregister(found_wakeup, probe_sched_wakeup, NULL);
  tracepoint_synchronize_unregister();
  cancel_work_sync(&event_work);
  struct llist_node *node;
  struct llist_node *next;
  struct sleep_event *event;
  node = llist_del_all(&sleep_event_list);
  while (node != NULL) {
    next = node->next;
    event = llist_entry(node, struct sleep_event, node);
    kfree(event);
    node = next;
  }

  printk(KERN_INFO "exeit\n");
}


module_init(start_m);
module_exit(stop_m);