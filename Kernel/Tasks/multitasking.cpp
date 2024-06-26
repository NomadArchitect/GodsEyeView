#include "multitasking.hpp"
#include "../Hardware/Drivers/keyboard.hpp"
#include "../Hardware/Drivers/mouse.hpp"
#include "../Hardware/audio.hpp"
#include "../Net/icmp.hpp"
#include <LibC/network.h>

BitArray<MAX_PIDS> pid_bitmap;

Task::Task(char* task_name, uint32_t eip, int privilege_level, int parent)
{
    if (strlen(task_name) > 20)
        task_name = "unknown";

    memset(arguments, 0, 500);
    memset(working_directory, 0, MAX_PATH_SIZE);
    memcpy(name, task_name, strlen(task_name));
    memset(stack, 0, sizeof(stack));
    name[strlen(task_name)] = '\0';

    cpustate = (cpu_state*)(stack + sizeof(stack) - sizeof(cpu_state));
    cpustate->eax = 0;
    cpustate->ebx = 0;
    cpustate->ecx = 0;
    cpustate->edx = 0;
    cpustate->esi = 0;
    cpustate->edi = 0;
    cpustate->ebp = 0;
    cpustate->esp = 0;
    cpustate->ss = 0;
    cpustate->eip = eip;
    cpustate->cs = GDT_CODE_DATA_SEGMENT;
    cpustate->eflags = 0x202;
    this->parent = parent;

    pid = pid_bitmap.find_unset();
    pid_bitmap.bit_set(pid);

    tty = &local_tty;
    process_group = pid;
    if (parent != -1) {
        has_inherited_tty = true;
        tty = TM->task(parent)->tty;
        process_group = TM->task(parent)->process_group;
    }

    execute = 0;
    state = 0;
    privilege = privilege_level;
}

Task::~Task()
{
    if (is_executable) {
        PMM->free_pages(loaded_executable.memory.physical_address, loaded_executable.memory.size);
        kfree(loaded_executable.program_data);
    }

    for (uint32_t i = 0; i < sockets.size(); i++)
        destroy_socket(i + 1);

    for (uint32_t i = 0; i < allocated_memory.size(); i++)
        PMM->free_pages(allocated_memory.at(i).physical_address, allocated_memory.at(i).size);
}

void Task::executable(executable_t exec)
{
    is_executable = true;
    loaded_executable = exec;
    cpustate->eip = exec.eip;
}

int8_t Task::notify(int signal)
{
    switch (signal) {
    case SIGILL:
        suicide(SIGILL);
        break;
    case SIGTERM:
        suicide(SIGTERM);
        break;
    case SIGQUIT:
        suicide(SIGQUIT);
        break;
    case SIGKILL:
        suicide(SIGKILL);
        break;
    case SIGHUP:
        suicide(SIGHUP);
        break;
    case SIGINT:
        suicide(SIGINT);
        break;
    }
    return 0;
}

void Task::suicide(int error)
{
    state = error;
    quantum = 0;
    TM->task_has_died();
    TM->yield();
}

void Task::disown()
{
    tty = &local_tty;
    process_group = pid;
}

int Task::setsid()
{
    process_group = pid;
    return pid;
}

int Task::chdir(char* dir)
{
    if (dir[0] == '/' && dir[1] == '\0') {
        working_directory[0] = '/';
        working_directory[1] = '\0';
        return 0;
    }

    char dir_path[MAX_PATH_SIZE];
    fs_entry_t entries[1];
    memset(dir_path, 0, MAX_PATH_SIZE);
    strcat(dir_path, working_directory);
    strcat(dir_path, dir);
    path_resolver(dir_path, true);

    if (VFS->is_virtual_directory(dir_path))
        return -1;

    if (VFS->list_directory(dir_path, entries, 1) == 0)
        return -1;

    strcpy(working_directory, dir_path);
    return 0;
}

void Task::cwd(char* buffer)
{
    strcpy(buffer, working_directory);
}

int Task::become_tty_master()
{
    if (!has_inherited_tty)
        return 1;

    tty = &local_tty;
    has_inherited_tty = false;
    return 0;
}

int Task::poll(pollfd* pollfds, uint32_t npolls, int timeout)
{
    if (npolls >= sizeof(polls) / sizeof(pollfd))
        return -1;

    sleeping = -SLEEP_WAIT_POLL;
    poll_sleeping = timeout;
    if (!poll_sleeping)
        poll_sleeping = -1;
    quantum = 0;

    for (uint32_t i = 0; i < npolls; i++)
        polls[i] = pollfds[i];

    num_poll = npolls;
    TM->test_poll();
    TM->yield();
    return npolls;
}

void Task::will_spawn_with_parent(uint8_t toggle)
{
    spawn_with_parent = toggle;
}

int Task::nice(int inc)
{
    priority += inc;
    if (priority > MAX_PRIORITIES)
        priority = MAX_PRIORITIES;
    if (priority < 1) {
        priority = 1;
        return 1;
    }
    return 0;
}

void Task::wake_from_poll()
{
    num_poll = 0;
    sleeping = 0;
}

int Task::destroy_socket(int sockfd)
{
    int index = sockfd - 1;
    if (sockets.at(index) == 0)
        return -1;
    ipv4_socket_t* socket = sockets.at(index);
    if (socket->type == NET_PROTOCOL_UDP) {
        UDP::close(socket->udp_socket);
        Pipe::destroy(socket->udp_socket->receive_pipe);
        kfree(socket->udp_socket);
    }
    if (socket->type == NET_PROTOCOL_TCP) {
        TCP::close(socket->tcp_socket);
        Pipe::destroy(socket->tcp_socket->receive_pipe);
        kfree(socket->tcp_socket);
    }
    kfree(sockets.at(index));
    sockets[index] = 0;
    return 0;
}

bool Task::socket_has_data(ipv4_socket_t* socket)
{
    if (socket->type == NET_PROTOCOL_UDP) {
        if (socket->udp_socket->receive_pipe->size)
            return true;
    }

    if (socket->type == NET_PROTOCOL_TCP) {
        if (socket->tcp_socket->state == CLOSED)
            return true;
        if (socket->tcp_socket->receive_pipe->size)
            return true;
    }

    if (socket->type == NET_PROTOCOL_ICMP) {
        if (ICMP::has_pong(socket->remote_ip))
            return true;
    }

    return false;
}

void Task::test_poll()
{
    for (uint32_t i = 0; i < num_poll; i++) {
        if (polls[i].events & POLLINS) {
            if (socket_has_data(sockets[polls[i].fd - 1]))
                return wake_from_poll();
        }

        if (polls[i].events & POLLIN) {
            if ((polls[i].fd == DEV_KEYBOARD_FD) && (KeyboardDriver::active->has_unread_event()))
                return wake_from_poll();
            if ((polls[i].fd == DEV_MOUSE_FD) && (MouseDriver::active->has_unread_event()))
                return wake_from_poll();
            if ((polls[i].fd == 1) && (tty->stdout_size() > 0))
                return wake_from_poll();
            if ((polls[i].fd == 0) && (tty->is_reading_stdin()))
                return wake_from_poll();
            if (VFS->size(polls[i].fd) > 0)
                return wake_from_poll();
        }

        if (polls[i].events & POLLOUT) {
            if ((polls[i].fd == DEV_AUDIO_FD) && (!AUDIO->is_playing()))
                return wake_from_poll();
        }
    }
}

void Task::process_mmap(memory_region_t region)
{
    if (allocated_memory.size() >= MAX_MEMORY_REGIONS)
        klog("Kernel lost track of task %s allocations", name);
    allocated_memory.append(region);
}

void Task::process_munmap(memory_region_t region)
{
    for (uint32_t i = 0; i < allocated_memory.size(); i++) {
        if (allocated_memory.at(i).virtual_address == region.virtual_address) {
            allocated_memory.remove_at(i);
            break;
        }
    }
}

int Task::socket(uint8_t type)
{
    ipv4_socket_t* socket = (ipv4_socket_t*)kmalloc(sizeof(ipv4_socket_t));
    memset(socket, 0, sizeof(ipv4_socket_t));
    socket->type = type;

    if (socket->type == NET_PROTOCOL_TCP) {
        socket->tcp_socket = (tcp_socket_t*)kmalloc(sizeof(tcp_socket_t));
        memset(socket->tcp_socket, 0, sizeof(tcp_socket_t));
    }

    if (socket->type == NET_PROTOCOL_UDP) {
        socket->udp_socket = (udp_socket_t*)kmalloc(sizeof(udp_socket_t));
        memset(socket->udp_socket, 0, sizeof(udp_socket_t));
    }

    if (sockets.is_full()) {
        klog("Max sockets reached for process %d", pid);
        return -1;
    }

    sockets.append(socket);
    return sockets.size();
}

void Task::sleep(int sleeping_modifier)
{
    sleeping = sleeping_modifier;
    quantum = 0;
    TM->yield();
}

TaskManager::TaskManager()
{
    pid_bitmap.bit_set(0);
    active = this;
}

TaskManager* TM = 0;
TaskManager::~TaskManager()
{
}

void TaskManager::yield()
{
    /* NOTE: Remove a tick from the counter since
     * the counter assumes that we are running
     * at PIT frequency (int $0x20 PIT interrupt) */
    if (current_ticks)
        current_ticks--;
    asm volatile("int $0x20");
}

inline Task* TaskManager::task_at(uint32_t index)
{
    Task* t = tasks.at(index);
    if (t == 0)
        PANIC("TM failed during 'task_at(index)' out of range");
    return t;
}

bool TaskManager::add_task(Task* task)
{
    /* The first task in 'tasks' should always be the idle task */
    if (tasks.size() == 0 && strcmp(task->get_name(), "idle") == 0) {
        task->priority = MAX_PRIORITIES + 1;
        task->quantum = 0;
        tasks.append(task);
        return true;
    }

    if (tasks.is_full())
        return false;

    bool is_initialized = (is_running != 0);
    is_running = 0;
    tasks.append(task);
    if (is_initialized)
        is_running = 1;
    return true;
}

bool TaskManager::append_tasks(int count, ...)
{
    va_list list;
    va_start(list, count);

    for (int i = 0; i < count; i++)
        add_task(va_arg(list, Task*));
    va_end(list);
    return true;
}

uint32_t TaskManager::used_task_time()
{
    return running_task_time;
}

void TaskManager::kill()
{
    task_at(current_task)->suicide(SIGTERM);
}

Task* TaskManager::task(int pid)
{
    for (uint32_t i = 0; i < tasks.size(); i++)
        if (task_at(i)->pid == pid)
            return task_at(i);
    return 0;
}

file_table_t* TaskManager::file_table()
{
    if (testing_poll_task == -1)
        return task_at(current_task)->get_file_table();
    return task_at(testing_poll_task)->get_file_table();
}

ipv4_socket_t** TaskManager::sockets()
{
    return task_at(current_task)->sockets.elements();
}

void TaskManager::task_count_info(int* sleeping, int* zombie, int* polling)
{
    *sleeping = 0;
    *zombie = 0;
    *polling = 0;

    for (uint32_t i = 0; i < tasks.size(); i++) {
        if (task_at(i)->state != ALIVE)
            (*zombie)++;
        else if (task_at(i)->sleeping < 0)
            (*polling)++;
        else if (task_at(i)->sleeping > 0)
            (*sleeping)++;
    }
}

int8_t TaskManager::send_signal(int pid, int sig)
{
    /* TODO: Implement process groups */
    if (pid == 0) {
        for (uint32_t i = 0; i < tasks.size(); i++)
            if ((task_at(i)->process_group == task_at(current_task)->pid) && (current_task != i))
                task_at(i)->notify(sig);
        return 0;
    }

    Task* receiver = task(pid);
    if (receiver == 0)
        return -1;
    if (receiver->privilege > task_at(current_task)->privilege)
        return -1;
    return receiver->notify(sig);
}

int TaskManager::poll(pollfd* pollfds, uint32_t npolls, int timeout)
{
    int ticks = 0;
    if (timeout > 0)
        ticks = current_ticks + timeout;
    return task_at(current_task)->poll(pollfds, npolls, ticks);
}

void TaskManager::sleep(uint32_t ticks)
{
    task_at(current_task)->sleep(current_ticks + ticks);
}

void TaskManager::test_poll()
{
    for (uint32_t i = 0; i < tasks.size(); i++) {
        if (task_at(i)->sleeping == -SLEEP_WAIT_POLL) {
            testing_poll_task = i;
            task_at(i)->test_poll();
        }

        if ((task_at(i)->sleeping == -SLEEP_WAIT_STDIN) && task_at(i)->tty->should_wake_stdin())
            task_at(i)->wake_from_poll();
    }
    testing_poll_task = -1;
}

int TaskManager::waitpid(int pid)
{
    Task* child = task(pid);
    if (child == 0)
        return -1;

    child->wake_pid_on_exit = task_at(current_task)->get_pid();
    task_at(current_task)->sleeping = -SLEEP_WAIT_WAKE;
    task_at(current_task)->quantum = 0;
    return pid;
}

int TaskManager::spawn(char* file, char** args, uint8_t argc)
{
    int fd = VFS->open(file);
    if (fd < 0)
        return -E_INVALIDFD;

    int size = VFS->size(fd);
    uint8_t* elfdata = (uint8_t*)kmalloc(size);
    VFS->read(fd, elfdata, size);
    VFS->close(fd);

    executable_t exec = Loader::load->exec(elfdata);
    exec.program_data = elfdata;

    if (!exec.valid) {
        kfree(elfdata);
        return -E_INVALIDEXEC;
    }

    int parent = (current_task != -1 && task()->spawn_with_parent) ? task()->get_pid() : -1;
    Task* child = new Task(file, 0, 0, parent);
    if (child == 0) {
        kfree(elfdata);
        return -E_UNKNOWN;
    }

    child->executable(exec);

    if (current_task < tasks.size() && current_task >= 0) {
        strcpy(child->working_directory, task_at(current_task)->working_directory);
    } else {
        child->working_directory[0] = '\0';
    }

    if (args && argc) {
        int args_size = MAX_SIZE_ARGS;

        for (uint32_t i = 0; i < argc; i++) {
            args_size -= strlen(args[i]);
            if (args_size <= 0) {
                kfree(elfdata);
                return -E_OVERFLOW;
            }
            if ((i == argc - 1) && args[i][0] == '&' && args[i][1] == '\0')
                child->disown();

            strcat(child->arguments, args[i]);
            strcat(child->arguments, " ");
        }
    }

    child->cpustate->ecx = (uint32_t)argc;
    child->cpustate->edx = (uint32_t)child->arguments;
    add_task(child);
    return child->pid;
}

void TaskManager::kill_zombie_tasks()
{
    if (!check_kill)
        return;

    for (uint32_t i = 0; i < tasks.size(); i++) {
        if (task_at(i)->state == ALIVE)
            continue;

        Task* zombie = task_at(i);

        if (zombie->wake_pid_on_exit) {
            Task* task_to_wake = task(zombie->wake_pid_on_exit);
            if (task_to_wake)
                task_to_wake->wake();
        }

        /* TODO: Find a faster method */
        pid_bitmap.bit_clear(zombie->get_pid());
        if (!zombie->has_inherited_tty) {
            for (uint32_t c = 0; c < tasks.size(); c++) {
                Task* child = task_at(c);
                if (child->tty == zombie->tty)
                    child->tty = &child->local_tty;
            }
        }

        delete task_at(i);
        tasks.remove_at(i);
        i--;
    }

    check_kill = false;
}

void TaskManager::pick_next_task()
{
    current_task++;
    scheduler_checked_tasks++;

    if (scheduler_checked_tasks > tasks.size()) {
        current_task = 0;
        return;
    }

    if (current_task >= tasks.size())
        current_task = 0;

    if (task_at(current_task)->priority != scheduler_priority)
        pick_next_task();

    if (task_at(current_task)->state != ALIVE)
        pick_next_task();

    if (task_at(current_task)->sleeping < 0) {
        if (task_at(current_task)->poll_sleeping > 0 && current_ticks >= task_at(current_task)->poll_sleeping)
            task_at(current_task)->wake_from_poll();
        else
            pick_next_task();
    }

    if (current_ticks >= task_at(current_task)->sleeping)
        task_at(current_task)->sleeping = 0;
    else
        pick_next_task();
}

cpu_state* TaskManager::schedule(cpu_state* cpustate)
{
    current_ticks++;

    if (current_task >= 0)
        task_at(current_task)->cpustate = cpustate;

    if ((tasks.size() <= 0) || (is_running == 0))
        return cpustate;

    if (current_task >= 0 && task_at(current_task)->quantum > 0) {
        task_at(current_task)->quantum--;
        return task_at(current_task)->cpustate;
    }

    tasks[current_task]->quantum = PROCESS_QUANTUM;
    kill_zombie_tasks();

    uint32_t last_task = current_task;
    uint32_t last_priority = scheduler_priority;
    scheduler_priority = 0;

    for (; scheduler_priority <= MAX_PRIORITIES + 1; scheduler_priority++) {
        current_task = (scheduler_priority >= last_priority) ? last_task : 0;
        scheduler_checked_tasks = 0;
        pick_next_task();

        if (current_task != 0)
            break;
    }

    if (task_at(current_task)->priority != (MAX_PRIORITIES + 1))
        running_task_time += task_at(current_task)->quantum;

    return task_at(current_task)->cpustate;
}
